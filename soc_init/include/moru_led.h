/*
 * moru_led — 보드 LED(GPIO0_A0) 로 진행 상태를 보인다.
 *
 * | 상태          | 패턴 (켜짐/꺼짐, 초)                 | 누가 정하나            |
 * |---------------|--------------------------------------|------------------------|
 * | (U-Boot)      | 계속 켜짐 — 블롭 읽기(30~50 s) 포함  | sdcard/boot.cmd        |
 * | BOOT          | 계속 켜짐 — Zephyr 기동 중           | PRE_KERNEL_1 (자동)    |
 * | READY         | 2/2 반복 — 부팅 완료·대기            | APPLICATION 끝 (자동)  |
 * | BLOB          | 0.5/0.5 ×2 → 1/1, 반복 — 모델 적재   | 앱                     |
 * | INFER         | 0.1/0.9 반복 — 추론 중(심장 박동)    | 앱                     |
 * | OTA           | 0.2/0.2 ×5 → 2/2, 반복 — OTA 수신    | moru ota begin         |
 * | FAIL          | 0.2/0.2 ×3 → 0.2/1.2, 반복 — 시험 실패 | 앱                   |
 * | CRASH         | 0.2/0.2 계속 — 치명 오류(인터럽트 없이) | fatal 핸들러        |
 *
 * 깜박임은 k_timer(ISR) 가 돌린다 — 스레드가 아니라서 4코어가 다 바빠도 멈추지
 * 않는다. CRASH 는 인터럽트가 꺼진 채 k_busy_wait 로 돈다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

enum moru_led_state {
	MORU_LED_BOOT = 0,
	MORU_LED_READY,
	MORU_LED_BLOB,
	MORU_LED_INFER,
	MORU_LED_OTA,
	MORU_LED_FAIL,
	MORU_LED_CRASH,
};

#ifdef CONFIG_MORU_LED
/** 상태를 바꾼다. 패턴은 처음(켜짐)부터 다시 시작한다. 같은 상태면 그대로. */
void moru_led_set(enum moru_led_state s);
enum moru_led_state moru_led_get(void);
/** 치명 오류: 인터럽트를 끄고 0.2 초 점멸을 영원히. 돌아오지 않는다. */
FUNC_NORETURN void moru_led_fatal(void);
#else
static inline void moru_led_set(enum moru_led_state s) { (void)s; }
static inline enum moru_led_state moru_led_get(void) { return MORU_LED_BOOT; }
#endif

#ifdef __cplusplus
}
#endif
