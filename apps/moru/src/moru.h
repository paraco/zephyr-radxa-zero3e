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

/**
 * 모델 이름.
 *
 * DT 루트의 model 속성은 바인딩에 없어 매크로가 생성되지 않으므로
 * (DT_PROP 불가, dt_prop() 도 빈 값) 고정 문자열로 둔다. 이 앱은 이미 보드
 * 전용이다 — SoC 문구와 main.c 의 GPIO0 주소도 하드코딩이다.
 */
#define MORU_MODEL "Radxa ZERO 3E"

/**
 * 전원 투입부터 Zephyr 의 PRE_KERNEL_1 까지 걸린 시간 (µs).
 *
 * 로그 타임스탬프는 PRE_KERNEL_1 을 0 으로 맞추므로(moru_boot.c 의
 * moru_cycle_timestamp), 그 앞 구간은 이 값으로만 남는다.
 *
 * ⚠ U-Boot·TF-A BL31 뿐 아니라 **Zephyr 자신의 arm64 초기 부팅**
 *   (reset.S 의 MMU·캐시 설정, EL2→EL1 전환, bss 지우기)도 포함된다.
 *   그 구간을 따로 측정한 적은 없다.
 *
 * 0 이면 측정하지 못한 것이다 (CNTFRQ_EL0 가 터무니없는 값이었던 경우).
 */
uint64_t moru_preboot_us(void);

/**
 * `moru htop` 핸들러 (moru_htop.c). 명령 등록은 moru_shell.c 가 한다.
 *
 * ⚠ 이 핸들러는 **돌아오지 않고 루프를 돈다** (q · Ctrl-C · 600초 상한).
 *   그동안 shell_process() 가 멈추므로 다른 셸 명령도, 셸 로그 백엔드
 *   비우기도 함께 멈춘다. 로그는 uart2 백엔드로 계속 나간다.
 */
struct shell;
int cmd_moru_htop(const struct shell *sh, size_t argc, char **argv);

#endif /* MORU_H_ */
