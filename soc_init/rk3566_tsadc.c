/*
 * rk3566_tsadc — 칩 온도 읽기 (읽기 전용 첫 판). 설명은 include/rk3566_tsadc.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <errno.h>
#include <rk3566_tsadc.h>

#define TSADC_PHYS 0xfe710000UL		/* rk356x-base.dtsi tsadc@fe710000 */
#define GRF_PHYS   0xfdc60000UL		/* rk356x-base.dtsi grf: syscon@fdc60000 */

/* rockchip_thermal.c */
#define USER_CON            0x00
#define AUTO_CON            0x04
#define INT_EN              0x08
#define INT_PD              0x0c
#define DATA(c)             (0x20 + (c) * 4)
#define COMP_SHUT(c)        (0x40 + (c) * 4)
#define HIGHT_INT_DEBOUNCE  0x60
#define HIGHT_TSHUT_DEBOUNCE 0x64
#define AUTO_PERIOD         0x68
#define AUTO_PERIOD_HT      0x6c
#define AUTO_EN             BIT(0)
#define AUTO_Q_SEL_EN       BIT(1)		/* TSADCV3_AUTO_Q_SEL_EN (rk_tsadcv3_control) */
#define AUTO_SRC_EN(c)      BIT(4 + (c))	/* TSADCV2_AUTO_SRC_EN */
#define DATA_MASK           0xfff
#define USER_INTER_PD_SOC   0xfc0		/* TSADCV5 — 97 µs */
#define AUTO_PERIOD_TIME    1622		/* TSADCV5 */
#define DEBOUNCE_COUNT      4
#define GRF_TSADC_CON       0x0600
#define GRF_TSADC_TSEN      (0x10001u << 8)
#define GRF_TSADC_ANA_REG(n) (0x10001u << (n))

/* rk3568_code_table (ADC_INCREMENT): {code, mC} */
static const struct { uint16_t code; int32_t mc; } tbl[] = {
	{0, -40000}, {1584, -40000}, {1620, -35000}, {1652, -30000}, {1688, -25000},
	{1720, -20000}, {1756, -15000}, {1788, -10000}, {1824, -5000}, {1856, 0},
	{1892, 5000}, {1924, 10000}, {1956, 15000}, {1992, 20000}, {2024, 25000},
	{2060, 30000}, {2092, 35000}, {2128, 40000}, {2160, 45000}, {2196, 50000},
	{2228, 55000}, {2264, 60000}, {2300, 65000}, {2332, 70000}, {2368, 75000},
	{2400, 80000}, {2436, 85000}, {2468, 90000}, {2500, 95000}, {2536, 100000},
	{2572, 105000}, {2604, 110000}, {2636, 115000}, {2672, 120000}, {2704, 125000},
};

static mm_reg_t ts, grf;
static struct rk3566_tsadc_regs boot;

uint32_t rk3566_tsadc_raw(int chn)
{
	return ts ? (sys_read32(ts + DATA(chn)) & DATA_MASK) : 0;
}

int rk3566_tsadc_get_mc(int chn, int *mc)
{
	if (!ts) { return -ENODEV; }
	if (chn < 0 || chn > 1) { return -EINVAL; }
	const uint32_t code = rk3566_tsadc_raw(chn);
	/* rk_tsadcv2_code_to_temp 의 ADC_INCREMENT: tbl[1] 미만은 잘못된 읽기 */
	if (code < tbl[1].code || code > tbl[ARRAY_SIZE(tbl) - 1].code) { return -EAGAIN; }
	for (size_t i = 2; i < ARRAY_SIZE(tbl); i++) {
		if (code <= tbl[i].code) {
			const int32_t dc = tbl[i].code - tbl[i - 1].code;
			const int32_t dt = tbl[i].mc - tbl[i - 1].mc;
			*mc = tbl[i - 1].mc + (int32_t)(code - tbl[i - 1].code) * dt / dc;
			return 0;
		}
	}
	*mc = tbl[1].mc;
	return 0;
}

const struct rk3566_tsadc_regs *rk3566_tsadc_boot_regs(void)
{
	return ts ? &boot : 0;
}

static int rk3566_tsadc_init(void)
{
	device_map(&ts, TSADC_PHYS, 0x100, K_MEM_CACHE_NONE);
	device_map(&grf, GRF_PHYS, 0x1000, K_MEM_CACHE_NONE);

	/* ① 부트로더가 남긴 것을 먼저 적어 둔다 (셧다운이 켜져 있었는지) */
	boot.auto_con   = sys_read32(ts + AUTO_CON);
	boot.int_en     = sys_read32(ts + INT_EN);
	boot.int_pd     = sys_read32(ts + INT_PD);
	boot.comp_shut0 = sys_read32(ts + COMP_SHUT(0));
	boot.comp_shut1 = sys_read32(ts + COMP_SHUT(1));
	boot.data0      = sys_read32(ts + DATA(0));
	boot.data1      = sys_read32(ts + DATA(1));

	/* ② 부트로더가 셧다운을 켜 두었으면(INT_EN 의 2GPIO/2CRU 비트) **건드리지 않는다** —
	 *    그 보호를 끄지 않으려고. 그때는 이미 돌고 있는 자동 측정을 그대로 읽는다. */
	if (boot.int_en & (0xfu << 4 | 0xfu << 8)) {
		return 0;
	}

	/* ③ rk_tsadcv7_initialize */
	sys_write32(USER_INTER_PD_SOC, ts + USER_CON);
	sys_write32(AUTO_PERIOD_TIME, ts + AUTO_PERIOD);
	sys_write32(DEBOUNCE_COUNT, ts + HIGHT_INT_DEBOUNCE);
	sys_write32(AUTO_PERIOD_TIME, ts + AUTO_PERIOD_HT);
	sys_write32(DEBOUNCE_COUNT, ts + HIGHT_TSHUT_DEBOUNCE);
	sys_write32(0, ts + AUTO_CON);			/* TSHUT 극성 low (dts hw-tshut-polarity 0) */
	sys_write32(GRF_TSADC_TSEN, grf + GRF_TSADC_CON);
	k_busy_wait(15);				/* TRM 18.5: ≥ 10 µs */
	sys_write32(GRF_TSADC_ANA_REG(0), grf + GRF_TSADC_CON);
	sys_write32(GRF_TSADC_ANA_REG(1), grf + GRF_TSADC_CON);
	sys_write32(GRF_TSADC_ANA_REG(2), grf + GRF_TSADC_CON);
	k_busy_wait(150);				/* TRM 18.5: ≥ 90 µs */

	/* ④ 채널 0·1 자동 측정. 셧다운 비교값은 닿지 않는 0xfff, INT_EN 은 0 그대로 */
	sys_write32(DATA_MASK, ts + COMP_SHUT(0));
	sys_write32(DATA_MASK, ts + COMP_SHUT(1));
	sys_write32(AUTO_SRC_EN(0) | AUTO_SRC_EN(1), ts + AUTO_CON);
	sys_write32(sys_read32(ts + AUTO_CON) | AUTO_EN | AUTO_Q_SEL_EN, ts + AUTO_CON);
	return 0;
}
/* device_map 은 POST_KERNEL 부터 (PRE_KERNEL 에서 부르면 보드가 죽었다 — moru CMakeLists) */
SYS_INIT(rk3566_tsadc_init, POST_KERNEL, 2);
