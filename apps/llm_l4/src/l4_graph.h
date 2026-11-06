/*
 * L4 그래프 — **호스트와 Zephyr 가 이 파일을 공유한다.**
 *
 * 그래프를 양쪽에 따로 쓰면 전사 오류가 생기고, 그걸 "커널이 다르다" 로
 * 오진한다. 한 파일을 양쪽에서 컴파일하면 차이는 플랫폼뿐이다.
 */
#ifndef L4_GRAPH_H
#define L4_GRAPH_H

#include <stdint.h>

/* 크기 — 작게 유지한다. 커널의 정확성을 보는 것이고 성능은 L8 이다. */
#define L4_K		64	/* 내적 길이 (Q4_0 블록 2개) */
#define L4_M		32	/* 가중치 행 */
#define L4_N		4	/* 입력 열(토큰) */
#define L4_MM_N		(L4_M * L4_N)

#define L4_RMS_NE	128
#define L4_SILU_NE	128

#define L4_HEAD_DIM	32
#define L4_N_HEAD	2
#define L4_N_TOK	4
#define L4_ROPE_NE	(L4_HEAD_DIM * L4_N_HEAD * L4_N_TOK)

struct l4_results {
	float mm_f32[L4_MM_N];
	float mm_q4_0[L4_MM_N];
	float rms[L4_RMS_NE];
	float silu[L4_SILU_NE];
	float rope[L4_ROPE_NE];
	int   dotprod;		/* __ARM_FEATURE_DOTPROD 가 켜졌는가 */
	int   i8mm;		/* __ARM_FEATURE_MATMUL_INT8 */
};

/* 0 = 성공. 실패하면 음수. */
int l4_run(struct l4_results *out);

#endif /* L4_GRAPH_H */
