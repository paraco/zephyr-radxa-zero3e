/*
 * L5b 그래프 — 호스트와 Zephyr 가 공유한다. l5b_graph.h 참고.
 *
 * Qwen3.5 의 **Gated Attention 층**을 llama.cpp 의 연산 순서 그대로 재현한다.
 * 가중치만 합성이다.
 *
 *   출처: src/models/qwen35.cpp:263-331      (층 전체 순서)
 *         src/llama-graph.cpp:3084-3141      (어텐션 본체, 비-flash 경로)
 *         src/llama-graph.cpp:1642-1658      (mRoPE 는 ggml_rope_multi)
 *
 * ⚠ 합성 가중치다. "ggml 이 이 층을 호스트와 똑같이 계산하는가" 만 가린다.
 */
#include "l5b_graph.h"

#include <string.h>
#include <math.h>

#include "ggml.h"
#include "ggml-cpu.h"

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

static void sample(float *dst, const float *src, int n, int stride)
{
	for (int i = 0; i < n; i++) {
		dst[i] = src[(size_t)i * stride];
	}
}

int l5b_run(struct l5b_results *out)
{
	memset(out, 0, sizeof(*out));
#ifdef __ARM_FEATURE_DOTPROD
	out->dotprod = 1;
#endif
#ifdef __ARM_FEATURE_MATMUL_INT8
	out->i8mm = 1;
#endif

	struct ggml_init_params ip = {
		.mem_size   = 16u * 1024 * 1024,
		.mem_buffer = NULL,
		.no_alloc   = false,
	};
	struct ggml_context *ctx = ggml_init(ip);

	if (!ctx) {
		return -1;
	}
	struct ggml_cgraph *gf = ggml_new_graph(ctx);

	const int HD = L5B_HEAD_DIM, NH = L5B_N_HEAD, NKV = L5B_N_HEAD_KV, NT = L5B_N_TOK;

	/*
	 * ── 결합 QG 투영의 결과.
	 * ⚠ Q 와 게이트가 **헤드마다 교차 배치**돼 있다. 헤드당 블록이
	 *   2*head_dim 이고 앞 절반이 Q, 뒤 절반이 게이트다(qwen35.cpp:283-302).
	 *   앞 절반 전체가 Q, 뒤 절반 전체가 게이트인 것이 **아니다** —
	 *   그렇게 읽으면 형태는 맞고 값이 섞인다.
	 */
	struct ggml_tensor *qg = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, HD * 2 * NH, NT);

	fill(qg, 1);
	const size_t es = ggml_element_size(qg);
	struct ggml_tensor *q = ggml_view_3d(ctx, qg, HD, NH, NT,
					     es * HD * 2, es * HD * 2 * NH, 0);
	struct ggml_tensor *gate = ggml_view_3d(ctx, qg, HD, NH, NT,
						es * HD * 2, es * HD * 2 * NH, es * HD);

	save_ne(out->ne_q_view, q);

	struct ggml_tensor *kraw = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, HD * NKV, NT);
	struct ggml_tensor *vt = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, NT, HD, NKV);

	fill(kraw, 2);
	fill(vt, 3);	/* KV 캐시의 v 는 전치돼 있다 (v_trans) */

	/* ── Q/K RMSNorm (가중치 곱까지) */
	struct ggml_tensor *qn_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, HD);
	struct ggml_tensor *kn_w = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, HD);

	fill(qn_w, 4);
	fill(kn_w, 5);
	struct ggml_tensor *qn = ggml_mul(ctx, ggml_rms_norm(ctx, q, 1e-6f), qn_w);
	struct ggml_tensor *k3 = ggml_reshape_3d(ctx, kraw, HD, NKV, NT);
	struct ggml_tensor *kn = ggml_mul(ctx, ggml_rms_norm(ctx, k3, 1e-6f), kn_w);

	struct ggml_tensor *gate2 = ggml_cont_2d(ctx, gate, HD * NH, NT);

	save_ne(out->ne_gate, gate2);

	/*
	 * ── mRoPE.  llama-graph.cpp:1647-1653 이 ggml_rope_multi 를 부른다.
	 * ⚠ n_rot=64 인데 head_dim=256 이다 — **앞 64 차원만 회전**하고 나머지
	 *   192 는 그대로다. 그것을 아래에서 증거로 확인한다.
	 * ⚠ mRoPE 는 **토큰당 위치 id 4개**를 요구한다 (ggml.c: a->ne[2]*4 == b->ne[0]).
	 */
	struct ggml_tensor *pos = ggml_new_tensor_1d(ctx, GGML_TYPE_I32, NT * 4);

	for (int i = 0; i < NT * 4; i++) {
		((int32_t *)pos->data)[i] = i % NT;
	}
	int sections[GGML_MROPE_SECTIONS] = { 11, 11, 10, 0 };
	struct ggml_tensor *qr = ggml_rope_multi(ctx, qn, pos, NULL, L5B_N_ROT, sections,
						 GGML_ROPE_TYPE_MROPE, 0, L5B_FREQ_BASE,
						 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	struct ggml_tensor *kr = ggml_rope_multi(ctx, kn, pos, NULL, L5B_N_ROT, sections,
						 GGML_ROPE_TYPE_MROPE, 0, L5B_FREQ_BASE,
						 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);

	/* ── 어텐션 본체 (비-flash 경로, llama-graph.cpp:3084-3141) */
	struct ggml_tensor *qp = ggml_permute(ctx, qr, 0, 2, 1, 3);	/* [HD, NT, NH] */
	struct ggml_tensor *kp = ggml_permute(ctx, kr, 0, 2, 1, 3);	/* [HD, NT, NKV] */
	struct ggml_tensor *kq = ggml_mul_mat(ctx, kp, qp);		/* GQA 전파 */

	save_ne(out->ne_kq, kq);

	/* 인과 마스크. soft_max_ext 규약: ne0 == kq->ne0, ne1 >= kq->ne1, 연속 */
	struct ggml_tensor *mask = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, NT, NT);

	for (int j = 0; j < NT; j++) {
		for (int i = 0; i < NT; i++) {
			((float *)mask->data)[j * NT + i] = (i <= j) ? 0.0f : -INFINITY;
		}
	}
	const float kq_scale = 1.0f / sqrtf((float)HD);
	struct ggml_tensor *kqs = ggml_soft_max_ext(ctx, kq, mask, kq_scale, 0.0f);
	struct ggml_tensor *kqv = ggml_mul_mat(ctx, vt, kqs);		/* [HD, NT, NH] */
	struct ggml_tensor *att = ggml_permute(ctx, kqv, 0, 2, 1, 3);	/* [HD, NH, NT] */

	att = ggml_cont_2d(ctx, att, HD * NH, NT);
	save_ne(out->ne_att2d, att);

	/* ── 게이트: **sigmoid** 다 (qwen35.cpp:322-325). L5a-4 의 silu 와 다르다. */
	struct ggml_tensor *gated = ggml_mul(ctx, att, ggml_sigmoid(ctx, gate2));

	/* ── 출력 투영 */
	struct ggml_tensor *wo = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, HD * NH, L5B_N_EMBD);

	fill(wo, 6);
	struct ggml_tensor *o = ggml_mul_mat(ctx, wo, gated);

	save_ne(out->ne_out, o);

	/* RoPE 전/후를 비교하려면 qn 도 결과로 남겨야 한다 */
	struct ggml_tensor *qn_c = ggml_cont(ctx, qn);
	struct ggml_tensor *kn_c = ggml_cont(ctx, kn);
	struct ggml_tensor *qr_c = ggml_cont(ctx, qr);
	struct ggml_tensor *kr_c = ggml_cont(ctx, kr);

	ggml_build_forward_expand(gf, qn_c);
	ggml_build_forward_expand(gf, kn_c);
	ggml_build_forward_expand(gf, qr_c);
	ggml_build_forward_expand(gf, kr_c);
	ggml_build_forward_expand(gf, kqs);
	ggml_build_forward_expand(gf, att);
	ggml_build_forward_expand(gf, gate2);
	ggml_build_forward_expand(gf, gated);
	ggml_build_forward_expand(gf, o);

	enum ggml_status st = ggml_graph_compute_with_ctx(ctx, gf, 1);

	if (st != GGML_STATUS_SUCCESS) {
		ggml_free(ctx);
		return -2 - (int)st;
	}

	const float *pqn = (const float *)qn_c->data;
	const float *pqr = (const float *)qr_c->data;

	sample(out->q_norm, pqn, L5B_Q_SAMP, L5B_Q_STRIDE);
	sample(out->q_rope, pqr, L5B_Q_SAMP, L5B_Q_STRIDE);
	out->q_norm_crc = crc32_of(pqn, (size_t)L5B_Q_N * 4);
	out->q_rope_crc = crc32_of(pqr, (size_t)L5B_Q_N * 4);
	sample(out->k_norm, (const float *)kn_c->data, L5B_K_SAMP, L5B_K_STRIDE);
	sample(out->k_rope, (const float *)kr_c->data, L5B_K_SAMP, L5B_K_STRIDE);
	out->k_norm_crc = crc32_of(kn_c->data, (size_t)L5B_K_N * 4);
	out->k_rope_crc = crc32_of(kr_c->data, (size_t)L5B_K_N * 4);

	memcpy(out->kq, kqs->data, sizeof(out->kq));
	sample(out->att, (const float *)att->data, L5B_ATT_SAMP, L5B_ATT_STRIDE);
	out->att_crc = crc32_of(att->data, (size_t)L5B_ATT_N * 4);
	sample(out->gate, (const float *)gate2->data, L5B_ATT_SAMP, L5B_ATT_STRIDE);
	out->gate_crc = crc32_of(gate2->data, (size_t)L5B_ATT_N * 4);
	sample(out->gated, (const float *)gated->data, L5B_ATT_SAMP, L5B_ATT_STRIDE);
	out->gated_crc = crc32_of(gated->data, (size_t)L5B_ATT_N * 4);
	sample(out->out, (const float *)o->data, L5B_OUT_SAMP, L5B_OUT_STRIDE);
	out->out_crc = crc32_of(o->data, (size_t)L5B_OUT_N * 4);

	/*
	 * ── 부분 RoPE 증거.
	 * RoPE 전후가 달라진 자리를 **모든 토큰·헤드에 걸쳐** 센다.
	 * [0,64) 은 여러 개 달라져야 하고 **[64,256) 은 0 이어야 한다.**
	 * 이 숫자가 없으면 "부분 RoPE 가 맞게 걸렸는지" 를 말할 수 없다.
	 *
	 * ⚠ 토큰 0 만 보면 안 된다 — 위치가 0 이면 theta=0 이라 cos=1·sin=0 이고
	 *   회전이 **항등**이다. 처음에 토큰 0 만 봐서 lo=0 hi=0 이 나왔고
	 *   "RoPE 가 안 걸렸다" 로 오독했다.
	 *
	 * 배치는 [HD, NH, NT] 연속이므로 첨자 = t*(HD*NH) + h*HD + d 다.
	 * mRoPE 의 위치 배열은 구간별이다 — p_t=pos[i2], p_h=pos[i2+ne2]
	 * (ggml-cpu/ops.cpp:6403-6404).
	 */
	for (int t = 0; t < NT; t++) {
		for (int h = 0; h < NH; h++) {
			const size_t base = (size_t)t * HD * NH + (size_t)h * HD;

			for (int d = 0; d < HD; d++) {
				if (pqn[base + d] != pqr[base + d]) {
					if (d < L5B_N_ROT) {
						out->rope_changed_lo++;
					} else {
						out->rope_changed_hi++;
					}
				}
			}
		}
	}

	ggml_free(ctx);
	return 0;
}
