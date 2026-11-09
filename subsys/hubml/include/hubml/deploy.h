/*
 * hubml 배포 — 원장에 있는 모델을 Edge 슬롯으로 보낸다 (MCOL CMD BEGIN · CHUNK · END, ACTIVATE).
 *   전송은 go-back-N (창 4 조각, 응답 없으면 다시) — UDP 라 허브가 끝까지 책임진다.
 *   끝나면(성공·실패) 원장 deploy 사건: uid · model_id · slot · result · code · edge_model_sha256.
 *   challenger 슬롯으로 보내는 것은 허브가 할 수 있고(시범), ACTIVATE(운영 승격)는 champion 별칭이
 *   그 모델을 가리킬 때만 보낸다 — 별칭 이동에는 사람 서명이 필요하다(ledger.c).
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hubml_dep_state { HUBML_DEP_IDLE, HUBML_DEP_BEGIN, HUBML_DEP_SEND, HUBML_DEP_END, HUBML_DEP_ACT, HUBML_DEP_DONE, HUBML_DEP_FAIL };

struct hubml_dep_status {
	enum hubml_dep_state state;
	char model_id[25];
	uint8_t slot;
	uint32_t size, acked, sent_chunks, resends;
	int code;		/* 0 · Edge status · -ETIMEDOUT */
	uint8_t edge_sha[32];
};

/** 원장의 model_id 를 Edge 의 slot(0 champion · 1 challenger)으로 보낸다. 0 = 시작함 */
int hubml_deploy_start(uint64_t uid, const char *model_id, uint8_t slot);
/** Edge 의 challenger 를 champion 으로 (champion 별칭이 그 모델을 가리켜야 한다) */
int hubml_deploy_activate(uint64_t uid);
/** 주기적으로 부른다 (응답 기다림·다시 보내기). tick = 호출 단위 */
void hubml_deploy_tick(void);
int hubml_deploy_status(uint64_t uid, struct hubml_dep_status *out);

#ifdef __cplusplus
}
#endif
