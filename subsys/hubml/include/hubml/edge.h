/*
 * hubml Edge 쪽 — 허브가 보낸 모델을 받아 슬롯에 넣고 점수를 낸다.
 * 허브(시험)와 Edge 펌웨어(Cortex-M7 · M33, R0)가 **같은 C 파일**을 쓴다. float32 만, 동적 할당 없음.
 *   슬롯 0 = champion(경보를 낸다) · 1 = challenger(시범 — 점수만, 경보 없음)
 *   받는 버퍼 하나(HUBML_EDGE_MAXMODEL) — END 에서 sha256 · 파일 검사(골든) · 계약을 본 뒤에만 슬롯에 넣는다.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <hubml/xmb.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HUBML_EDGE_MAXMODEL
#define HUBML_EDGE_MAXMODEL 16384	/* kind 3 (34 특징 · 구간 4) = 11,268 B */
#endif

struct hubml_edge_slot {
	bool valid;
	uint32_t num, size;
	uint8_t sha[32];
	struct xmd_model m;
};

struct hubml_edge {
	uint16_t contract;
	struct hubml_edge_slot slot[2];
	/* 받는 중 */
	bool rx_on;
	uint16_t rx_xfer;
	uint8_t rx_slot;
	uint32_t rx_size, rx_off, rx_num;
	uint8_t rx_sha[32];
	/* 마지막으로 끝낸 전송 — 응답을 잃어 허브가 다시 묻거나 늦은 조각이 오면 같은 결과를 돌려준다
	 * (2026-11-09 Lm5: 이것이 없어 sha256 다름(-3)·계약 다름(-5)이 늦은 메시지의 답에 덮였다) */
	uint16_t done_xfer;
	int8_t done_status;
	uint8_t done_slot;
	uint8_t buf[HUBML_EDGE_MAXMODEL];
	uint32_t n_ok, n_fail;
};

void hubml_edge_init(struct hubml_edge *e, uint16_t contract);
/** CMD 본문을 처리하고 CMD_ACK 본문(MCOL_CMD_ACK_LEN)을 reply 에 쓴다. 반환 = reply 길이 */
int hubml_edge_on_cmd(struct hubml_edge *e, const uint8_t *pl, uint16_t len, uint8_t *reply);
/** 점수: champion · challenger (슬롯이 비었으면 -1) */
void hubml_edge_score(const struct hubml_edge *e, const float *x, float *champ, float *chall);

#ifdef __cplusplus
}
#endif
