/* SPDX-License-Identifier: Apache-2.0 — hub_relay.c 머리말 참고 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** 이번 부팅에 U-Boot 가 올린 모델 파일 이름 ("models/…" 의 … 또는 "blob.bin"). 모르면 "-" */
const char *hub_relay_loaded(void);
/** hub.conf 의 로그 번호 (U-Boot 가 넘김). 모르면 "-" */
const char *hub_relay_logseq(void);
/** 로그 한 줄 덧붙이기 (끝에 \n 을 붙인다). 다 차면 버리고 잘림 표시를 한 번 남긴다. */
void hub_log(const char *line);
size_t hub_log_len(void);
/** 로그 글의 시작 (RAM 중계 구역). hub_log_len() 앞까지는 바뀌지 않는다 — SD 실시간 기록이 읽는다 */
const char *hub_log_buf(void);
/** false = Zephyr 가 SD 에 직접 쓴다 → 'HLOG' 를 지우고 다시 세우지 않는다 (U-Boot 는 저장하지 않는다).
 *  true = 재부팅 때 U-Boot 가 logs/hub-<다음 번호>.txt 로 쓴다 (기본, SD 쓰기 실패 때의 대비) */
void hub_relay_set_uboot_save(bool on);
/** 이번 부팅 로그 번호. U-Boot 대비 저장도 이 번호를 쓴다 */
unsigned hub_relay_next_seq(void);
void hub_relay_set_next_seq(unsigned seq);
/** 모델 파일 이름 규칙: 1..60 자, [A-Za-z0-9._-] 만 (경로·공백 금지). 0 / -EINVAL */
int hub_relay_name_ok(const char *file);
/** hub load 가 모델을 바꾼 뒤 이름을 고친다 (U-Boot 가 넘긴 값 대신) */
void hub_relay_set_loaded(const char *file);
/** 다음 부팅에 올릴 모델을 적는다. save 면 hub.conf 의 기본값도 바꾼다. 0 / -EINVAL */
int hub_relay_request_model(const char *file, bool save);
/** PSCI SYSTEM_RESET — 로그는 다음 부팅에 U-Boot 가 카드에 쓴다 */
void hub_relay_reboot(void);
#ifdef __cplusplus
}
#endif
