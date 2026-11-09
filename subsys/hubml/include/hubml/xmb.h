/*
 * .xmb (XMB1) — Edge 트리 모델 파일. 정본: Pico_ADC0_DMA_Capture/docs/MODEL_BIN_SPEC.md §1.
 *   HEADER 64 B · ROOTS T×u32 · NODES N×16 B · GOLDEN 4×(n_feat×4+4) · FOOTER body_crc32
 * 평가는 펌웨어 eval_buf · Studio XgbEvaluator 와 1:1 — float32 · strict `<` · 범위 밖 특징 0 ·
 * 깊이 상한 64 · base_logit = logf(base/(1-base+1e-7f)) · sigmoid 안전 분기.
 * ⚠ LWCT 펌웨어는 n_feat 22..24 만 받는다. MPDM(n_feat 34)은 MPDM 펌웨어가 받아야 한다(R0).
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XMB_MAX_TREE 250	/* MPDM 팩 limits.tree */
#define XMB_MAX_NODE 6144	/* MPDM 팩 limits.node */
#define XMB_MAX_FEAT 64
#define XMB_NGOLDEN  4
#define XMB_HDR_LEN  64

struct xmb_node {
	int16_t feat, left, right;
	uint8_t is_leaf;
	float thr, val;
};

struct xmb_model {
	uint8_t kind, nfeat, target;
	uint16_t ntree, contract;
	uint32_t nnode, train_ts;
	float base, thr;
	char meta[24];
	uint32_t root[XMB_MAX_TREE];
	struct xmb_node node[XMB_MAX_NODE];
	float golden[XMB_NGOLDEN][XMB_MAX_FEAT + 1];	/* 입력 n_feat 개 + 기대 확률 */
};

float xmb_predict(const struct xmb_model *m, const float *f);
/** 반환 = 파일 크기, 음수 = 오류 */
int xmb_size(const struct xmb_model *m);
int xmb_write(const struct xmb_model *m, uint8_t *out, size_t cap);
/** 헤더 CRC · 상한 · 본문 CRC · 골든(|Δ| ≤ tol) 검사. 0 = 정상, 음수 = 사유 (-1 magic … -7 golden) */
int xmb_parse(const uint8_t *in, size_t n, struct xmb_model *m, float tol);

/*
 * kind=3 — 통계 기준선 모델 (MPDM 제안, IF-PKG 확장 · Edge 펌웨어 R0 에서 받아야 한다).
 *   2026-11-09 QEMU Lm2: Isolation Forest(kind 1)는 단일 특징 30~60 % 변화를 거의 못 잡았다(검출 ≈ 오경보율).
 *   운전 구간별 마할라노비스 기준선은 ×1.3 에서 100 %. 그래서 Edge 에는 이것을 보낸다.
 *   머리 64 B 는 XMB1 과 같고 kind=3 · tree_count 0 · node_count 0 · threshold 1.0 (점수 문턱).
 *   본문: nreg u8 · reg_feat u8 · rsv u16 · edges f32×(XMD_MAX_REG−1)
 *         구간마다 { mean f32×n · sd f32×n · L f32×n(n+1)/2 (하삼각, 행 순서) · thr_d2 f32 }
 *         골든 4 × (n f32 + 기대 점수 f32) · 끝 body_crc32
 *   점수 = d²/thr_d2[구간] (float32 전진 대입), 점수 > 1 이면 경보. 가장 벗어난 특징 = max |x−μ|/sd.
 */
#define XMD_MAX_REG 4
struct xmd_model {
	uint8_t nfeat, nreg, reg_feat, target;
	uint16_t contract;
	uint32_t train_ts;
	char meta[24];
	float edges[XMD_MAX_REG - 1];
	struct {
		float mean[XMB_MAX_FEAT], sd[XMB_MAX_FEAT];
		float L[XMB_MAX_FEAT * (XMB_MAX_FEAT + 1) / 2];
		float thr_d2;
	} reg[XMD_MAX_REG];
	float golden[XMB_NGOLDEN][XMB_MAX_FEAT + 1];
};
/** 점수 (경보 > 1). zmax 에 가장 벗어난 특징 번호(NULL 가능) */
float xmd_score(const struct xmd_model *m, const float *x, int *zmax);
int xmd_size(const struct xmd_model *m);
int xmd_write(const struct xmd_model *m, uint8_t *out, size_t cap);
int xmd_parse(const uint8_t *in, size_t n, struct xmd_model *m, float tol);

#ifdef __cplusplus
}
#endif
