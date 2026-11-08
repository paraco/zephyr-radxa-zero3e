/*
 * rk3566_tsadc — 칩 온도 (TSADC 채널 0 = CPU, 1 = GPU).
 *
 * 출처: linux drivers/thermal/rockchip_thermal.c (rk3568_tsadc_data · rk_tsadcv7_initialize ·
 * rk_tsadcv3_control · rk_tsadcv2_get_temp · rk3568_code_table) ·
 * arch/arm64/boot/dts/rockchip/rk356x-base.dtsi (tsadc@fe710000 · grf syscon@fdc60000).
 *
 * 셧다운(TSHUT): CRU 방식, 기본 CONFIG_RK3566_TSADC_TSHUT_MC (95 °C, linux dts hw-tshut-temp).
 *   문턱을 넘으면 칩이 리셋된다 — 소프트웨어가 멈춰도 듣는 마지막 방어선.
 *   ⚠ 이 보드에서 CRU 리셋이 실제로 걸리는지는 문턱을 낮춰 실기로 확인해야 한다
 *   (rk3566_tsadc_set_tshut, hubllm 의 `hub tshut <°C>`).
 *   부트로더가 셧다운을 켜 두었으면(INT_EN 비트) 아무것도 쓰지 않는다 → set_tshut 은 -EPERM.
 *   부트로더가 남긴 설정은 rk3566_tsadc_boot_regs 로 본다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct rk3566_tsadc_regs {
	uint32_t auto_con, int_en, int_pd, comp_shut0, comp_shut1, data0, data1;
};

#ifdef CONFIG_RK3566_TSADC
/** mC(1/1000 °C). 0 = 성공, -EAGAIN = 아직 표본 없음/범위 밖, -ENODEV = 초기화 안 됨 */
int rk3566_tsadc_get_mc(int chn, int *mc);
/** 원시 코드 (DATA 레지스터 & 0xfff) */
uint32_t rk3566_tsadc_raw(int chn);
/** 초기화 **전에** 읽은 레지스터 (부트로더가 남긴 것) */
const struct rk3566_tsadc_regs *rk3566_tsadc_boot_regs(void);
/** 셧다운 문턱 mC. 0 = 끔. -ERANGE(표 밖) · -EPERM(부트로더 설정 유지 중) */
int rk3566_tsadc_set_tshut(int mc);
int rk3566_tsadc_tshut_mc(void);
#else
#include <errno.h>
static inline int rk3566_tsadc_get_mc(int chn, int *mc) { (void)chn; (void)mc; return -ENODEV; }
static inline uint32_t rk3566_tsadc_raw(int chn) { (void)chn; return 0; }
static inline const struct rk3566_tsadc_regs *rk3566_tsadc_boot_regs(void) { return 0; }
static inline int rk3566_tsadc_set_tshut(int mc) { (void)mc; return -ENODEV; }
static inline int rk3566_tsadc_tshut_mc(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
