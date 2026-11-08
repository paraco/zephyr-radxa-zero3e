/* clk1416 — vdd_cpu 1.025 V → APLL 1416 MHz (실기 전용). SPDX-License-Identifier: Apache-2.0 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/** 0 = 1416 으로 올림. 그 밖이면 1104 그대로(이유를 찍는다). */
int clk1416_raise(void);
/** 1416 이면 1104 로 내린다. 이미 1104 면 아무것도 하지 않는다. */
void clk1416_back_to_1104(const char *why);
/** 지금 상태·실측·감시 횟수를 한 줄로 찍는다. */
void clk1416_report(void);
int clk1416_is_1416(void);
unsigned clk1416_guard_reads(void);
unsigned clk1416_guard_fail(void);
/** PMU 로 잰 지금 코어 클럭 (MHz, 20 ms 측정) */
unsigned clk1416_cpu_mhz(void);
#ifdef __cplusplus
}
#endif
