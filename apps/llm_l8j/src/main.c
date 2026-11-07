/* L8j — uart2 인터럽트 폭주의 **근본 원인**을 가린다.
 *
 * L8i 가 범인을 지목했다: uart2(GIC_SPI 118 = INTID 150)의 인터럽트가 계속
 * 걸려 있고 아무도 지우지 않아 사이클의 약 77% 를 ISR 이 먹는다.
 * 사다리 앱은 CONFIG_UART_INTERRUPT_DRIVEN=n 으로 끄면 되고, 실제로 L6c 가
 * 토큰당 23.6초 → 1.423초(16.6배)가 됐다.
 *
 * ⚠ 그런데 **moru 는 끌 수 없다** — 셸이 인터럽트 구동 UART 를 요구한다.
 *   제품 쪽 셸이 지금 77% 를 잃고 있다. 그래서 근본 원인을 가려야 한다.
 *
 * ── 가설: DesignWare 의 "busy detect" 인터럽트 ────────────────────────
 *
 * RK3566 의 UART 는 표준 16550 이 아니라 **DesignWare DW_APB_UART** 다
 * (DTS compatible 이 "rockchip,rk3568-uart","ns16550" 이라 Zephyr 는 범용
 * ns16550 드라이버를 쓴다). DW_APB_UART 에는 표준에 **없는** 인터럽트가
 * 하나 있다:
 *
 *     IIR[3:0] = 0x7  "busy detect"
 *
 * 이것은 **UART Status Register(USR, 오프셋 0x7C)를 읽어야만 지워진다.**
 * LSR 을 읽거나 IIR 을 읽는 것으로는 지워지지 않는다. UART 가 전송 중일 때
 * LCR 을 쓰면(= 보율·프레임 설정) 이 조건이 걸린다 — U-Boot 가 1.5 Mbps 로
 * 쓰고 있던 UART 를 Zephyr 가 115200 으로 다시 설정하는 바로 그 순간이다.
 *
 * Linux 는 이것을 전용 드라이버로 처리한다
 * (drivers/tty/serial/8250/8250_dw.c 의 dw8250_handle_irq 가 IIR 이 busy 를
 * 가리키면 USR 을 읽는다). **Zephyr 의 범용 ns16550 드라이버에는 그 처리가
 * 없다.** 그러면 IIR 이 0x7 에 머물고 레벨 IRQ 가 영원히 걸려 있다 —
 * 관측된 증상과 정확히 맞는다.
 *
 * ── 가리는 법 — 양성·음성 대조를 모두 둔다 ───────────────────────────
 *
 *   [2] 기준선                     폭주 상태의 ALU + EXC_TAKEN
 *   [3] **USR 을 한 번 읽는다**     회복하면 → busy detect 확정
 *   [4] irq_disable(uart2)         양성 대조. 반드시 회복해야 한다
 *   [5] irq_enable(uart2)          음성 대조. 다시 나빠져야 한다
 *                                   (안 나빠지면 [3]이 영구히 고쳤다는 뜻)
 *
 * ⚠ [4]가 회복하지 않으면 지목 자체가 틀린 것이다. 그래서 양성 대조를 둔다.
 * ⚠ [5]가 필요하다. [3] 직후 좋아진 것이 USR 때문인지 그냥 시간이 지나서인지
 *   구분해야 한다.
 *
 * ⚠ **RBR(오프셋 0x00)은 읽지 않는다.** 읽으면 수신 문자를 소비해 버린다.
 *   LSR 읽기는 오류 비트를 지우지만 그건 감수한다(정보가 더 크다).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/irq.h>
#include <inttypes.h>

#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) __asm__ volatile("msr " name ", %0" ::"r"((uint64_t)(v)))

/* ── DW_APB_UART 레지스터 (reg-shift = 2 → 색인 × 4) ──────────────────── */
#define R_RBR	0x00	/* ⚠ 읽지 않는다 — 수신 문자를 소비한다 */
#define R_IER	0x04
#define R_IIR	0x08	/* 읽기 = IIR, 쓰기 = FCR */
#define R_LCR	0x0c
#define R_MCR	0x10
#define R_LSR	0x14
#define R_MSR	0x18
#define R_SCR	0x1c
#define R_USR	0x7c	/* DesignWare 전용 — busy detect 를 지우는 유일한 길 */

