/*
 * clk1416 — 부팅 뒤 Zephyr 에서 vdd_cpu 1.025 V → APLL 1416 MHz 로 올린다.
 *
 * 출처: apps/llm_l9b (2026-11-08 실기: VSEL 120 초 유지 · 실측 1415 MHz ·
 * CRC 50/50). 헬퍼는 L9b 에서 **그대로 복사**했다 — L9b 는 그 기록의 재현을
 * 위해 고치지 않는다. 원문 대조는 L9b 머리 주석과 두 번의 검토 커밋에 있다.
 *
 * 다른 점:
 *   - 120 초 관찰 대신 짧은 확인(2 초, 100 ms 마다). 유지 여부는 L9b 가 봤다.
 *   - 올린 뒤 **감시 스레드**가 500 ms 마다 VSEL 을 읽는다. 읽기 실패이거나
 *     0x99 가 아니면 즉시 1104 로 내린다 (저전압 1416 에 머물지 않게).
 *   - 안정성 판정은 호출자 몫이다 (L6c 는 로짓 12/12 일치가 곧 시험이다).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <inttypes.h>
#include "clk1416.h"

#ifndef CONFIG_BOARD_RADXA_ZERO_3E
#error "clk1416 은 실기(radxa_zero_3e) 전용이다"
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

static __unused uint32_t rk8600_uv(uint8_t r) { return 712500u + 12500u * (uint32_t)(r & 0x3f); }

/* ── CRU (u-boot cru_rk3568.h · clk_pll.c · clk_rk3568.c) ───────────── */
#define CRU_APLL_CON0 0x00
#define CRU_APLL_CON1 0x04
#define CRU_MODE_CON  0xc0
#define CRU_CLKSEL(n) (0x100 + 4 * (n))

static mm_reg_t i2c, cru;
static uint64_t cpu_mhz(void);

/* 진단: power down 뒤 lock 이 떨어지기까지 · power up 뒤 잠기기까지 (µs) */
static int apll_lockdrop_us = -1, apll_lock_us = -1;

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

/* ── APLL ───────────────────────────────────────────────────────────── */
/* rk3036_pll_set_rate 순서. fbdiv 만 바꾼다(postdiv1 2 · refdiv 1 · postdiv2 1 ·
 * dsmpd 1 은 1104·1416 이 같다 — L9a 실측 con1 0x1441 이 이미 그 값이다).
 * 0 = lock 됨, -ETIMEDOUT = 10 ms 안에 lock 안 됨 */
static int apll_set_fbdiv(uint32_t fbdiv)
{
	/* MODE 마스크는 원문처럼 0x1 (PLL_RK3328_MODE_MASK). 전 판은 0x3 이었다 —
	 * 결과는 같았지만(bit1 이 0) 원문보다 넓게 쓸 이유가 없다. 검토 지적. */
	cw(CRU_MODE_CON, 0x00010000);			/* slow (24 MHz OSC) */
	cw(CRU_APLL_CON1, 0x20002000);			/* power down */

	/* ⚠ 검토 지적 [올리면 안 됨]: power down 직후 lock 비트가 **즉시** 0 이
	 *   되는지 보장이 없다. 지금 lock 은 1 이다. 떨어지는 데 시간이 걸리면
	 *   아래 폴링이 **이전의 lock=1** 을 보고 잠기지 않은 PLL 로 NORMAL 에
	 *   들어갈 수 있다. U-Boot 원문도 같은 순서라 실제 가능성은 낮지만,
	 *   확인 비용은 몇 줄이고 틀리면 "1416 에서 불안정했다" 는 잘못된
	 *   결론을 낸다. 그래서 **떨어지는 것을 확인한다** (최대 1 ms). */
	int t;
	for (t = 0; t < 1000 && (cr(CRU_APLL_CON1) & (1u << 10)); t++) {
		k_busy_wait(1);
	}
	apll_lockdrop_us = t;

	cw(CRU_APLL_CON0, 0x7fff0000 | (2u << 12) | fbdiv); /* postdiv1 2 · fbdiv */
	cw(CRU_APLL_CON1, 0x20000000);			/* power up */
	k_busy_wait(2);					/* 폴링 전에 2 µs (검토 제안) */
	for (t = 0; t < 10000; t++) {			/* 10 ms */
		if (cr(CRU_APLL_CON1) & (1u << 10)) {
			k_busy_wait(100);		/* lock 직후 100 µs (검토 제안) */
			cw(CRU_MODE_CON, 0x00010001);	/* NORMAL — **잠긴 뒤에만** */
			apll_lock_us = t;
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

static int to_1104(void)
{
	/* 내릴 때: PLL 먼저, 분주 나중 (rk3568_armclk_set_clk) */
	const int err = apll_set_fbdiv(92);
	core_divs(3);
	return err;
}

/* ── 공개 함수 ──────────────────────────────────────────────────────── */
static struct k_spinlock clk_lock;	/* 올림·내림이 겹치지 않게 */
static volatile int clk_state;		/* 0 = 1104 · 1 = 1416 */
static volatile uint32_t guard_reads, guard_fail;

static int set_1104_locked(void)
{
	k_spinlock_key_t k = k_spin_lock(&clk_lock);
	int err = 0;

	if (clk_state == 1) {
		err = to_1104();
		clk_state = 0;
	}
	k_spin_unlock(&clk_lock, k);
	return err;
}

void clk1416_back_to_1104(const char *why)
{
	const int was = clk_state;
	const int err = set_1104_locked();

	if (was) {
		printk("  [clk] %s → 1104 로 내림: %s · 실측 %" PRIu64 " MHz\n", why,
		       err ? "⚠ lock 안 됨 — SLOW(24 MHz)" : "OK", cpu_mhz());
	}
}

#define GUARD_STACK 2048
K_THREAD_STACK_DEFINE(guard_stack, GUARD_STACK);
static struct k_thread guard_thread;

static void guard_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	uint8_t v[3];

	while (clk_state == 1) {
		k_msleep(500);
		if (clk_state != 1) { break; }
		const int re = read3(v);
		guard_reads++;
		if (re || v[0] != VSEL_TARGET || v[1] != VSEL_TARGET) {
			guard_fail = 1;
			clk1416_back_to_1104(re ? "감시: VSEL 읽기 실패" : "감시: VSEL 이 바뀌었다");
			printk("        (읽기 %d · VSEL0 0x%02x · VSEL1 0x%02x)\n", re, v[0], v[1]);
			break;
		}
	}
}

int clk1416_raise(void)
{
	uint8_t v[3];
	int err;

	device_map(&i2c, I2C0_BASE, 0x1000, K_MEM_CACHE_NONE);
	device_map(&cru, CRU_BASE, 0x1000, K_MEM_CACHE_NONE);

	const uint32_t c0 = cr(CRU_APLL_CON0), c1 = cr(CRU_APLL_CON1);
	printk("  [clk] 시작 %" PRIu64 " MHz · con0 0x%04x con1 0x%04x\n", cpu_mhz(), c0, c1);
	if ((c0 & 0x7fff) != 0x205c || (c1 & 0x11ff) != 0x1041) {
		printk("  [clk] ⚠ APLL 이 1104 판이 아니다 — 아무것도 쓰지 않는다\n");
		return -EINVAL;
	}
	if (ir(I2C_CLKDIV) == 0) {
		printk("  [clk] ⚠ I2C0 CLKDIV 0 — 쓰지 않는다\n");
		return -EINVAL;
	}
	err = read3(v);
	if (err || !(v[0] & 0x80) || !(v[1] & 0x80)) {
		printk("  [clk] ⚠ 첫 읽기 %d · VSEL0 0x%02x · VSEL1 0x%02x — 쓰지 않는다\n",
		       err, v[0], v[1]);
		return err ? err : -EIO;
	}
	printk("  [clk] 읽기 VSEL0 0x%02x · VSEL1 0x%02x · CONTROL 0x%02x\n", v[0], v[1], v[2]);

	if (rk8600_write(VSEL0, VSEL_TARGET) || rk8600_write(VSEL1, VSEL_TARGET)) {
		printk("  [clk] ⚠ VSEL 쓰기 실패 — 1104 그대로\n");
		return -EIO;
	}
	/* 2 초 짧은 확인 (L9b 가 120 초 유지를 봤다) */
	for (int i = 0; i < 20; i++) {
		k_msleep(100);
		if (read3(v) || v[0] != VSEL_TARGET || v[1] != VSEL_TARGET) {
			printk("  [clk] ⚠ %d00 ms 에 VSEL 0x%02x/0x%02x — 1104 그대로\n",
			       i + 1, v[0], v[1]);
			return -EIO;
		}
	}
	printk("  [clk] VSEL0·VSEL1 0x99 (1025000 µV) 2 초 유지\n");

	k_spinlock_key_t k = k_spin_lock(&clk_lock);
	core_divs(5);				/* 올릴 때: 분주 먼저 */
	err = apll_set_fbdiv(118);
	if (err) {
		(void)to_1104();
		k_spin_unlock(&clk_lock, k);
		printk("  [clk] ⚠ 1416 lock 시간초과 — 1104 로\n");
		return err;
	}
	clk_state = 1;
	k_spin_unlock(&clk_lock, k);

	const uint64_t mhz = cpu_mhz();
	printk("  [clk] **실측 %" PRIu64 " MHz** · con0 0x%04x · lock 떨어짐 %d µs · 잠김 %d µs\n",
	       mhz, cr(CRU_APLL_CON0), apll_lockdrop_us, apll_lock_us);
	if (mhz < 1380 || mhz > 1450) {
		clk1416_back_to_1104("실측이 1380~1450 밖");
		return -ERANGE;
	}
	/* 다시 올릴 때: 앞 감시 스레드가 아직 끝나지 않았으면(최대 500 ms) 기다린다 —
	 * 살아 있는 스레드 구조체에 k_thread_create 를 하면 안 된다 */
	static bool guard_started;
	if (guard_started) { (void)k_thread_join(&guard_thread, K_SECONDS(2)); }
	guard_started = true;
	k_thread_create(&guard_thread, guard_stack, GUARD_STACK, guard_fn,
			NULL, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
	k_thread_name_set(&guard_thread, "clk_guard");
	return 0;
}

int clk1416_is_1416(void) { return clk_state == 1; }
unsigned clk1416_guard_reads(void) { return guard_reads; }
unsigned clk1416_guard_fail(void) { return guard_fail; }
unsigned clk1416_cpu_mhz(void) { return (unsigned)cpu_mhz(); }

void clk1416_report(void)
{
	printk("  [clk] 지금 %s · 실측 %" PRIu64 " MHz · 감시 읽기 %u 회 · 이상 %u\n",
	       clk_state ? "1416" : "1104", cpu_mhz(), guard_reads, guard_fail);
}
