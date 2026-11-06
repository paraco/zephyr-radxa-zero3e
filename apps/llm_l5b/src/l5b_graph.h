/*
 * L5b 그래프 — **호스트와 Zephyr 가 이 파일을 공유한다.**
 *
 * 치수는 Qwen3.5-0.8B 의 **실측값**이다(gguf_dump):
 *   attention.key_length = value_length = 256   (head_dim)
 *   attention.head_count = 8 · head_count_kv = 2   (GQA 4:1)
 *   rope.dimension_count = 64                   (**부분 RoPE** — 256 중 64만 회전)
 *   rope.dimension_sections = [11, 11, 10, 0]    (mRoPE 구간)
 *   rope.freq_base = 1e7 · embedding_length = 1024
 */
#ifndef L5B_GRAPH_H
#define L5B_GRAPH_H

#include <stdint.h>

#define L5B_HEAD_DIM	256
#define L5B_N_HEAD	8
#define L5B_N_HEAD_KV	2
#define L5B_N_TOK	4
#define L5B_N_EMBD	1024
#define L5B_N_ROT	64
#define L5B_FREQ_BASE	1.0e7f

#define L5B_Q_N		(L5B_HEAD_DIM * L5B_N_HEAD * L5B_N_TOK)		/* 8192 */
#define L5B_K_N		(L5B_HEAD_DIM * L5B_N_HEAD_KV * L5B_N_TOK)	/* 2048 */
#define L5B_KQ_N	(L5B_N_TOK * L5B_N_TOK * L5B_N_HEAD)		/* 128 */
#define L5B_ATT_N	(L5B_HEAD_DIM * L5B_N_HEAD * L5B_N_TOK)		/* 8192 */
#define L5B_OUT_N	(L5B_N_EMBD * L5B_N_TOK)			/* 4096 */

/* 큰 출력은 보폭 표본으로 비교하고 전체는 CRC32 로 한 번 더 본다 */
#define L5B_Q_STRIDE	8
#define L5B_Q_SAMP	(L5B_Q_N / L5B_Q_STRIDE)	/* 1024 */
#define L5B_K_STRIDE	2
#define L5B_K_SAMP	(L5B_K_N / L5B_K_STRIDE)	/* 1024 */
#define L5B_ATT_STRIDE	8
#define L5B_ATT_SAMP	(L5B_ATT_N / L5B_ATT_STRIDE)	/* 1024 */
#define L5B_OUT_STRIDE	4
#define L5B_OUT_SAMP	(L5B_OUT_N / L5B_OUT_STRIDE)	/* 1024 */

struct l5b_results {
	float    q_norm[L5B_Q_SAMP];	uint32_t q_norm_crc;
	float    k_norm[L5B_K_SAMP];	uint32_t k_norm_crc;
	float    q_rope[L5B_Q_SAMP];	uint32_t q_rope_crc;
	float    k_rope[L5B_K_SAMP];	uint32_t k_rope_crc;
	float    kq[L5B_KQ_N];		/* 전체 — 작다 */
	float    att[L5B_ATT_SAMP];	uint32_t att_crc;
	float    gate[L5B_ATT_SAMP];	uint32_t gate_crc;
	/* sigmoid 를 지난 뒤 — 층 출력의 오차가 어디서 들어오는지 가린다 */
	float    gated[L5B_ATT_SAMP];	uint32_t gated_crc;
	float    out[L5B_OUT_SAMP];	uint32_t out_crc;
	/* 축 배치 */
	int64_t  ne_q_view[4];
	int64_t  ne_gate[4];
	int64_t  ne_kq[4];
	int64_t  ne_att2d[4];
	int64_t  ne_out[4];
	/* 부분 RoPE 증거 — 회전된 앞 64 과 안 건드린 뒤쪽이 구분되는가 */
	int      rope_changed_lo;	/* [0,64) 중 RoPE 로 값이 바뀐 개수 */
	int      rope_changed_hi;	/* [64,256) 중 바뀐 개수 — **0 이어야 한다** */
	int      dotprod;
	int      i8mm;
};

int l5b_run(struct l5b_results *out);

#endif /* L5B_GRAPH_H */