#define UART2_NODE DT_NODELABEL(uart2)

#if !DT_NODE_EXISTS(UART2_NODE)
/* QEMU(qemu_cortex_a53)는 pl011 을 쓰고 uart2 노드가 없다. 이 앱은 보드의
 * DW_APB_UART 를 직접 들여다보는 것이 전부라 **실기 전용**이다. */
#error "L8j 는 실기(radxa_zero_3e) 전용이다 — uart2 노드가 필요하다"
#endif

static mm_reg_t u2;

static inline uint32_t ur(uint32_t off) { return sys_read32(u2 + off); }

static const char *iir_name(uint32_t iir)
{
	switch (iir & 0xf) {
	case 0x0: return "modem status";
	case 0x1: return "**없음** (대기 인터럽트 없음)";
	case 0x2: return "THR empty (송신 버퍼 빔)";
	case 0x4: return "RX data available";
	case 0x6: return "receiver line status";
	case 0x7: return "⚠⚠ **BUSY DETECT** (DesignWare 전용 — USR 읽기로만 지워진다)";
	case 0xc: return "character timeout";
	default:  return "(미정의)";
	}
}

static void dump_uart(const char *when)
{
	const uint32_t ier = ur(R_IER), iir = ur(R_IIR), lcr = ur(R_LCR);
	const uint32_t mcr = ur(R_MCR), lsr = ur(R_LSR), msr = ur(R_MSR);
	printk("      [%s]\n", when);
	printk("        IER 0x%02x (ERBFI=%u ETBEI=%u ELSI=%u EDSSI=%u)\n",
	       ier, (unsigned)(ier & 1), (unsigned)((ier >> 1) & 1),
	       (unsigned)((ier >> 2) & 1), (unsigned)((ier >> 3) & 1));
	printk("        IIR 0x%02x → %s\n", iir, iir_name(iir));
	printk("        LCR 0x%02x · MCR 0x%02x · LSR 0x%02x · MSR 0x%02x\n",
	       lcr, mcr, lsr, msr);
}

/* ── PMU ────────────────────────────────────────────────────────────────── */
#define EV_INST_RETIRED	0x08
#define EV_EXC_TAKEN	0x09

static void pmu_init(void)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	v &= ~(uint64_t)(1U << 3);                                  /* D 를 지운다 */
	v |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 6);          /* E|P|C|LC */
	SYSREG_W("pmcr_el0", v);
	SYSREG_W("pmevtyper0_el0", EV_INST_RETIRED);
	SYSREG_W("pmevtyper1_el0", EV_EXC_TAKEN);
	SYSREG_W("pmcntenset_el0", ((uint64_t)1 << 31) | 3);
	__asm__ volatile("isb");
}
static inline uint64_t cyc(void) { return SYSREG_R("pmccntr_el0"); }

static uint64_t __attribute__((noinline)) alu1(uint64_t n)
{
	uint64_t out;
	__asm__ volatile(
		"	mov	x9,  #1\n	mov	x10, #2\n"
		"	mov	x11, #3\n	mov	x12, #4\n"
		"	mov	x13, #5\n	mov	x14, #6\n"
		"	mov	x15, #7\n	mov	x16, #8\n"
		"	.align	4\n"
		"1:	add	x9,  x9,  #1\n	add	x10, x10, #1\n"
		"	add	x11, x11, #1\n	add	x12, x12, #1\n"
		"	add	x13, x13, #1\n	add	x14, x14, #1\n"
		"	add	x15, x15, #1\n	add	x16, x16, #1\n"
		"	subs	%[n], %[n], #1\n	cbnz	%[n], 1b\n"
		"	add	%[o], x9, x16\n"
		: [o] "=&r"(out), [n] "+&r"(n) :
		: "x9","x10","x11","x12","x13","x14","x15","x16","cc");
	return out;
}

