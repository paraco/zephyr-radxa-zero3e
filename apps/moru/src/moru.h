/*
 * apps/moru 내부 공용 선언.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MORU_H_
#define MORU_H_

#include <stdint.h>

/** 초기 부팅 동기 마커 — uart2 에 직접 찍는다 (moru_mark.c). */
void moru_mark(const char *s);

/** ARM implementer·part 번호를 사람이 읽는 이름으로 (moru_boot.c). */
const char *moru_cpu_part_name(uint32_t implementer, uint32_t part);

/** 모델 이름 — DT 의 model 속성을 코드로 고정한 것 (moru_boot.c). */
#define MORU_MODEL "Radxa ZERO 3E"

/**
 * Zephyr 진입 전 부트로더가 쓴 시간.
 *
 * 로그 타임스탬프는 Zephyr 진입을 0 으로 맞추므로(moru_boot.c 의
 * moru_cycle_timestamp), 전원 투입부터 진입까지의 시간은 따로 보관한다.
 * 0 이면 측정하지 못한 것이다 (CNTFRQ_EL0 가 터무니없는 값이었던 경우).
 */
uint64_t moru_preboot_us(void);

#endif /* MORU_H_ */
