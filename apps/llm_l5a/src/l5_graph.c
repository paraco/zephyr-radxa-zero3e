/*
 * L5a 그래프 — 호스트와 Zephyr 가 공유한다. l5_graph.h 참고.
 *
 * 무엇을 하는가: Qwen3.5 의 **Gated DeltaNet 층**을 구성하는 네 조각을
 * llama.cpp 의 연산 순서 **그대로** 재현한다. 가중치만 합성이다.
 *
 *   L5a-1  ggml_ssm_conv                      — 인과 컨볼루션
 *   L5a-2  beta/alpha/dt/A 로 만드는 계수      — src/models/qwen35.cpp:357-372
 *   L5a-3  델타 규칙 순환 1스텝                 — src/models/delta-net-base.cpp:286-368
 *   L5a-4  ssm_norm + silu 게이트              — src/models/qwen35.cpp:252-261
 *   L5a-5  축 배치                            — 중간 텐서의 ne[] 를 기록
 *
 * ⚠ 합성 가중치다. 이 칸이 가리는 것은 "ggml 이 이 층을 호스트와 똑같이
 *   계산하는가" 이고, "llama.cpp 의 qwen35 경로가 옳은가" 는 **아니다**.
 *   후자는 실제 가중치 + 호스트 중간 텐서가 필요하다.
 */
#include "l5_graph.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "ggml.h"
#include "ggml-cpu.h"

/* 입력·가중치는 전부 **첨자의 함수**다. 난수는 양쪽에서 같음을 보장할 수 없고
 * 0 으로 채우면 계산이 틀려도 통과한다. */
static float fn(int i, int salt)
{
	return (float)(((i * 7919 + salt * 104729) % 251) - 125) * 0.008f;
}

static void fill(struct ggml_tensor *t, int salt)
{
	float *p = (float *)t->data;
	int64_t n = ggml_nelements(t);

	for (int64_t i = 0; i < n; i++) {
		p[i] = fn((int)i, salt);
	}
}

static void save_ne(int64_t *dst, const struct ggml_tensor *t)
{
	for (int i = 0; i < 4; i++) {
		dst[i] = t->ne[i];
	}
}

/* crc32(IEEE) — Zephyr 의 crc32_ieee 와 zlib.crc32 와 같다.
 * 호스트에서도 같은 코드를 쓰려고 여기 둔다(libz 의존을 피한다). */
static uint32_t crc32_of(const void *data, size_t len)
{
	static const uint32_t tab[16] = {
		0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
		0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
		0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
		0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
	};
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = ~0u;

	for (size_t i = 0; i < len; i++) {
		crc = (crc >> 4) ^ tab[(crc ^ p[i]) & 0x0f];
		crc = (crc >> 4) ^ tab[(crc ^ ((uint32_t)p[i] >> 4)) & 0x0f];
	}
	return ~crc;
}