static volatile uint64_t sink;

/* 한 구간을 재고 찍는다. 바퀴당 사이클과 EXC_TAKEN 이 판정의 전부다. */
static uint64_t phase(const char *label, uint64_t n)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	SYSREG_W("pmcr_el0", v | (1U << 1) | (1U << 2));
	__asm__ volatile("isb");
	const uint64_t c0 = cyc(), e0 = SYSREG_R("pmevcntr1_el0");
	sink += alu1(n);
	__asm__ volatile("isb");
	const uint64_t dc = cyc() - c0, de = SYSREG_R("pmevcntr1_el0") - e0;
	const uint64_t cpi100 = n ? (dc * 100) / n : 0;
	const uint64_t ipc100 = dc ? (n * 10 * 100) / dc : 0;
	printk("      %-26s 바퀴당 %" PRIu64 ".%02" PRIu64 " 사이클 · IPC %"
	       PRIu64 ".%02" PRIu64 " · EXC_TAKEN %" PRIu64 "\n",
	       label, cpi100/100, cpi100%100, ipc100/100, ipc100%100, de);
	return cpi100;
}

int main(void)
{
	const uint64_t N = 500000;
	pmu_init();

	printk("=== L8j: uart2 인터럽트 폭주의 근본 원인 ===\n");
	printk("  가설: DesignWare 의 busy-detect 인터럽트(IIR=0x7)는 USR(0x7C)\n"
	       "        읽기로만 지워진다. Zephyr 의 범용 ns16550 에는 그 처리가\n"
	       "        없다. Linux 는 8250_dw.c 의 dw8250_handle_irq 가 처리한다\n");

	device_map(&u2, DT_REG_ADDR(UART2_NODE), DT_REG_SIZE(UART2_NODE),
		   K_MEM_CACHE_NONE);
	printk("  [0] uart2 @ 0x%08x (phys) → 0x%lx (virt) · Zephyr IRQ %d\n",
	       (unsigned)DT_REG_ADDR(UART2_NODE), (unsigned long)u2,
	       DT_IRQN(UART2_NODE));

	/* ⚠ **순서가 전부다.** 기준선을 레지스터를 건드리기 **전에** 잰다.
	 *   USR 읽기는 busy detect 를 지우므로, 먼저 읽으면 기준선이 사라진다.
	 *   IIR 읽기는 busy detect 를 지우지 않으므로(DW 사양) 그 사이에 둔다 —
	 *   그래도 그 가정을 시험한다([3]).
	 */
	printk("  [1] 기준선 — uart2 레지스터를 **아직 건드리지 않았다**\n");
	(void)alu1(1000);
	const uint64_t base = phase("기준선", N);

	printk("  [2] 레지스터를 본다 (USR 은 아직 읽지 않는다)\n");
	dump_uart("폭주 중");

	printk("  [3] IIR 을 읽은 뒤 — DW 사양상 busy detect 는 **안 지워져야** 한다\n");
	const uint64_t afteriir = phase("IIR 읽은 뒤", N);

	printk("  [4] **USR(0x7C)을 읽는다** — 가설의 핵심\n");
	{
		const uint32_t usr = ur(R_USR);
		printk("        USR 0x%02x (BUSY=%u TFNF=%u TFE=%u RFNE=%u RFF=%u)\n",
		       usr, (unsigned)((usr >> 0) & 1), (unsigned)((usr >> 1) & 1),
		       (unsigned)((usr >> 2) & 1), (unsigned)((usr >> 3) & 1),
		       (unsigned)((usr >> 4) & 1));
	}
	const uint64_t afterusr = phase("USR 읽은 뒤", N);
	dump_uart("USR 읽은 뒤");

	printk("  [5] 양성 대조 — irq_disable(uart2)\n");
	irq_disable(DT_IRQN(UART2_NODE));
	const uint64_t off = phase("IRQ 끔", N);

	printk("  [6] 음성 대조 — irq_enable(uart2)\n");
	irq_enable(DT_IRQN(UART2_NODE));
	const uint64_t on = phase("IRQ 다시 켬", N);
	dump_uart("끝");

	/* ── 판정 ──────────────────────────────────────────────────────────── */
	printk("  [7] 판정\n");
	printk("      기준선 %" PRIu64 ".%02" PRIu64 " → IIR뒤 %" PRIu64 ".%02"
	       PRIu64 " → **USR뒤 %" PRIu64 ".%02" PRIu64 "** → IRQ끔 %" PRIu64
	       ".%02" PRIu64 " → 다시켬 %" PRIu64 ".%02" PRIu64
	       "  (바퀴당 사이클, 정상은 5.00)\n",
	       base/100, base%100, afteriir/100, afteriir%100,
	       afterusr/100, afterusr%100, off/100, off%100, on/100, on%100);
#define OK(x) ((x) <= 700)
	if (!OK(base)) {
		if (OK(off) && !OK(on)) {
			printk("      ✓ IRQ 를 끄면 회복하고 켜면 다시 나빠진다 → **uart2 가 맞다**\n");
		} else if (!OK(off)) {
			printk("      ⚠⚠ IRQ 를 꺼도 회복하지 않았다 → **uart2 지목이 틀렸다.**\n"
			       "         다른 IRQ 를 찾아야 한다. ICC_HPPIR1 을 다시 본다\n");
		}
		if (OK(afterusr) && !OK(afteriir)) {
			printk("      ✓✓ **USR 읽기가 폭주를 지웠다 — busy detect 확정.**\n");
			printk("         고침: ns16550 ISR 이 IIR&0xf == 0x7 일 때 USR(0x7C)을\n");
			printk("         읽어야 한다 (Linux 8250_dw.c 의 dw8250_handle_irq 와 같다)\n");
			printk("         → rockchip 전용 드라이버 또는 ns16550 에 DW 처리를 추가\n");
			printk("         ⚠ 업스트림 Zephyr 보고 거리다\n");
		} else if (OK(afteriir)) {
			printk("      → **IIR 읽기만으로 지워졌다.** busy detect 가 아니다.\n");
			printk("         ISR 이 IIR 을 읽지 않거나 늦게 읽는 쪽을 본다\n");
		} else {
			printk("      → USR 로도 안 지워졌다. busy detect 가 아니다.\n");
			printk("         위 [2] 의 IIR 값이 어느 원인인지 말해 준다\n");
		}
	} else {
		/* ⚠ 2026-11-07: 이 갈래의 뜻이 **바뀌었다.** 처음에는 "폭주가
		 *   재현되지 않았다, L8i 와 다른 점을 찾아라" 였다. 그 뒤
		 *   soc_init/dw_uart_busy.c 가 모듈 수준 POST_KERNEL priority 0
		 *   에서 USR 을 읽어 치우게 됐다. 그것이 main() 보다 먼저이므로
		 *   **고침이 돌면 기준선이 정상으로 나오는 것이 정상이다.**
		 *   옛 문구를 그대로 두면 고침이 작동하는 것을 "재현 실패" 로
		 *   오독한다. */
		printk("      ✓ 기준선이 이미 정상이다 (%" PRIu64 ".%02" PRIu64
		       "). **모듈 고침이 돌았다.**\n", base/100, base%100);
		printk("         soc_init/dw_uart_busy.c 가 POST_KERNEL 0 에서\n");
		printk("         USR 을 읽어 치운다 — main() 보다 먼저다.\n");
		printk("         IIR 이 0xc1(대기 없음)로 시작하면 그 확인이다\n");
		printk("         ⚠ 고침을 빼고 폭주를 다시 보려면 soc_init 의\n");
		printk("           CONFIG_UART_INTERRUPT_DRIVEN 게이트를 손대거나\n");
		printk("           SYS_INIT 등록을 지운다\n");
	}
#undef OK
	printk("      ⚠ 어느 경우든 **moru 에서 다시 확인**해야 한다. 이 앱은\n");
	printk("        셸이 없어 IER 설정이 moru 와 다를 수 있다\n");
	printk("=== L8j 측정 끝 ===  (sink %" PRIu64 ")\n", sink);
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
