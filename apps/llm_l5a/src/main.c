/*
 * L5a — Gated DeltaNet 층 관문 (합성 가중치).
 *
 * 사다리(docs/llm-hub-test-plan.md)의 L5a 하위 다섯 개를 한 번에 본다.
 * 치수는 Qwen3.5-0.8B 의 실측값이다.
 *
 * ⚠ 이 칸이 가리는 것은 **"ggml 이 이 층을 호스트와 똑같이 계산하는가"** 다.
 *   "llama.cpp 의 qwen35 경로가 옳은가" 는 **아니다** — 그것은 실제 가중치와
 *   호스트 중간 텐서가 필요하다. 혼동하지 않도록 출력에도 적는다.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "l5_graph.h"
#include "l5_golden.h"

/* 계획서의 L5 허용 오차 1e-4 = 100,000 ppb */
#define TOL_PPB		100000ull

static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float ab(float f) { return f < 0 ? -f : f; }

static uint64_t rel_ppb(float got, float want)
{
	float d = ab(got - want);
	float m = ab(want);

	if (d == 0.0f) { return 0; }
	if (m < 1e-30f) { m = 1e-30f; }
	float r = d / m * 1e9f;

	if (r > 1e18f) { return (uint64_t)1e18; }
	return (uint64_t)r;
}

static int check(const char *name, const float *got, const uint32_t *golden, int n)
{
	int nbit = 0;
	uint64_t worst = 0;
	int wi = 0;

	printk("  %-28s %5d개 ... ", name, n);
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
		printk("OK — **비트 일치**\n");
		return 0;
	}
	if (worst <= TOL_PPB) {
		printk("OK — 비트 %d/%d 다름, 최대 %" PRIu64 " ppb\n", nbit, n, worst);
		/*
		 * 통과했어도 최대 오차가 눈에 띄면 **값을 찍는다.**
		 * 상대오차가 큰데 결과가 0 근처면 그것은 상쇄(cancellation)이고
		 * 구현 오류가 아니다. 숫자를 보지 않으면 그 구분을 할 수 없다.
		 */
		if (worst > 1000ull) {
			float want;

			memcpy(&want, &golden[wi], 4);
			printk("      └ 최대 오차 지점 [%d]: 얻음 0x%08x  골든 0x%08x\n",
			       wi, bits(got[wi]), golden[wi]);
			printk("        크기 |골든| 이 다른 원소 중앙값의 몇 배인지 보라 — "
			       "0 근처면 상쇄다\n");
			/* 절대오차와 그 지점의 크기를 정수로 찍는다 (printk 에 float 금지) */
			printk("        |골든| = %u e-9 · |차| = %u e-9\n",
			       (unsigned)(ab(want) * 1e9f), (unsigned)(ab(got[wi] - want) * 1e9f));
		}
		return 0;
	}
	printk("FAIL — 최대 %" PRIu64 " ppb (> %" PRIu64 ") at [%d]\n", worst, TOL_PPB, wi);
	printk("      %*s얻음 0x%08x  골든 0x%08x\n", 6, "", bits(got[wi]), golden[wi]);
	return 1;
}

static int check_ne(const char *name, const int64_t *got, const int64_t *golden)
{
	printk("  %-28s ", name);
	printk("[%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "] ... ",
	       got[0], got[1], got[2], got[3]);
	for (int i = 0; i < 4; i++) {
		if (got[i] != golden[i]) {
			printk("FAIL — 골든 [%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "]\n",
			       golden[0], golden[1], golden[2], golden[3]);
			return 1;
		}
	}
	printk("OK\n");
	return 0;
}

static struct l5_results res;

int main(void)
{
	int fail = 0;

	printk("=== L5a: Gated DeltaNet 층 관문 (합성 가중치) ===\n");
	printk("  치수 실측: S=%d H=%d d_conv=%d conv_dim=%d n_embd=%d\n",
	       L5_S, L5_H, L5_D_CONV, L5_CONV_DIM, L5_N_EMBD);

	int rc = l5_run(&res);

	if (rc != 0) {
		printk("  l5_run 실패: %d\n", rc);
		printk("=== L5a FAIL (실패 1) ===\n");
		return 0;
	}

	printk("  [0] 기능 집합  Zephyr dotprod=%d i8mm=%d / 호스트 %d %d ... ",
	       res.dotprod, res.i8mm, L5_HOST_DOTPROD, L5_HOST_I8MM);
	if (res.dotprod == L5_HOST_DOTPROD && res.i8mm == L5_HOST_I8MM) {
		printk("OK\n");
	} else {
		printk("불일치 — 아래 비교는 다른 커널 경로끼리다\n");
		fail++;
	}

	/* L5a-5 축 배치를 **먼저** 본다. 형태가 어긋나면 값 비교가 무의미하다. */
	printk("  ── L5a-5 축 배치\n");
	fail += check_ne("[1] q permute", res.ne_q_perm, l5_g_ne_q_perm);
	fail += check_ne("[2] sum_rows(s*k)", res.ne_sk, l5_g_ne_sk);
	fail += check_ne("[3] k*d_t", res.ne_kd, l5_g_ne_kd);
	fail += check_ne("[4] 출력 o", res.ne_o, l5_g_ne_o);

	printk("  ── L5a-1 인과 컨볼루션\n");
	fail += check("[5] ssm_conv (보폭 표본)", res.conv, l5_g_conv, L5_CONV_SAMP);
	printk("  %-28s 0x%08x ... ", "[6] ssm_conv 전체 CRC32", res.conv_crc);
	if (res.conv_crc == L5_G_CONV_CRC) {
		printk("OK — 전체가 비트 일치\n");
	} else {
		printk("다름 (골든 0x%08x) — 표본 비교로 판정한다\n", L5_G_CONV_CRC);
	}

	printk("  ── L5a-2 감쇠·게이트 계수\n");
	fail += check("[7] beta = sigmoid(..)", res.beta, l5_g_beta, L5_H);
	fail += check("[8] softplus(alpha+dt)", res.a_softplus, l5_g_a_softplus, L5_H);
	fail += check("[9] gate = sp * ssm_a", res.gate, l5_g_gate, L5_H);

	printk("  ── L5a-3 델타 규칙 순환 1스텝\n");
	fail += check("[10] 출력 o", res.o, l5_g_o, L5_O_N);
	fail += check("[11] 상태 (보폭 표본)", res.state, l5_g_state, L5_STATE_SAMP);
	printk("  %-28s 0x%08x ... ", "[12] 상태 전체 CRC32", res.state_crc);
	if (res.state_crc == L5_G_STATE_CRC) {
		printk("OK — 262,144개 전부 비트 일치\n");
	} else {
		printk("다름 (골든 0x%08x) — 표본 비교로 판정한다\n", L5_G_STATE_CRC);
	}

	printk("  ── L5a-4 ssm_norm + silu 게이트\n");
	fail += check("[13] normg", res.normg, l5_g_normg, L5_O_N);

	if (fail == 0) {
		printk("=== L5a PASS (실패 0) ===\n");
		printk("  ⚠ 합성 가중치다. 'llama.cpp 의 qwen35 경로가 옳은가' 는 아직 못 가렸다.\n");
	} else {
		printk("=== L5a FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
