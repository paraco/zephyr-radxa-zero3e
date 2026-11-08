/* L9b — Zephyr 에서 I2C0 을 직접 두드려 vdd_cpu 를 올리고, 유지되면 1416 MHz.
 *
 * ── 왜 Zephyr 에서인가 ────────────────────────────────────────────────
 *
 * U-Boot 의 boot.cmd 에서 0x99 를 VSEL0·VSEL1 에 썼더니 (2026-11-08):
 *     쓰기 직후:  VSEL0 0x99 · VSEL1 0x99
 *     부팅 끝:    VSEL0 0x8f · VSEL1 0x99     ← VSEL0 만 되돌아갔다
 * 쓰기와 확인 사이에 U-Boot 가 60여 초(blob 로드)를 더 돌았다. 그래서
 * **되돌린 주체가 칩인지 U-Boot 인지** 가릴 수 없었다.
 *
 * Zephyr 에서 쓰고 Zephyr 가 계속 읽으면 그 사이에 끼는 것이 없다:
 *   · 120초 동안 유지되면 → 되돌린 것은 **U-Boot** 였다
 *   · 여기서도 되돌아가면 → **칩**이 되돌린다
 *
 * ── I2C 트랜잭션은 **U-Boot 의 것을 그대로** 따른다 ─────────────────
 *
 * 이 보드에서 i2c mw / i2c read 가 실제로 동작한 경로가 그것이다:
 *   u-boot/drivers/i2c/rk_i2c.c 의 rockchip_i2c_xfer · rk_i2c_write ·
 *   rk_i2c_read · rk_i2c_send_start_bit · rk_i2c_send_stop_bit
 *   u-boot/arch/arm/include/asm/arch-rockchip/i2c.h 의 struct i2c_regs ·
 *   I2C_CON_* · I2C_*IEN · I2C_*IPD · I2C_MRXADDR_SET
 * (2026-11-08 원문 전문 확인.) 메시지 사이에 **STOP 이 아니라 CON=0** 을
 * 쓰는 것까지 그대로다 — 드라이버 주석: HW 가 repeated start 를 못 해서
 * 내부 상태를 리셋하고 새 START 를 낸다.
 *
 * CLKDIV 는 **건드리지 않는다.** U-Boot 가 i2c dev 0 에서 설정해 둔 값을
 * 물려받는다(dwc3 의 클럭을 물려받는 것과 같은 방식). 0 이면 중단한다.
 *
 * ── 안전 장치 ─────────────────────────────────────────────────────────
 *
 *   · 쓰기 전에 **읽는다.** NAK·시간초과·EN=0 이면 **쓰지 않고 중단**
 *   · 쓰는 값은 VSEL_TARGET 하나뿐이고 컴파일 시 단언으로 고정한다
 *     (bit7 EN=1 · sel ≤ 35 = 1.15 V · MODE=0). 대상 레지스터도 0x00·0x01
 *     뿐이고 rk8600_write 가 실행 시에도 한 번 더 막는다
 *   · 클럭은 **두 VSEL 이 관찰 창 내내 목표값일 때만** 올린다
 *   · PLL lock 을 **시간초과로** 기다린다 (U-Boot 에는 없다). 시간초과면
 *     1104 로 되돌린다
 *   · 올린 뒤 CRC32 안정성 시험. 하나라도 다르면 즉시 1104 로
 *   · apply try 로 올리므로 멈춰도 **전원 재투입이면 1104 moru** 로 돌아온다
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <stdlib.h>
#include <inttypes.h>

#ifndef CONFIG_BOARD_RADXA_ZERO_3E
#error "L9b 는 실기(radxa_zero_3e) 전용이다 — I2C0·CRU 를 직접 다룬다"
#endif

#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })

/* ── 주소 (출처 둘 이상 + L9a 실측) ─────────────────────────────────── */
#define I2C0_BASE  0xfdd40000UL	/* linux rk356x-base.dtsi · TF-A rk3568_def.h */
#define CRU_BASE   0xfdd20000UL	/* 같은 두 출처 + L9a 실측(계산 816 = 실측 815) */