int l5_run(struct l5_results *out)
{
	memset(out, 0, sizeof(*out));
#ifdef __ARM_FEATURE_DOTPROD
	out->dotprod = 1;
#endif
#ifdef __ARM_FEATURE_MATMUL_INT8
	out->i8mm = 1;
#endif

	struct ggml_init_params ip = {
		.mem_size   = 48u * 1024 * 1024,
		.mem_buffer = NULL,
		.no_alloc   = false,
	};
	struct ggml_context *ctx = ggml_init(ip);

	if (!ctx) {
		return -1;
	}
	struct ggml_cgraph *gf = ggml_new_graph(ctx);

	/* ─────────────────────────────────────────────── L5a-1 인과 컨볼루션
	 * ggml_ssm_conv 규약 (ggml.c:5774-5791):
	 *   c  = [d_conv, d_inner]            커널
	 *   sx = [d_conv-1+n_t, d_inner, n_s] 입력 (앞쪽이 과거 상태)
	 *   결과 = [d_inner, n_t, n_s]
	 * 토큰 1개(n_t=1)이므로 sx 의 첫 축이 d_conv 다.
	 */
	struct ggml_tensor *conv_w = ggml_new_tensor_2d(ctx, GGML_TYPE_F32,
							L5_D_CONV, L5_CONV_DIM);
	struct ggml_tensor *conv_x = ggml_new_tensor_3d(ctx, GGML_TYPE_F32,
							L5_D_CONV, L5_CONV_DIM, 1);
	fill(conv_w, 1);
	fill(conv_x, 2);
	struct ggml_tensor *conv_o = ggml_ssm_conv(ctx, conv_x, conv_w);

	/* ──────────────────────────────────── L5a-2 감쇠·게이트 계수
	 * qwen35.cpp:357-372 그대로:
	 *   beta  = sigmoid(ssm_beta @ x)
	 *   alpha = ssm_alpha @ x
	 *   gate  = softplus(alpha + ssm_dt) * ssm_a
	 *
	 * ⚠ 공개된 변환 함정이 **ssm_a** 다. 텐서 이름이 SSM_A_NOSCAN 이고
	 *   파일에는 **이미 -exp(A_log) 로 변환된 값**이 들어 있다(qwen35.cpp:372
	 *   의 주석 "-A_log.exp() * softplus"). 즉 **항상 음수**다.
	 *   여기서 A_log 를 다시 exp 하면 틀리고, 양수를 넣으면 뒤의 exp(gate)
	 *   가 발산한다. 그래서 합성값도 음수로 둔다.
	 */
	struct ggml_tensor *x = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L5_N_EMBD, 1);
	struct ggml_tensor *w_beta = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L5_N_EMBD, L5_H);
	struct ggml_tensor *w_alpha = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L5_N_EMBD, L5_H);
	struct ggml_tensor *ssm_dt = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, L5_H);
	struct ggml_tensor *ssm_a = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, L5_H);

	fill(x, 3);
	fill(w_beta, 4);
	fill(w_alpha, 5);
	fill(ssm_dt, 6);
	for (int i = 0; i < L5_H; i++) {
		((float *)ssm_a->data)[i] = -(0.25f + 0.02f * (float)i);	/* 항상 음수 */
	}

	struct ggml_tensor *beta = ggml_sigmoid(ctx, ggml_mul_mat(ctx, w_beta, x));
	struct ggml_tensor *alpha = ggml_mul_mat(ctx, w_alpha, x);
	struct ggml_tensor *a_sp = ggml_softplus(ctx, ggml_add(ctx, alpha, ssm_dt));
	struct ggml_tensor *gate = ggml_mul(ctx, a_sp, ssm_a);

	/* ───────────────────────── L5a-3 델타 규칙 순환 1스텝
	 * delta-net-base.cpp:286-368 의 연산 순서를 그대로 옮긴다.
	 * 한 줄도 바꾸지 않는다 — 바꾸면 무엇을 시험하는지 알 수 없게 된다.
	 */
	struct ggml_tensor *q = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, L5_S, L5_H, 1, 1);
	struct ggml_tensor *k = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, L5_S, L5_H, 1, 1);
	struct ggml_tensor *v = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, L5_S, L5_H, 1, 1);
	struct ggml_tensor *s = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, L5_S, L5_S, L5_H, 1);

	fill(q, 7);
	fill(k, 8);
	fill(v, 9);
	fill(s, 10);

	/* g, b 는 [1, H, n_tokens, n_seqs] 로 들어간다 (GDA: g->ne[0]==1) */
	struct ggml_tensor *g4 = ggml_reshape_4d(ctx, gate, 1, L5_H, 1, 1);
	struct ggml_tensor *b4 = ggml_reshape_4d(ctx, beta, 1, L5_H, 1, 1);

	const float scale = 1.0f / sqrtf((float)L5_S);
	struct ggml_tensor *qs = ggml_scale(ctx, q, scale);

	struct ggml_tensor *qp = ggml_permute(ctx, qs, 0, 2, 1, 3);
	struct ggml_tensor *kp = ggml_permute(ctx, k, 0, 2, 1, 3);
	struct ggml_tensor *vp = ggml_permute(ctx, v, 0, 2, 1, 3);

	struct ggml_tensor *gg = ggml_reshape_4d(ctx, g4, 1, g4->ne[0], L5_H, 1);
	struct ggml_tensor *bb = ggml_reshape_4d(ctx, b4, 1, 1, L5_H, 1);

	gg = ggml_exp(ctx, gg);
	struct ggml_tensor *s1 = ggml_mul(ctx, s, gg);

	struct ggml_tensor *sk = ggml_sum_rows(ctx, ggml_mul(ctx, s1, kp));
	struct ggml_tensor *d = ggml_mul(ctx, ggml_sub(ctx, vp, ggml_transpose(ctx, sk)), bb);
	struct ggml_tensor *d_t = ggml_transpose(ctx, d);
	struct ggml_tensor *kr = ggml_repeat(ctx, kp, s1);
	struct ggml_tensor *kd = ggml_mul(ctx, kr, d_t);
	struct ggml_tensor *s2 = ggml_add(ctx, s1, kd);
	struct ggml_tensor *o = ggml_sum_rows(ctx, ggml_mul(ctx, s2, qp));

	o = ggml_permute(ctx, o, 2, 0, 1, 3);
	o = ggml_cont(ctx, o);		/* 비교하려면 연속이어야 한다 */

	save_ne(out->ne_q_perm, qp);
	save_ne(out->ne_sk, sk);
	save_ne(out->ne_kd, kd);
	save_ne(out->ne_o, o);

	/* ───────────────────── L5a-4 ssm_norm + silu 게이트
	 * qwen35.cpp:252-261 — build_norm(RMS) 뒤에 **silu** 로 곱한다.
	 * ⚠ sigmoid 가 아니다. 공개된 함정에 RMSNorm 이 들어 있는 이유가
	 *   여기다 — 가중치 곱과 게이트 곱의 순서·함수를 바꾸면 조용히 틀린다.
	 */
	struct ggml_tensor *norm_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, L5_S);
	struct ggml_tensor *z = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L5_S, L5_H);

	fill(norm_w, 11);
	fill(z, 12);
	struct ggml_tensor *o2 = ggml_reshape_2d(ctx, o, L5_S, L5_H);
	struct ggml_tensor *nz = ggml_mul(ctx, ggml_rms_norm(ctx, o2, 1e-6f), norm_w);
	struct ggml_tensor *normg = ggml_mul(ctx, nz, ggml_silu(ctx, z));

	/* ── 계산. 스레드 1개 — 누산 순서를 고정한다. */
	ggml_build_forward_expand(gf, conv_o);
	ggml_build_forward_expand(gf, beta);
	ggml_build_forward_expand(gf, a_sp);
	ggml_build_forward_expand(gf, gate);
	ggml_build_forward_expand(gf, o);
	ggml_build_forward_expand(gf, s2);
	ggml_build_forward_expand(gf, normg);

	enum ggml_status st = ggml_graph_compute_with_ctx(ctx, gf, 1);

	if (st != GGML_STATUS_SUCCESS) {
		ggml_free(ctx);
		return -2 - (int)st;
	}

	const float *cp = (const float *)conv_o->data;

	for (int i = 0; i < L5_CONV_SAMP; i++) {
		out->conv[i] = cp[(size_t)i * L5_CONV_STRIDE];
	}
	out->conv_crc = crc32_of(cp, (size_t)L5_CONV_DIM * sizeof(float));
	out->conv_full = llm_fullsum_of(cp, L5_CONV_DIM);

	memcpy(out->beta, beta->data, sizeof(out->beta));
	memcpy(out->a_softplus, a_sp->data, sizeof(out->a_softplus));
	memcpy(out->gate, gate->data, sizeof(out->gate));
	memcpy(out->o, o->data, sizeof(out->o));
	memcpy(out->normg, normg->data, sizeof(out->normg));

	const float *sp = (const float *)s2->data;

	for (int i = 0; i < L5_STATE_SAMP; i++) {
		out->state[i] = sp[(size_t)i * L5_STATE_STRIDE];
	}
	out->state_crc = crc32_of(sp, (size_t)L5_STATE_N * sizeof(float));
	out->state_full = llm_fullsum_of(sp, L5_STATE_N);

	ggml_free(ctx);
	return 0;
}
