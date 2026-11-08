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
/** 다음 부팅에 올릴 모델을 적는다. save 면 hub.conf 의 기본값도 바꾼다. 0 / -EINVAL */
int hub_relay_request_model(const char *file, bool save);
/** PSCI SYSTEM_RESET — 로그는 다음 부팅에 U-Boot 가 카드에 쓴다 */
void hub_relay_reboot(void);
#ifdef __cplusplus
}
#endif