/* ── struct i2c_regs (u-boot asm/arch-rockchip/i2c.h) ───────────────── */
#define I2C_CON      0x00
#define I2C_CLKDIV   0x04
#define I2C_MRXADDR  0x08
#define I2C_MRXRADDR 0x0c
#define I2C_MTXCNT   0x10
#define I2C_MRXCNT   0x14
#define I2C_IEN      0x18
#define I2C_IPD      0x1c
#define I2C_TXDATA0  0x100	/* 0x24 + reserved0[0x37]*4 */
#define I2C_RXDATA0  0x200	/* 0x120 + reserved1[0x38]*4 */

#define I2C_CON_EN        (1u << 0)
#define I2C_CON_MOD(m)    ((m) << 1)
#define I2C_MODE_TX       0x00
#define I2C_MODE_TRX      0x01
#define I2C_CON_START     (1u << 3)
#define I2C_CON_STOP      (1u << 4)
#define I2C_CON_LASTACK   (1u << 5)

#define I2C_MBTFIEN   (1u << 2)
#define I2C_MBRFIEN   (1u << 3)
#define I2C_STARTIEN  (1u << 4)
#define I2C_STOPIEN   (1u << 5)
#define I2C_NAKRCVIEN (1u << 6)

#define I2C_MBTFIPD   (1u << 2)
#define I2C_MBRFIPD   (1u << 3)
#define I2C_STARTIPD  (1u << 4)
#define I2C_STOPIPD   (1u << 5)
#define I2C_NAKRCVIPD (1u << 6)
#define I2C_IPD_ALL_CLEAN 0x7f

#define I2C_MRXADDR_SET(vld, a) (((vld) << 24) | (a))

#define I2C_TIMEOUT_US 100000	/* u-boot I2C_TIMEOUT_MS 100 */

/* ── RK8600 (linux fan53555.c) ──────────────────────────────────────── */
#define RK8600_ADDR  0x40
#define VSEL0        0x00
#define VSEL1        0x01

/* 0x99 = bit7 EN=1 · bit6 MODE=0 · sel 25 → 712500 + 12500×25 = 1025000 µV
 * (linux rk3566.dtsi opp-table 1416 MHz = 1025000 µV)
 * MODE=0 은 L9a 실측(VSEL0 0x8f · VSEL1 0x97, 둘 다 MODE 0)을 따른다. */
#define VSEL_TARGET  0x99
BUILD_ASSERT((VSEL_TARGET & 0x80) != 0, "BUCK_EN 을 지우면 레귤레이터가 꺼져 보드가 즉사한다");
BUILD_ASSERT((VSEL_TARGET & 0x3f) <= 35, "sel 35(1.15 V) 를 넘기지 않는다");
BUILD_ASSERT((VSEL_TARGET & 0x40) == 0, "MODE 는 출하 설정(0) 그대로 둔다");

static uint32_t rk8600_uv(uint8_t r) { return 712500u + 12500u * (uint32_t)(r & 0x3f); }

/* ── CRU (u-boot cru_rk3568.h · clk_pll.c · clk_rk3568.c) ───────────── */
#define CRU_APLL_CON0 0x00
#define CRU_APLL_CON1 0x04
#define CRU_MODE_CON  0xc0
#define CRU_CLKSEL(n) (0x100 + 4 * (n))

static mm_reg_t i2c, cru;

static inline uint32_t ir(uint32_t o) { return sys_read32(i2c + o); }
static inline void iw(uint32_t o, uint32_t v) { sys_write32(v, i2c + o); }
static inline uint32_t cr(uint32_t o) { return sys_read32(cru + o); }
static inline void cw(uint32_t o, uint32_t v) { sys_write32(v, cru + o); }

/* IPD 의 bit 를 기다린다. NAK 은 기록만 한다(u-boot 와 같다). */
static int i2c_wait(uint32_t bit, bool *nak)
{
	for (uint32_t t = 0; t < I2C_TIMEOUT_US; t++) {
		const uint32_t ipd = ir(I2C_IPD);

		if (ipd & I2C_NAKRCVIPD) {
			iw(I2C_IPD, I2C_NAKRCVIPD);
			if (nak) { *nak = true; }
		}
		if (ipd & bit) {
			iw(I2C_IPD, bit);
			return 0;
		}
		k_busy_wait(1);
	}
	return -ETIMEDOUT;
}

/* rk_i2c_send_start_bit */
static int i2c_start(void)
{
	iw(I2C_IPD, I2C_IPD_ALL_CLEAN);
	iw(I2C_CON, I2C_CON_EN | I2C_CON_START);
	iw(I2C_IEN, I2C_STARTIEN);
	return i2c_wait(I2C_STARTIPD, NULL);
}

/* rk_i2c_send_stop_bit + rk_i2c_disable */
static void i2c_stop_disable(void)
{
	iw(I2C_IPD, I2C_IPD_ALL_CLEAN);
	iw(I2C_CON, I2C_CON_EN | I2C_CON_STOP);
	iw(I2C_IEN, I2C_STOPIEN);
	(void)i2c_wait(I2C_STOPIPD, NULL);
	iw(I2C_CON, 0);
}

/* rk_i2c_write(chip, 0, 0, buf, len) — buf 는 바이트 열(첫 바이트가 레지스터) */
static int i2c_tx(uint8_t chip, const uint8_t *buf, uint32_t len, bool *nak)
{
	const uint32_t n = len + 1;	/* 주소 바이트 포함 */
	uint32_t tx = (uint32_t)chip << 1;

	if (n > 4) { return -EINVAL; }	/* 이 앱은 한 워드만 쓴다 */
	for (uint32_t j = 1; j < n; j++) {
		tx |= (uint32_t)buf[j - 1] << (j * 8);
	}
	int err = i2c_start();
	if (err) { return err; }
	iw(I2C_TXDATA0, tx);
	iw(I2C_CON, I2C_CON_EN | I2C_CON_MOD(I2C_MODE_TX));
	iw(I2C_MTXCNT, n);
	iw(I2C_IEN, I2C_MBTFIEN | I2C_NAKRCVIEN);
	return i2c_wait(I2C_MBTFIPD, nak);
}

/* rk_i2c_read(chip, 0, 0, buf, len) */
static int i2c_rx(uint8_t chip, uint8_t *buf, uint32_t len, bool *nak)
{
	if (len == 0 || len > 4) { return -EINVAL; }
	int err = i2c_start();
	if (err) { return err; }
	iw(I2C_MRXADDR, I2C_MRXADDR_SET(1, ((uint32_t)chip << 1) | 1));
	iw(I2C_MRXRADDR, 0);
	iw(I2C_CON, I2C_CON_EN | I2C_CON_LASTACK | I2C_CON_MOD(I2C_MODE_TRX));
	iw(I2C_MRXCNT, len);
	iw(I2C_IEN, I2C_MBRFIEN | I2C_NAKRCVIEN);
	err = i2c_wait(I2C_MBRFIPD, nak);
	if (err) { return err; }
	const uint32_t rx = ir(I2C_RXDATA0);
	for (uint32_t j = 0; j < len; j++) {
		buf[j] = (uint8_t)(rx >> (j * 8));
	}
	return 0;
}

/* rockchip_i2c_xfer 와 같은 꼴: [쓰기 {reg}] → CON=0 → [읽기 n] → STOP → CON=0 */
static int rk8600_read(uint8_t reg, uint8_t *out, uint32_t n)
{
	bool nak = false;
	int err = i2c_tx(RK8600_ADDR, &reg, 1, &nak);

	if (!err && !nak) {
		iw(I2C_CON, 0);		/* 메시지 사이: STOP 이 아니다 */
		err = i2c_rx(RK8600_ADDR, out, n, &nak);
	}
	i2c_stop_disable();
	return err ? err : (nak ? -EIO : 0);
}

/* rockchip_i2c_xfer 와 같은 꼴: [쓰기 {reg, val}] → STOP → CON=0 */
static int rk8600_write(uint8_t reg, uint8_t val)
{
	/* ⚠ 쓰는 곳과 값을 실행 시에도 막는다 — 단언은 상수만 지킨다 */
	if ((reg != VSEL0 && reg != VSEL1) || val != VSEL_TARGET) {
		printk("      ⚠⚠ 허용되지 않은 쓰기 거부: reg 0x%02x val 0x%02x\n", reg, val);
		return -EPERM;
	}
	const uint8_t b[2] = { reg, val };
	bool nak = false;
	int err = i2c_tx(RK8600_ADDR, b, 2, &nak);

	i2c_stop_disable();
	return err ? err : (nak ? -EIO : 0);
}

static int read3(uint8_t v[3])
{
	return rk8600_read(VSEL0, v, 3);	/* VSEL0 · VSEL1 · CONTROL */
}

/* ── PMU 실측 주파수 ────────────────────────────────────────────────── */
static uint64_t cpu_mhz(void)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	v &= ~(uint64_t)(1U << 3);
	v |= (1U << 0) | (1U << 2) | (1U << 6);
	__asm__ volatile("msr pmcr_el0, %0" ::"r"(v));
	__asm__ volatile("msr pmcntenset_el0, %0" ::"r"((uint64_t)1 << 31));
	__asm__ volatile("isb");
	const uint64_t f = SYSREG_R("cntfrq_el0");
	const uint64_t t0 = SYSREG_R("cntvct_el0"), c0 = SYSREG_R("pmccntr_el0");
	while (SYSREG_R("cntvct_el0") - t0 < f / 50) { }
	const uint64_t c1 = SYSREG_R("pmccntr_el0"), t1 = SYSREG_R("cntvct_el0");
	return (t1 - t0) ? ((c1 - c0) * f) / (t1 - t0) / 1000000 : 0;
}

/* ── 안정성 시험: 같은 버퍼의 CRC32 를 반복해 전부 같아야 한다 ─────────── */
static uint32_t crc32_sw(const uint8_t *p, size_t n)
{
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
	}
	return ~c;
}

#define STAB_SZ   (1u << 20)
#define STAB_REPS 50

static int stability(const uint8_t *buf, uint32_t ref, int reps)
{
	int bad = 0;
	for (int r = 0; r < reps; r++) {
		if (crc32_sw(buf, STAB_SZ) != ref) { bad++; }
	}
	return bad;
}

/* ── APLL ───────────────────────────────────────────────────────────── */
/* rk3036_pll_set_rate 순서. fbdiv 만 바꾼다(postdiv1 2 · refdiv 1 · postdiv2 1 ·
 * dsmpd 1 은 1104·1416 이 같다 — L9a 실측 con1 0x1441 이 이미 그 값이다).
 * 0 = lock 됨, -ETIMEDOUT = 10 ms 안에 lock 안 됨 */
static int apll_set_fbdiv(uint32_t fbdiv)
{
	cw(CRU_MODE_CON, 0x00030000);			/* slow (24 MHz OSC) */
	cw(CRU_APLL_CON1, 0x20002000);			/* power down */
	cw(CRU_APLL_CON0, 0x7fff0000 | (2u << 12) | fbdiv); /* postdiv1 2 · fbdiv */
	cw(CRU_APLL_CON1, 0x20000000);			/* power up */
	for (int t = 0; t < 10000; t++) {		/* 10 ms */
		if (cr(CRU_APLL_CON1) & (1u << 10)) {
			cw(CRU_MODE_CON, 0x00030001);	/* NORMAL — **잠긴 뒤에만** */
			return 0;
		}
		k_busy_wait(1);
	}
	/* ⚠ lock 이 안 됐으면 **SLOW(24 MHz)에 머문다.** 잠기지 않은 PLL 로
	 *   코어를 돌리지 않는다. 느리지만 살아 있고, 호출자가 1104 로 되돌린다.
	 *   (U-Boot 의 rk3036_pll_set_rate 는 시간초과 없이 영원히 기다린다) */
	return -ETIMEDOUT;
}

/* 표 원문: RK3568_CPUCLK_RATE(1104000000, 1, 3) · (1416000000, 1, 5)
 * GICCLK·ATCLK = clksel_con[3] · PERIPHCLK·PCLK = clksel_con[4]
 * (aclk 는 둘 다 1 이고 L9a 실측 clksel_con[5]=0x0111 이 이미 1 이다) */
static void core_divs(uint32_t d)
{
	cw(CRU_CLKSEL(3), 0x1f1f0000 | (d << 8) | d);
	cw(CRU_CLKSEL(4), 0x1f1f0000 | (d << 8) | d);
}

static void to_1104(void)
{
	/* 내릴 때: PLL 먼저, 분주 나중 (rk3568_armclk_set_clk) */
	(void)apll_set_fbdiv(92);
	core_divs(3);
}

int main(void)
{
	uint8_t v[3];
	int err;

	printk("=== L9b: Zephyr 에서 I2C0 직접 — vdd_cpu 1.025 V, 유지되면 1416 MHz ===\n");
	device_map(&i2c, I2C0_BASE, 0x1000, K_MEM_CACHE_NONE);
	device_map(&cru, CRU_BASE, 0x1000, K_MEM_CACHE_NONE);

	printk("  [0] 실측 CPU %" PRIu64 " MHz · APLL con0 0x%08x con1 0x%08x\n",
	       cpu_mhz(), cr(CRU_APLL_CON0), cr(CRU_APLL_CON1));
	printk("      clksel_con[3] 0x%08x · [4] 0x%08x · [5] 0x%08x\n",
	       cr(CRU_CLKSEL(3)), cr(CRU_CLKSEL(4)), cr(CRU_CLKSEL(5)));

	/* ── [1] 읽기 먼저 ─────────────────────────────────────────────── */
	const uint32_t clkdiv = ir(I2C_CLKDIV);
	printk("  [1] I2C0 CLKDIV 0x%08x (U-Boot 가 설정해 둔 값 — 건드리지 않는다)\n", clkdiv);
	if (clkdiv == 0) {
		printk("      ⚠⚠ CLKDIV 가 0 이다 — 버스가 설정되지 않았다. **쓰지 않고 중단**\n");
		goto halt;
	}
	err = read3(v);
	if (err) {
		printk("      ⚠⚠ 읽기 실패 (%d) — **쓰지 않고 중단**\n", err);
		goto halt;
	}
	printk("      VSEL0 0x%02x (%u µV) · VSEL1 0x%02x (%u µV) · CONTROL 0x%02x\n",
	       v[0], rk8600_uv(v[0]), v[1], rk8600_uv(v[1]), v[2]);
	printk("      ─ boot.cmd 의 중계값(L9a: VSEL0 0x8f · VSEL1 0x99)과 같으면\n"
	       "        이 I2C 경로가 U-Boot 와 같은 것을 읽는다는 확인이다\n");
	if (!(v[0] & 0x80) || !(v[1] & 0x80)) {
		printk("      ⚠⚠ EN 비트가 0 인 VSEL 이 있다 — 예상과 다르다. **쓰지 않고 중단**\n");
		goto halt;
	}

	/* ── [2] 쓰기 → 즉시 읽기 ──────────────────────────────────────── */
	printk("  [2] 0x%02x 를 VSEL0·VSEL1 에 쓴다 (%u µV)\n", VSEL_TARGET,
	       rk8600_uv(VSEL_TARGET));
	err = rk8600_write(VSEL0, VSEL_TARGET);
	printk("      VSEL0 쓰기: %s (%d)\n", err ? "⚠ 실패" : "OK", err);
	if (err) { goto halt; }
	err = rk8600_write(VSEL1, VSEL_TARGET);
	printk("      VSEL1 쓰기: %s (%d)\n", err ? "⚠ 실패" : "OK", err);
	if (err) { goto halt; }
	k_busy_wait(1000);	/* ramp 2300 µV/µs → 125 mV 는 약 54 µs. 넉넉히 1 ms */
	err = read3(v);
	printk("      즉시 읽기: VSEL0 0x%02x · VSEL1 0x%02x · CONTROL 0x%02x (%s)\n",
	       v[0], v[1], v[2], err ? "⚠ 읽기 실패" : "OK");
	if (err) { goto halt; }

	/* ── [3] 120초 관찰 ────────────────────────────────────────────── */
	printk("  [3] 120초 관찰 — 250 ms 마다 읽고 **변화만** 시각과 함께 찍는다\n");
	printk("      (U-Boot 실험의 창은 약 60초였다. 그보다 길게 본다)\n");
	uint8_t last0 = v[0], last1 = v[1];
	bool held = (v[0] == VSEL_TARGET) && (v[1] == VSEL_TARGET);
	const int64_t t_start = k_uptime_get();
	int reads = 0, rerr = 0;

	while (k_uptime_get() - t_start < 120000) {
		k_msleep(250);
		if (read3(v)) { rerr++; held = false; continue; }
		reads++;
		if (v[0] != last0 || v[1] != last1) {
			printk("      %6lld ms: VSEL0 0x%02x→0x%02x · VSEL1 0x%02x→0x%02x\n",
			       k_uptime_get() - t_start, last0, v[0], last1, v[1]);
			last0 = v[0];
			last1 = v[1];
		}
		if (v[0] != VSEL_TARGET || v[1] != VSEL_TARGET) { held = false; }
		if ((reads % 40) == 0) {
			printk("      %6lld ms: (경과) VSEL0 0x%02x · VSEL1 0x%02x\n",
			       k_uptime_get() - t_start, v[0], v[1]);
		}
	}
	printk("      읽기 %d 회 · 실패 %d 회 · 끝 상태 VSEL0 0x%02x · VSEL1 0x%02x\n",
	       reads, rerr, last0, last1);

	if (!held) {
		printk("  ⚠⚠ **두 VSEL 이 창 내내 0x%02x 로 유지되지 않았다.** 클럭을 올리지 않는다\n",
		       VSEL_TARGET);
		if (last0 != VSEL_TARGET && last1 == VSEL_TARGET) {
			printk("      → Zephyr 안에서도 VSEL0 이 되돌아갔다 = **칩이 되돌린다**\n");
		}
		goto halt;
	}
	printk("      ✓ 120초 내내 유지됐다 → U-Boot 실험의 되돌림은 **U-Boot 쪽**이었다\n");

	/* ── [4] 1416 MHz ──────────────────────────────────────────────── */
	uint8_t *buf = malloc(STAB_SZ);
	if (!buf) { printk("      ⚠ 버퍼 할당 실패\n"); goto halt; }
	for (size_t i = 0; i < STAB_SZ; i++) { buf[i] = (uint8_t)((i * 2654435761u) >> 24); }
	const uint32_t ref = crc32_sw(buf, STAB_SZ);
	printk("  [4] 1104 MHz 기준 CRC32 0x%08x\n", ref);

	/* 전압을 **바로 직전에** 한 번 더 확인한다 */
	if (read3(v) || v[0] != VSEL_TARGET || v[1] != VSEL_TARGET) {
		printk("      ⚠⚠ 직전 확인 실패 (VSEL0 0x%02x · VSEL1 0x%02x) — 올리지 않는다\n",
		       v[0], v[1]);
		goto halt;
	}
	unsigned int key = irq_lock();
	core_divs(5);			/* 올릴 때: 분주 먼저 */
	err = apll_set_fbdiv(118);	/* 그다음 PLL */
	irq_unlock(key);
	if (err) {
		printk("      ⚠⚠ APLL 이 10 ms 안에 lock 되지 않았다 — 1104 로 되돌린다\n");
		key = irq_lock(); to_1104(); irq_unlock(key);
		goto halt;
	}
	const uint64_t mhz = cpu_mhz();
	printk("      실측 CPU **%" PRIu64 " MHz** · con0 0x%08x · con1 0x%08x\n",
	       mhz, cr(CRU_APLL_CON0), cr(CRU_APLL_CON1));

	/* ── [5] 안정성 ────────────────────────────────────────────────── */
	printk("  [5] 안정성 — 1 MiB CRC32 를 %d 회, 전부 0x%08x 여야 한다\n", STAB_REPS, ref);
	const int bad = stability(buf, ref, STAB_REPS);
	(void)read3(v);
	printk("      불일치 %d / %d · VSEL0 0x%02x · VSEL1 0x%02x\n", bad, STAB_REPS, v[0], v[1]);
	if (bad) {
		printk("  ⚠⚠ **불안정하다.** 즉시 1104 로 되돌린다\n");
		key = irq_lock(); to_1104(); irq_unlock(key);
		printk("      되돌린 뒤 실측 %" PRIu64 " MHz\n", cpu_mhz());
		goto halt;
	}
	printk("  ✓✓ **1416 MHz 에서 안정** (실측 %" PRIu64 " MHz, 불일치 0)\n", mhz);
	printk("      다음: 이 순서(I2C 전압 → 분주 → PLL)를 부팅 경로에 넣고 L6c 를 잰다\n");
	free(buf);

	/* ⚠ 측정이 끝났으면 **1104 로 내리고** 멈춘다. 1416 에 머문 채 k_sleep 하면,
	 *   만약 VSEL0 이 나중에 되돌아갈 때(그리고 VSEL0 이 실제 레일이면) 무한정
	 *   저전압으로 앉아 있게 된다. 1104 는 어느 쪽 해석이든 전압이 충분하다. */
	key = irq_lock(); to_1104(); irq_unlock(key);
	printk("      멈추기 전에 1104 로 내렸다 — 실측 %" PRIu64 " MHz\n", cpu_mhz());

halt:
	printk("=== L9b 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru(1104 MHz)로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
