/*
 * hubml 실행부 — 앱(hubllm · QEMU 시험)이 부르는 입구.
 *   hubml_start(): 원장·수집 엔진을 열고, 2 초마다 commit + 배포 재전송 틱을 도는 스레드를 띄운다.
 *   운반 1단계 = USB 셸: Mac 이 `ml rx b64=<데이터그램>` 으로 넣고, 허브가 보낼 것(ACK · CMD)은
 *   `HUBML/1 tx addr=<n> b64=<데이터그램>` 줄로 마지막으로 `ml` 을 친 셸에 찍는다(MLCC 합의 — USB 먼저).
 *   HaLow(NRC7394 BridgeRPC) 운반은 같은 hubml_xport 로 바꿔 끼운다.
 * 셸 명령은 ml_shell.c (HUBML/1 줄 규약 · --kv).
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** root 예 "/0:/ml" · hub_id = 칩 ID. 여러 번 불러도 한 번만 연다. 0 = 정상 */
int hubml_start(const char *root, const char *hub_id);
bool hubml_started(void);
const char *hubml_root(void);
const char *hubml_hub_id(void);
/** 저장소 준비 갈고리 (weak) — hubllm 은 SD 마운트를 여기서 한다. 0 = 준비됨 */
int hubml_storage_ready(void);

#ifdef __cplusplus
}
#endif
