/*
 * L5b — Gated Attention 층 관문 (합성 가중치).
 *
 * GQA(8:2) + Q/K RMSNorm + **부분 mRoPE(256 중 64)** + 인과 마스크 softmax
 * + sigmoid 게이트 + 출력 투영. 치수는 Qwen3.5-0.8B 실측값이다.
 *
 * ⚠ 합성 가중치다. "ggml 이 이 층을 호스트와 똑같이 계산하는가" 만 가린다.
 *   "llama.cpp 의 qwen35 경로가 옳은가" 는 아니다.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "l5b_graph.h"
#include "l5b_golden.h"

#define TOL_PPB		100000ull	/* 계획서의 L5 허용 오차 1e-4 */

static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float ab(float f) { return f < 0 ? -f : f; }

static uint64_t rel_ppb(float got, float want)
{
	float d = ab(got - want);
	float m = ab(want);

	if (d == 0.0f) { return 0; }
	if (m < 1e-30f) { m = 1e-30f; }
	float r = d / m * 1e9f;

	return (r > 1e18f) ? (uint64_t)1e18 : (uint64_t)r;
}

static int check(const char *name, const float *got, const uint32_t *golden, int n,
		 uint32_t crc, uint32_t crc_golden)
{
	int nbit = 0;
	uint64_t worst = 0;
	int wi = 0;

	printk("  %-22s %4d개 ... ", name, n);
	for (int i = 0; i < n; i++) {
		if (bits(got[i]) != golden[i]) {
			float want;

			memcpy(&want, &golden[i], 4);
			nbit++;
			uint64_t r = rel_ppb(got[i], want);

			if (r > worst) { worst = r; wi = i; }
		}
	}
	if (nbit == 0) {
		if (crc_golden && crc == crc_golden) {
			printk("OK — **전체 비트 일치** (CRC 0x%08x)\n", crc);
		} else if (crc_golden) {
			printk("OK — 표본 비트 일치 (전체 CRC 는 다름)\n");
		} else {
			printk("OK — **비트 일치**\n");
		}
		return 0;
	}
	if (worst <= TOL_PPB) {
		printk("OK — 비트 %d/%d 다름, 최대 %" PRIu64 " ppb\n", nbit, n, worst);
		return 0;
	}
	printk("FAIL — 최대 %" PRIu64 " ppb (> %" PRIu64 ") at [%d]\n", worst, TOL_PPB, wi);
	printk("        얻음 0x%08x  골든 0x%08x\n", bits(got[wi]), golden[wi]);
	return 1;
}

static int check_ne(const char *name, const int64_t *got, const int64_t *g)
{
	printk("  %-22s [%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "] ... ",
	       name, got[0], got[1], got[2], got[3]);
	for (int i = 0; i < 4; i++) {
		if (got[i] != g[i]) {
			printk("FAIL — 골든 [%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "]\n",
			       g[0], g[1], g[2], g[3]);
			return 1;
		}
	}
	printk("OK\n");
	return 0;
}

static struct l5b_results res;

