/*
 * rk3566_tsadc — 칩 온도 (TSADC 채널 0 = CPU, 1 = GPU).
 *
 * 출처: linux drivers/thermal/rockchip_thermal.c (rk3568_tsadc_data · rk_tsadcv7_initialize ·
 * rk_tsadcv3_control · rk_tsadcv2_get_temp · rk3568_code_table) ·
 * arch/arm64/boot/dts/rockchip/rk356x-base.dtsi (tsadc@fe710000 · grf syscon@fdc60000).
 *
 * ⚠ 첫 판은 **읽기만** 한다. 셧다운(TSHUT, CRU/GPIO)은 켜지 않는다 — INT_EN 은 건드리지 않고
 *   COMP_SHUT 은 닿지 않는 값(0xfff)으로 둔다. 부트로더가 남긴 설정은 rk3566_tsadc_boot_regs 로 본다.
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
#else
#include <errno.h>
static inline int rk3566_tsadc_get_mc(int chn, int *mc) { (void)chn; (void)mc; return -ENODEV; }
static inline uint32_t rk3566_tsadc_raw(int chn) { (void)chn; return 0; }
static inline const struct rk3566_tsadc_regs *rk3566_tsadc_boot_regs(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif
