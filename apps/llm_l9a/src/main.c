/* L9a — 클럭·전압의 **현재 상태를 읽는다. 아무것도 쓰지 않는다.**
 *
 * ── 왜 이 칸이 필요한가 ───────────────────────────────────────────────
 *
 * 남은 성능 레버는 CPU 클럭 하나다 (816 → 1608 MHz, 1.97배). 그런데 그것은
 * **전압 레귤레이터를 I2C 로 올려야** 하고, 되돌리기가 카드 재작성이다.
 * 쓰기 전에 **추측을 측정으로 바꿔야 할 것이 셋** 있다:
 *
 *   (1) 지금 vdd_cpu 가 몇 V 인가
 *   (2) **어느 VSEL 레지스터가 살아 있는가** — fan53555 는 VSEL0(0x00)과
 *       VSEL1(0x01) 둘을 갖고, 어느 쪽이 활성인지는 **칩의 VSEL 핀**이
 *       결정한다. 소스로는 알 수 없다(회로도가 필요하다). 틀리면 전압이
 *       안 올라간 채 클럭이 올라간다 — 저전압 불안정이고 조용한 실패다
 *   (3) 지금 APLL 이 어떻게 구성돼 있는가
 *
 * ── 설계 모순을 어떻게 풀었나 ─────────────────────────────────────────
 *
 * U-Boot 는 i2c 명령이 있지만 **콘솔이 1.5 Mbps 라 출력을 한 줄도 읽을 수
 * 없다**(docs/hardware-verified.md). Zephyr 는 출력이 읽히지만 이 보드에
 * **I2C 드라이버가 없다**.
 *
 * 해법: U-Boot 의 `i2c read <chip> <addr> <len> <memaddr>` 는 읽은 값을
 * **메모리에 넣는다.** boot.cmd 가 그렇게 해 두면 Zephyr 가 그 메모리를
 * 읽어 UART 로 찍는다. **Zephyr I2C 드라이버가 필요 없다.**
 *
 * CRU 는 메모리 맵이므로 Zephyr 가 직접 읽는다 — 중계가 필요 없다.
 *
 * ── 확정된 주소 (출처 둘 이상) ────────────────────────────────────────
 *
 *   CRU   0xfdd20000   Linux rk356x-base.dtsi 의 cru: 노드 reg
 *                    + TF-A plat/rockchip/rk3568/rk3568_def.h 의 CRU_BASE
 *   I2C0  0xfdd40000   같은 두 출처
 *   APLL = pll[0] = CRU+0x00  (u-boot cru_rk3568.h 의 struct rk3568_cru,
 *                              check_member(mode_con00, 0xc0) 로 확인)
 *
 * ⚠ 이 앱은 **읽기만 한다.** sys_write32 를 한 번도 부르지 않는다.
 * ⚠ 실기 전용이다 — QEMU 에는 이 주소들이 없다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <inttypes.h>

#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })

/* boot.cmd 가 값을 놓아 두는 자리.
 * 0x51000000 은 sram0(0x40000000~0x50000000) 밖 · OTA 슬롯
 * (0x50000000 + 8 MiB) 밖 · 모델 창(0x58000000~) 밖이다. DRAM 안이다. */
#define RELAY_BASE   0x51000000UL
#define RELAY_MAGIC  0x4d4f5255UL	/* "MORU" — boot.cmd 블록이 돌았다는 표식 */
#define RELAY_OFF_MAGIC  0x00
#define RELAY_OFF_I2C    0x08		/* VSEL0 · VSEL1 · CONTROL 3바이트 */
#define RELAY_FILL   0xdeadbeefUL	/* i2c read 가 덮어쓸 자리의 초기값 */

#define CRU_BASE     0xfdd20000UL
#define CRU_APLL_CON0  0x00
#define CRU_APLL_CON1  0x04
#define CRU_APLL_CON2  0x08
#define CRU_MODE_CON   0xc0
#define CRU_CLKSEL(n)  (0x100 + 4 * (n))

/* fan53555 / RK8600: V(µV) = 712500 + 12500 × sel, sel = bits[5:0] */
static uint32_t rk8600_uv(uint8_t reg)
{
	return 712500u + 12500u * (uint32_t)(reg & 0x3f);
}

