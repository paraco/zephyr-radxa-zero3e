/*
 * L5a 그래프 — **호스트와 Zephyr 가 이 파일을 공유한다.**
 *
 * 치수는 Qwen3.5-0.8B 의 **실측값**이다 (models/ 의 GGUF 를 gguf_dump 로 읽음):
 *   ssm.state_size 128 · ssm.group_count 16 · ssm.time_step_rank 16
 *   ssm.conv_kernel 4 · embedding_length 1024
 *   head_k_dim = head_v_dim = 128 · n_k_heads = n_v_heads = 16
 *   key_dim = value_dim = 128*16 = 2048 · conv_dim = 2048*2 + 2048 = 6144
 */
#ifndef L5_GRAPH_H
#define L5_GRAPH_H

#include <stdint.h>

#define L5_S		128	/* ssm.state_size — head_k_dim = head_v_dim */
#define L5_H		16	/* n_k_heads == n_v_heads */
#define L5_D_CONV	4	/* ssm.conv_kernel */
#define L5_CONV_DIM	6144	/* key_dim*2 + value_dim */
#define L5_N_EMBD	1024

/* 골든 헤더가 너무 커지지 않게 큰 출력은 **보폭 표본**으로 비교한다.
 * 전체는 CRC32 로 한 번 더 본다 — 비트 일치면 CRC 가 같다. */
#define L5_CONV_STRIDE	6
#define L5_CONV_SAMP	(L5_CONV_DIM / L5_CONV_STRIDE)		/* 1024 */
#define L5_O_N		(L5_S * L5_H)				/* 2048 */
#define L5_STATE_N	(L5_S * L5_S * L5_H)			/* 262144 */
#define L5_STATE_STRIDE	256
#define L5_STATE_SAMP	(L5_STATE_N / L5_STATE_STRIDE)		/* 1024 */

struct l5_results {
	/* L5a-1 인과 컨볼루션 */
	float    conv[L5_CONV_SAMP];
	uint32_t conv_crc;
	/* L5a-2 감쇠·게이트 계수 */
	float    beta[L5_H];		/* sigmoid(ssm_beta @ x) */
	float    a_softplus[L5_H];	/* softplus(ssm_alpha @ x + ssm_dt) */
	float    gate[L5_H];		/* a_softplus * ssm_a   (ssm_a = -exp(A_log)) */
	/* L5a-3 델타 규칙 순환 1스텝 */
	float    o[L5_O_N];
	float    state[L5_STATE_SAMP];
	uint32_t state_crc;
	/* L5a-4 ssm_norm + 게이트 */
	float    normg[L5_O_N];
	/* L5a-5 축 배치 — 중간 텐서의 ne[] 를 그대로 기록한다 */
	int64_t  ne_q_perm[4];
	int64_t  ne_sk[4];
	int64_t  ne_kd[4];
	int64_t  ne_o[4];
	/* 커널 기능 집합 */
	int      dotprod;
	int      i8mm;
};

/* 0 = 성공. 실패하면 음수. */
int l5_run(struct l5_results *out);

#endif /* L5_GRAPH_H */
