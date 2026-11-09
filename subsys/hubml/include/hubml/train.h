/*
 * hubml 학습 — 정상 데이터만으로 장비별 이상 점수 모델을 만든다 (모터 예지 MPDM, MFEAT34).
 *
 *  1층 통계 기준선 (허브가 쓴다): 운전 구간(load_ratio 4 구간)마다 평균·공분산 → 마할라노비스 d².
 *      문턱 = 학습 구간 d² 의 q 분위수. 어느 특징이 벗어났는지 z 점수로 설명할 수 있다.
 *  2층 Isolation Forest → .xmb (Edge 가 쓴다): 표본 ψ 개 × T 그루, 깊이 ≤ ceil(log2 ψ).
 *      잎 값 = k·(c(ψ) − (깊이 + c(잎 크기)))/T, base 0.5 → Edge 평가기(잎 합 → 시그모이드)의
 *      확률 = sigmoid(k·(c(ψ) − E[경로 길이])) — 짧게 고립될수록(이상) 커진다. 문턱 = 학습 구간 q 분위수.
 *
 *  데이터는 SD 에서 흘려 읽는다(여러 번 지나간다). 마지막 holdout 비율은 학습에 쓰지 않고 오경보율을 잰다.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <hubml/xmb.h>
#include <hubml/gate.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUBML_NFEAT      34	/* MFEAT34 */
#define HUBML_NREG       4
#define HUBML_REG_FEAT   27	/* load_ratio (0 기준 번호) */
#define HUBML_MAX_DAYS   64
#define HUBML_TRAIN_MIN  1000	/* 학습 최소 벡터 수 (거른 뒤) — 미만이면 -ENODATA */

/** 특징 벡터 하나를 받는 콜백. idx = 0 부터의 순서 번호 */
typedef void (*hubml_vec_cb)(const float *x, uint32_t idx, void *user);
/** 장비 uid 의 날짜들(yyyymmdd)에 있는 FEAT 를 순서대로 넘긴다 (quality != 0 은 뺀다). 반환 = 개수 */
int hubml_feat_each(const char *root, uint64_t uid, const char days[][9], int ndays, hubml_vec_cb cb, void *user);
/** 위와 같되 gate 가 있으면 관문(include/exclude, 측정 UTC)으로 거른다. gate = NULL 이면 hubml_feat_each 와 같다.
 *  counts(있으면)에 이 패스의 셈. ⚠ 정적 앵커 표를 쓴다 — 동시에 두 번 부르지 않는다(hubml 스레드 하나) */
int hubml_feat_each_gate(const char *root, uint64_t uid, const char days[][9], int ndays, const struct hubml_gate *gate,
			 hubml_vec_cb cb, void *user, struct hubml_gate_counts *counts);

struct hubml_train_cfg {
	uint64_t uid;
	const char (*days)[9];
	int ndays;
	float holdout;		/* 끝에서 이 비율은 평가용 (예 0.2) */
	float q;		/* 문턱 분위수 (예 0.995) */
	uint16_t trees;		/* 예 45 */
	uint16_t psi;		/* 예 64 */
	float k;		/* 잎 값 배율 (예 1.0) */
	uint32_t seed;
	uint32_t train_ts;
	const struct hubml_gate *gate;	/* NULL = 관문 없음(예전 동작). 있으면 days/ndays 는 gate->fdays/nfdays */
};

struct hubml_baseline {
	int nreg;
	float reg_edge[HUBML_NREG - 1];	/* load_ratio 구간 경계 */
	struct {
		uint32_t n;
		double mean[HUBML_NFEAT];
		double sd[HUBML_NFEAT];
		double L[HUBML_NFEAT * HUBML_NFEAT];	/* 공분산의 촐레스키 */
		double thr_d2;
		int8_t borrowed;	/* -1 = 자기 데이터, 아니면 빌려 온 구간 번호 (학습하지 않은 운전 구간) */
	} reg[HUBML_NREG];
};

struct hubml_train_result {
	uint32_t n_total, n_train, n_hold;
	uint32_t ms_scan, ms_build, ms_total;
	float far_stats, far_tree;	/* holdout 오경보율 */
	float thr_prob;
	uint32_t nodes;
	struct hubml_gate_counts gate_cnt;	/* 관문이 있을 때 한 패스 기준 셈 (n_total 은 거른 뒤 수) */
};

/** 학습. bl·xm 을 채운다. 0 = 정상 */
int hubml_train(const char *root, const struct hubml_train_cfg *c, struct hubml_baseline *bl,
		struct xmb_model *xm, struct hubml_train_result *r);

int hubml_baseline_regime(const struct hubml_baseline *bl, const float *x);
/** 마할라노비스 d² (구간 자동). zmax_idx 에 가장 많이 벗어난 특징 번호(있으면) */
double hubml_baseline_d2(const struct hubml_baseline *bl, const float *x, int *zmax_idx);

/** 기준선을 Edge 용 kind=3 모델로 (float32). 골든 4 개 = golden_x 입력 */
void hubml_baseline_export(const struct hubml_baseline *bl, uint32_t train_ts, const float golden_x[XMB_NGOLDEN][HUBML_NFEAT],
			   struct xmd_model *out);

/** holdout 구간에 변환(가짜 고장)을 걸어 경보율을 잰다 — 변환 NULL 이면 오경보율 */
typedef void (*hubml_xform)(float *x, void *user);
struct hubml_eval_out { uint32_t n; float rate_stats, rate_tree, rate_edge; };
/** xd 가 있으면 Edge(kind 3, float32) 점수로도 센다 (rate_edge) */
int hubml_eval_holdout(const char *root, const struct hubml_train_cfg *c, const struct hubml_train_result *r,
		       const struct hubml_baseline *bl, const struct xmb_model *xm, const struct xmd_model *xd,
		       hubml_xform xf, void *xu, struct hubml_eval_out *o);

#ifdef __cplusplus
}
#endif