int main(void)
{
	const uint64_t cntfrq = SYSREG_R("cntfrq_el0");

	printk("=== L9a: 클럭·전압 현재 상태 (읽기만 한다) ===\n");
	printk("  ⚠ 이 앱은 sys_write32 를 한 번도 부르지 않는다.\n");

	/* ── [0] PMU 로 실측 주파수 ──────────────────────────────────────── */
	uint64_t hz = 0;
	{
		uint64_t v = SYSREG_R("pmcr_el0");
		v &= ~(uint64_t)(1U << 3);			/* D(64분주) 지움 */
		v |= (1U << 0) | (1U << 2) | (1U << 6);		/* E | C | LC */
		__asm__ volatile("msr pmcr_el0, %0" ::"r"(v));
		__asm__ volatile("msr pmcntenset_el0, %0" ::"r"((uint64_t)1 << 31));
		__asm__ volatile("isb");

		const uint64_t t0 = SYSREG_R("cntvct_el0");
		const uint64_t c0 = SYSREG_R("pmccntr_el0");
		while (SYSREG_R("cntvct_el0") - t0 < cntfrq / 50) { /* 20 ms */ }
		const uint64_t c1 = SYSREG_R("pmccntr_el0");
		const uint64_t t1 = SYSREG_R("cntvct_el0");
		hz = (t1 - t0) ? ((c1 - c0) * cntfrq) / (t1 - t0) : 0;
		printk("  [0] 실측 CPU **%" PRIu64 " MHz** (PMU/타이머) · CNTFRQ %"
		       PRIu64 " Hz\n", hz / 1000000, cntfrq);
	}

	/* ── [1] U-Boot 가 중계해 둔 I2C 값 ─────────────────────────────── */
	printk("  [1] vdd_cpu (U-Boot 의 i2c read 가 메모리에 놓아 둔 값)\n");
	{
		mm_reg_t relay;

		device_map(&relay, RELAY_BASE, 0x1000, K_MEM_CACHE_NONE);
		const uint32_t magic = sys_read32(relay + RELAY_OFF_MAGIC);
		const uint32_t raw   = sys_read32(relay + RELAY_OFF_I2C);

		printk("      표식 0x%08x %s\n", magic,
		       (magic == RELAY_MAGIC) ? "(\"MORU\" — boot.cmd 블록이 돌았다)"
					      : "⚠ **boot.cmd 블록이 돌지 않았다**");
		if (magic != RELAY_MAGIC) {
			printk("      → boot.scr 이 이 판이 아니거나, mw.l 이 실패했다.\n");
			printk("        아래 I2C 값은 **쓰레기다. 읽지 않는다**\n");
		} else if (raw == RELAY_FILL) {
			printk("      0x%08x = 초기값 그대로 → ⚠ **i2c 명령이 없거나 실패**\n", raw);
			printk("        이 U-Boot 에 i2c 가 안 들어 있을 수 있다.\n");
			printk("        그러면 전압 경로를 다시 설계해야 한다\n");
		} else {
			const uint8_t vsel0 = (uint8_t)(raw & 0xff);
			const uint8_t vsel1 = (uint8_t)((raw >> 8) & 0xff);
			const uint8_t ctrl  = (uint8_t)((raw >> 16) & 0xff);

			printk("      원시 0x%08x\n", raw);
			printk("      VSEL0 (0x00) = 0x%02x  EN=%u MODE=%u sel=%2u → %u µV\n",
			       vsel0, (unsigned)((vsel0 >> 7) & 1),
			       (unsigned)((vsel0 >> 6) & 1), (unsigned)(vsel0 & 0x3f),
			       rk8600_uv(vsel0));
			printk("      VSEL1 (0x01) = 0x%02x  EN=%u MODE=%u sel=%2u → %u µV\n",
			       vsel1, (unsigned)((vsel1 >> 7) & 1),
			       (unsigned)((vsel1 >> 6) & 1), (unsigned)(vsel1 & 0x3f),
			       rk8600_uv(vsel1));
			printk("      CONTROL (0x02) = 0x%02x  (slew = bits[6:4] = %u)\n",
			       ctrl, (unsigned)((ctrl >> 4) & 7));
			printk("      ─ 읽는 법: **850000 µV 근처(sel 11)인 쪽이 살아 있는 쪽**이다.\n");
			printk("        U-Boot 가 816 MHz 로 두었으니 그 전압이어야 한다\n");
			printk("        1.10 V 는 sel 31 · 1.15 V 는 sel 35 다\n");
		}
	}

	/* ── [2] APLL 과 코어 먹싱 ───────────────────────────────────────── */
	printk("  [2] APLL · 코어 먹싱 (CRU 0x%08lx — Zephyr 가 직접 읽는다)\n",
	       (unsigned long)CRU_BASE);
	{
		mm_reg_t cru;

		device_map(&cru, CRU_BASE, 0x1000, K_MEM_CACHE_NONE);
		const uint32_t c0 = sys_read32(cru + CRU_APLL_CON0);
		const uint32_t c1 = sys_read32(cru + CRU_APLL_CON1);
		const uint32_t c2 = sys_read32(cru + CRU_APLL_CON2);
		const uint32_t md = sys_read32(cru + CRU_MODE_CON);

		const uint32_t fbdiv    = c0 & 0xfff;
		const uint32_t postdiv1 = (c0 >> 12) & 0x7;
		const uint32_t refdiv   = c1 & 0x3f;
		const uint32_t postdiv2 = (c1 >> 6) & 0x7;
		const uint32_t dsmpd    = (c1 >> 12) & 1;
		const uint32_t pwrdown  = (c1 >> 13) & 1;
		const uint32_t lock     = (c1 >> 10) & 1;
		const uint32_t mode     = md & 0x3;

		printk("      con0 0x%08x · con1 0x%08x · con2 0x%08x · MODE_CON 0x%08x\n",
		       c0, c1, c2, md);
		printk("      fbdiv %u · postdiv1 %u · refdiv %u · postdiv2 %u · "
		       "dsmpd %u · pwrdown %u · **lock %u**\n",
		       fbdiv, postdiv1, refdiv, postdiv2, dsmpd, pwrdown, lock);
		printk("      APLL 모드 %u (%s)\n", mode,
		       (mode == 0) ? "SLOW" : (mode == 1) ? "NORMAL" : "DEEP/예약");

		const uint32_t den = refdiv * postdiv1 * postdiv2;
		if (den) {
			const uint32_t vco = 24u * fbdiv / (refdiv * postdiv1);
			printk("      계산: VCO %u MHz · APLL 출력 **%u MHz**"
			       "  (24×fbdiv/(refdiv×pd1×pd2))\n",
			       vco, 24u * fbdiv / den);
			printk("      ⚠ 표 원문 대조: 816 = (ref 1, fb 68, pd1 2, pd2 1) ·\n");
			printk("        **1608 = (ref 1, fb 67, pd1 1, pd2 1)** ← 목표\n");
		}
		for (int n = 0; n <= 5; n++) {
			printk("      clksel_con[%d] = 0x%08x\n", n,
			       sys_read32(cru + CRU_CLKSEL(n)));
		}
		printk("      ─ clksel_con[0] bit7 = CLK_CORE_PRE_SEL (0=SRC 1=APLL)\n");
		printk("        clksel_con[2] bits[9:8] = SRC_SEL (0=APLL) · "
		       "bits[4:0] = SRC_DIV\n");
	}

	printk("  [3] 판정 — 쓰기 전에 이 셋이 측정값이어야 한다\n");
	printk("      · 어느 VSEL 이 살아 있나 → [1] 에서 850000 µV 근처인 쪽\n");
	printk("      · 지금 APLL 출력이 실측 %" PRIu64 " MHz 와 맞나 → [0] 대 [2]\n",
	       hz / 1000000);
	printk("      · i2c 중계가 되나 → [1] 의 표식과 초기값\n");
	printk("      이 셋이 맞으면 다음은 **전압부터** 올린다 (1.10 V, sel 31).\n");
	printk("      순서가 반대면 저전압 불안정이다\n");
	printk("=== L9a 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
