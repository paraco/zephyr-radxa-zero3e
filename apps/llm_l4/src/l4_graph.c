/*
 * L4 그래프 — 호스트와 Zephyr 가 공유한다. l4_graph.h 참고.
 *
 * 입력은 전부 **첨자의 함수**다. 난수를 쓰면 양쪽에서 같은 값을 보장할 수
 * 없고, 0 으로 채우면 커널이 틀려도 통과한다(L1 에서 같은 이유로 그렇게 했다).
 */
#include "l4_graph.h"

#include <string.h>
#include <stdlib.h>

#include "ggml.h"
#include "ggml-cpu.h"

/* f32 입력 — 양수·음수·0 을 고루 지나간다 */
static float in_f32(int i)
{
	return (float)((i * 7919) % 31 - 15) * 0.125f;
}

/*
 * Q4_0 가중치의 **생바이트**. 임의 바이트도 유효한 Q4_0 블록이다
 * (d 가 fp16, qs 가 니블 16개). 다만 d 가 NaN/Inf 가 되면 결과가 NaN 이라
 * 비교가 무의미해지므로 **d 는 고정하고 니블만 바꾼다.**
 *
 * 블록 = 18 B: d fp16 | qs[16]
 */
static void fill_q4_0(uint8_t *p, int nblocks)
{
	for (int b = 0; b < nblocks; b++) {
		uint8_t *blk = p + (size_t)b * 18;
		/* fp16 값들: 0.0625, -0.125, 0.25, -0.5 가 순환 */
		static const uint16_t ds[4] = { 0x2C00, 0xB000, 0x3400, 0xB800 };
		uint16_t d = ds[b & 3];

		memcpy(blk, &d, 2);
		for (int j = 0; j < 16; j++) {
			blk[2 + j] = (uint8_t)((b * 37 + j * 11) & 0xFF);
		}
	}
}

int l4_run(struct l4_results *out)
{
	memset(out, 0, sizeof(*out));
#ifdef __ARM_FEATURE_DOTPROD
	out->dotprod = 1;
#endif
#ifdef __ARM_FEATURE_MATMUL_INT8
	out->i8mm = 1;
#endif

	struct ggml_init_params ip = {
		.mem_size   = 4u * 1024 * 1024,
		.mem_buffer = NULL,
		.no_alloc   = false,
	};
	struct ggml_context *ctx = ggml_init(ip);

	if (!ctx) {
		return -1;
	}

	/* ── ① f32 행렬곱.  a[K,M] × b[K,N] → [M,N] */
	struct ggml_tensor *a = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L4_K, L4_M);
	struct ggml_tensor *b = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L4_K, L4_N);

	for (int i = 0; i < L4_K * L4_M; i++) { ((float *)a->data)[i] = in_f32(i); }
	for (int i = 0; i < L4_K * L4_N; i++) { ((float *)b->data)[i] = in_f32(i + 1000); }
	struct ggml_tensor *mm = ggml_mul_mat(ctx, a, b);

	/*
	 * ── ② Q4_0 행렬곱 — **진짜 추론 경로다.**
	 * ggml 이 b 를 Q8_0 으로 양자화해 ggml_vec_dot_q4_0_q8_0 를 부르고,
	 * 그 안에서 __ARM_FEATURE_DOTPROD 가 있으면 vdotq_s32 를 쓴다.
	 * 즉 이 한 줄이 dotprod 경로를 지나간다.
	 */
	struct ggml_tensor *aq = ggml_new_tensor_2d(ctx, GGML_TYPE_Q4_0, L4_K, L4_M);
	fill_q4_0((uint8_t *)aq->data, (L4_K / 32) * L4_M);
	struct ggml_tensor *bq = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, L4_K, L4_N);

	for (int i = 0; i < L4_K * L4_N; i++) { ((float *)bq->data)[i] = in_f32(i + 1000); }
	struct ggml_tensor *mmq = ggml_mul_mat(ctx, aq, bq);

	/* ── ③ RMSNorm */
	struct ggml_tensor *r = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, L4_RMS_NE);

	for (int i = 0; i < L4_RMS_NE; i++) { ((float *)r->data)[i] = in_f32(i + 2000); }
	struct ggml_tensor *rms = ggml_rms_norm(ctx, r, 1e-5f);

	/* ── ④ SiLU */
	struct ggml_tensor *s = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, L4_SILU_NE);

	for (int i = 0; i < L4_SILU_NE; i++) { ((float *)s->data)[i] = in_f32(i + 3000); }
	struct ggml_tensor *silu = ggml_silu(ctx, s);

	/* ── ⑤ RoPE.  a[head_dim, n_head, n_tok], b = 위치 i32[n_tok] */
	struct ggml_tensor *q = ggml_new_tensor_3d(ctx, GGML_TYPE_F32,
						   L4_HEAD_DIM, L4_N_HEAD, L4_N_TOK);
	for (int i = 0; i < L4_ROPE_NE; i++) { ((float *)q->data)[i] = in_f32(i + 4000); }
	struct ggml_tensor *pos = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, L4_N_TOK);

	for (int i = 0; i < L4_N_TOK; i++) { ((int32_t *)pos->data)[i] = i; }
	struct ggml_tensor *rope = ggml_rope(ctx, q, pos, L4_HEAD_DIM, 0);

	/* ── 계산. 스레드 1개 — 재현성을 위해 누산 순서를 고정한다. */
	struct ggml_cgraph *gf = ggml_new_graph(ctx);

	ggml_build_forward_expand(gf, mm);
	ggml_build_forward_expand(gf, mmq);
	ggml_build_forward_expand(gf, rms);
	ggml_build_forward_expand(gf, silu);
	ggml_build_forward_expand(gf, rope);

	enum ggml_status st = ggml_graph_compute_with_ctx(ctx, gf, 1);

	if (st != GGML_STATUS_SUCCESS) {
		ggml_free(ctx);
		return -2 - (int)st;
	}

	memcpy(out->mm_f32, mm->data, sizeof(out->mm_f32));
	memcpy(out->mm_q4_0, mmq->data, sizeof(out->mm_q4_0));
	memcpy(out->rms, rms->data, sizeof(out->rms));
	memcpy(out->silu, silu->data, sizeof(out->silu));
	memcpy(out->rope, rope->data, sizeof(out->rope));

	ggml_free(ctx);
	return 0;
}