int main(void)
{
	int fail = 0;

	printk("=== L5b: Gated Attention 층 관문 (합성 가중치) ===\n");
	printk("  치수 실측: head_dim=%d n_head=%d n_head_kv=%d (GQA %d:1) n_rot=%d/%d\n",
	       L5B_HEAD_DIM, L5B_N_HEAD, L5B_N_HEAD_KV,
	       L5B_N_HEAD / L5B_N_HEAD_KV, L5B_N_ROT, L5B_HEAD_DIM);

	int rc = l5b_run(&res);

	if (rc != 0) {
		printk("  l5b_run 실패: %d\n", rc);
		printk("=== L5b FAIL (실패 1) ===\n");
		return 0;
	}

	printk("  [0] 기능 집합  dotprod=%d i8mm=%d / 호스트 %d %d ... ",
	       res.dotprod, res.i8mm, L5B_HOST_DOTPROD, L5B_HOST_I8MM);
	if (res.dotprod == L5B_HOST_DOTPROD && res.i8mm == L5B_HOST_I8MM) {
		printk("OK\n");
	} else {
		printk("불일치 — 다른 커널 경로끼리다\n");
		fail++;
	}

	printk("  ── 축 배치 (값보다 먼저 본다)\n");
	fail += check_ne("[1] Q 교차 뷰", res.ne_q_view, l5b_g_ne_q_view);
	fail += check_ne("[2] 게이트 2d", res.ne_gate, l5b_g_ne_gate);
	fail += check_ne("[3] kq", res.ne_kq, l5b_g_ne_kq);
	fail += check_ne("[4] 어텐션 2d", res.ne_att2d, l5b_g_ne_att2d);
	fail += check_ne("[5] 출력", res.ne_out, l5b_g_ne_out);

	/*
	 * 부분 RoPE 증거. head_dim 256 중 n_rot 64 만 회전해야 한다.
	 * hi(= [64,256) 에서 바뀐 개수)가 **0** 이어야 맞다.
	 */
	printk("  ── 부분 RoPE (%d/%d)\n", L5B_N_ROT, L5B_HEAD_DIM);
	printk("  %-22s lo=%d hi=%d (골든 lo=%d hi=%d) ... ",
	       "[6] 회전 범위", res.rope_changed_lo, res.rope_changed_hi,
	       L5B_G_ROPE_LO, L5B_G_ROPE_HI);
	/*
	 * ⚠ lo 를 골든값과 **정확히** 비교하면 안 된다. sin/cos 가 1 ulp 다르면
	 *   회전된 원소 하나가 한쪽에서만 원래 값과 같아져 개수가 ±1 흔들린다.
	 *   그러면 버그가 아닌 것을 쫓게 된다. 실기(다른 libm)에서 더 그렇다.
	 *
	 *   주장을 지탱하는 것은 **hi == 0**(뒤쪽은 건드리지 않았다)이고,
	 *   앞쪽은 **lo > 0**(회전이 실제로 걸렸다)면 충분하다.
	 */
	if (res.rope_changed_hi == 0 && L5B_G_ROPE_HI == 0 && res.rope_changed_lo > 0) {
		printk("OK — 뒤쪽 %d 차원은 그대로다", L5B_HEAD_DIM - L5B_N_ROT);
		if (res.rope_changed_lo != L5B_G_ROPE_LO) {
			printk(" (lo 가 골든과 %d 만큼 다르다 — libm 1 ulp 범위라면 정상)",
			       res.rope_changed_lo - L5B_G_ROPE_LO);
		}
		printk("\n");
	} else {
		printk("FAIL — hi 가 0 이 아니면 부분 RoPE 범위가 틀린 것이다\n");
		fail++;
	}

	printk("  ── 값\n");
	fail += check("[7] Q RMSNorm", res.q_norm, l5b_g_q_norm, L5B_Q_SAMP,
		      res.q_norm_crc, L5B_G_QNORM_CRC);
	fail += check("[8] K RMSNorm", res.k_norm, l5b_g_k_norm, L5B_K_SAMP,
		      res.k_norm_crc, L5B_G_KNORM_CRC);
	fail += check("[9] Q mRoPE", res.q_rope, l5b_g_q_rope, L5B_Q_SAMP,
		      res.q_rope_crc, L5B_G_QROPE_CRC);
	fail += check("[10] K mRoPE", res.k_rope, l5b_g_k_rope, L5B_K_SAMP,
		      res.k_rope_crc, L5B_G_KROPE_CRC);
	fail += check("[11] softmax(kq)", res.kq, l5b_g_kq, L5B_KQ_N, 0, 0);
	fail += check("[12] 어텐션 출력", res.att, l5b_g_att, L5B_ATT_SAMP,
		      res.att_crc, L5B_G_ATT_CRC);
	fail += check("[13] 게이트", res.gate, l5b_g_gate, L5B_ATT_SAMP,
		      res.gate_crc, L5B_G_GATE_CRC);
	/*
	 * [14] 는 sigmoid 를 **지난** 값이다. [13](sigmoid 전)이 비트 일치인데
	 * [14] 가 다르면 오차의 입구가 sigmoid(libm)라는 뜻이다 — 추론이 아니라
	 * 측정으로 가린다.
	 */
	fail += check("[14] sigmoid 곱한 뒤", res.gated, l5b_g_gated, L5B_ATT_SAMP,
		      res.gated_crc, L5B_G_GATED_CRC);
	fail += check("[15] 층 출력 (wo 투영)", res.out, l5b_g_out, L5B_OUT_SAMP,
		      res.out_crc, L5B_G_OUT_CRC);

	if (fail == 0) {
		printk("=== L5b PASS (실패 0) ===\n");
		printk("  ⚠ 합성 가중치다. 'llama.cpp 의 qwen35 경로가 옳은가' 는 아직 못 가렸다.\n");
	} else {
		printk("=== L5b FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
