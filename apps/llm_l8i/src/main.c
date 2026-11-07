/* L8i — **타이밍으로 추론하지 않고 PMU 이벤트로 센다.**
 *
 * L8h 가 남긴 사실: 메모리를 전혀 쓰지 않는 ALU 루프가 IPC 0.45(한 바퀴
 * 21.96 사이클 / 명령 10개)다. 그리고 측정 버퍼의 PTE 는 완벽하다
 * (MAIR 0xff Normal WB · SH=3 · AF=1). 즉 메모리 속성은 무죄다.
 *
 * ⚠ 그런데 **느려짐이 균일하지 않다.** 이것이 단일 원인 가설을 깬다:
 *      ALU            22 대 ~5 사이클   = 4.4×
 *      L1 크기 읽기   0.8 대 10~16 B/cyc = 12~20×
 *   분기 예측 실패가 64바이트마다 한 번이면 읽기는 64/15 ≈ 4 B/cyc 에서
 *   멈춘다. 0.8 이 아니다. 무엇이든 **메모리를 ALU 보다 더 세게** 때린다.
 *   하나의 원인이 아니거나, L1-I 와 L1-D 가 함께 유지되지 않는 것이다.
 *   언롤 시험만으로는 이 둘을 가를 수 없다.
 *
 * 그래서 세 가지를 **한 번의 왕복**에서 한다.
 *
 *  [A] 인터럽트 폭주를 배제한다. 아직 한 번도 배제한 적이 없다.
 *      보드 defconfig 에 CONFIG_UDC_DRIVER=y 라 모든 사다리 앱에서 dwc3 가
 *      초기화되고 IRQ 가 연결된다. U-Boot 의 preboot 가 `usb start` 로
 *      컨트롤러를 살려 둔 채 넘긴다. 레벨 IRQ 가 계속 걸려 있으면 ALU 와
 *      메모리에서 똑같이 사이클을 빼앗는다.
 *      → 같은 ALU 루프를 `daifset #0xf` 로 **막고** 한 번 더 잰다.
 *         IPC 가 1.5~2 로 뛰면 코어가 아니라 인터럽트다.
 *      → 막은 상태에서 ICC_HPPIR1_EL1 을 읽는다. 대기 중 INTID 를 **이름으로**
 *         알려 준다 (1023 = 대기 없음).
 *      ⚠ UDC_DRIVER 를 끄지 않는다. 빌드 변수와 런타임 변수를 한꺼번에
 *        바꾸면 무엇이 효과를 냈는지 모른다. EXC_TAKEN 이 뜨면 HPPIR 가
 *        출처를 알려 주므로 왕복이 더 필요 없다.
 *
 *  [B] PMU **이벤트 카운터**로 센다. A55 는 6개를 준다(PMCR_EL0.N).
 *      전부 아키텍처 공통 이벤트라 TRM 이 필요 없다:
 *        INST_RETIRED 0x08 · L1I_CACHE_REFILL 0x01 · L1D_CACHE_REFILL 0x03
 *        L2D_CACHE_REFILL 0x17 · BR_MIS_PRED 0x10 · EXC_TAKEN 0x09
 *        STALL_FRONTEND 0x23 · STALL_BACKEND 0x24
 *      ⚠ 먼저 PMCEID0_EL0 를 읽어 **구현 여부를 확인하고** 찍는다. 없는
 *        이벤트의 0 을 "리필이 없다" 로 읽으면 또 오진한다.
 *      판정표:
 *        L1I_CACHE_REFILL ≈ 바퀴당 1 + STALL_FRONTEND 높음 → 인출이 캐시 안 됨
 *        BR_MIS_PRED ≈ 바퀴당 1                          → 예측기
 *        EXC_TAKEN ≫ 0                                   → 인터럽트
 *        STALL_BACKEND 만 높음                            → 실행 자체
 *
 *  [C] **분기가 없는** 직선 코드 1024 개 add 를 연달아 두 번 돈다.
 *      분기가 없으므로 예측기 가설은 여기서 ~0.5 사이클/명령을 예측하고,
 *      인출 비캐시 가설은 ~1.25 이상을 예측한다. 그리고 1회차와 2회차가
 *      다르면 **L1-I 가 아예 유지되지 않는다**는 뜻이다.
 *
 *  그리고 **코드 주소의 PTE** 를 본다. 지금까지 데이터 버퍼만 워크했다.
 *
 * ⚠ QEMU 는 INST_RETIRED 는 구현하지만 리필류는 구현하지 않는다 — 거기서
 *   0 이 나오는 것은 정상이고, 판정은 실기에만 적용한다.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdlib.h>
#include <inttypes.h>

#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) __asm__ volatile("msr " name ", %0" ::"r"((uint64_t)(v)))

/* ── 공통 이벤트 번호 (아키텍처 정의 — TRM 불필요) ────────────────────── */
#define EV_L1I_REFILL	0x01
#define EV_L1D_REFILL	0x03
#define EV_INST_RETIRED	0x08
#define EV_EXC_TAKEN	0x09
#define EV_BR_MIS_PRED	0x10
#define EV_L2D_REFILL	0x17
#define EV_STALL_FRONT	0x23
#define EV_STALL_BACK	0x24

#define NEV 6
static uint8_t ev_set[NEV];
static const char *ev_name[NEV];

static void pmu_init(void)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	/* ⚠ D(bit 3, 64사이클마다 1증가)를 **명시로 지운다.** 지금까지는 OR 만
	 *   했다. 815 MHz 가 나왔으니 D=0 이긴 했지만, 그 추론에 의존하지 않는다. */
	v &= ~(uint64_t)(1U << 3);
	v |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 6); /* E | P(이벤트리셋) | C | LC */
	SYSREG_W("pmcr_el0", v);
	SYSREG_W("pmcntenset_el0", (uint64_t)1 << 31);      /* 사이클 카운터 */
	__asm__ volatile("isb");
}

static void pmu_prog(int i, uint8_t ev)
{
	switch (i) {
	case 0: SYSREG_W("pmevtyper0_el0", ev); break;
	case 1: SYSREG_W("pmevtyper1_el0", ev); break;
	case 2: SYSREG_W("pmevtyper2_el0", ev); break;
	case 3: SYSREG_W("pmevtyper3_el0", ev); break;
	case 4: SYSREG_W("pmevtyper4_el0", ev); break;
	case 5: SYSREG_W("pmevtyper5_el0", ev); break;
	}
}
static uint64_t pmu_read(int i)
{
	switch (i) {
	case 0: return SYSREG_R("pmevcntr0_el0");
	case 1: return SYSREG_R("pmevcntr1_el0");
	case 2: return SYSREG_R("pmevcntr2_el0");
	case 3: return SYSREG_R("pmevcntr3_el0");
	case 4: return SYSREG_R("pmevcntr4_el0");
	case 5: return SYSREG_R("pmevcntr5_el0");
	}
	return 0;
}
static void pmu_set_events(const uint8_t *evs, const char *const *names)
{
	SYSREG_W("pmcntenclr_el0", 0x3fULL);
	for (int i = 0; i < NEV; i++) {
		ev_set[i] = evs[i];
		ev_name[i] = names[i];
		pmu_prog(i, evs[i]);
	}
	SYSREG_W("pmcntenset_el0", 0x3fULL);
	__asm__ volatile("isb");
}

static inline uint64_t cyc(void) { return SYSREG_R("pmccntr_el0"); }

struct meas { uint64_t cycles; uint64_t ev[NEV]; };

static void meas_begin(struct meas *m)
{
	/* 사이클·이벤트 카운터를 리셋한다 (PMCR.P|C) */
	uint64_t v = SYSREG_R("pmcr_el0");
	SYSREG_W("pmcr_el0", v | (1U << 1) | (1U << 2));
	__asm__ volatile("isb");
	m->cycles = cyc();
	for (int i = 0; i < NEV; i++) { m->ev[i] = pmu_read(i); }
}
static void meas_end(struct meas *m)
{
	__asm__ volatile("isb");
	m->cycles = cyc() - m->cycles;
	for (int i = 0; i < NEV; i++) { m->ev[i] = pmu_read(i) - m->ev[i]; }
}
static void meas_print(const char *label, const struct meas *m, uint64_t iters,
		       uint64_t instr_per_iter)
{
	const uint64_t ins = iters * instr_per_iter;
	const uint64_t cpi100 = iters ? (m->cycles * 100) / iters : 0;
	const uint64_t ipc100 = m->cycles ? (ins * 100) / m->cycles : 0;
	printk("      %-16s %" PRIu64 " 사이클 · 바퀴당 %" PRIu64 ".%02" PRIu64
	       " · IPC %" PRIu64 ".%02" PRIu64 "\n",
	       label, m->cycles, cpi100/100, cpi100%100, ipc100/100, ipc100%100);
	printk("                       ");
	for (int i = 0; i < NEV; i++) {
		/* 바퀴당 값을 100배로 보여 준다 — 1 근처인지가 판정의 핵심이다 */
		const uint64_t per100 = iters ? (m->ev[i] * 100) / iters : 0;
		printk("%s %" PRIu64 "(%" PRIu64 ".%02" PRIu64 "/바퀴)  ",
		       ev_name[i], m->ev[i], per100/100, per100%100);
	}
	printk("\n");
}

/* ── 워크로드 ──────────────────────────────────────────────────────────── */

/* (a) 1× 루프: 독립 add 8 + subs + cbnz = 명령 10개 */
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

/* (b) 8× 언롤: 독립 add 64 + subs + cbnz = 명령 66개
 *     분기당 비용이면 바퀴당 사이클이 (a) 와 거의 같고,
 *     명령당 비용이면 약 6.6배가 된다. */
static uint64_t __attribute__((noinline)) alu8(uint64_t n)
{
	uint64_t out;
	__asm__ volatile(
		"	mov	x9,  #1\n	mov	x10, #2\n"
		"	mov	x11, #3\n	mov	x12, #4\n"
		"	mov	x13, #5\n	mov	x14, #6\n"
		"	mov	x15, #7\n	mov	x16, #8\n"
		"	.align	4\n"
		"1:	.rept	8\n"
		"	add	x9,  x9,  #1\n	add	x10, x10, #1\n"
		"	add	x11, x11, #1\n	add	x12, x12, #1\n"
		"	add	x13, x13, #1\n	add	x14, x14, #1\n"
		"	add	x15, x15, #1\n	add	x16, x16, #1\n"
		"	.endr\n"
		"	subs	%[n], %[n], #1\n	cbnz	%[n], 1b\n"
		"	add	%[o], x9, x16\n"
		: [o] "=&r"(out), [n] "+&r"(n) :
		: "x9","x10","x11","x12","x13","x14","x15","x16","cc");
	return out;
}

/* (c) **분기 없는** 직선 코드: 독립 add 1024 개. 루프도 분기도 없다.
 *     예측기 가설   → 약 0.5 사이클/명령
 *     인출 비캐시   → 1.25 이상
 *     1회차 ≠ 2회차 → L1-I 가 유지되지 않는다 (코드 4 KiB)
 */
static uint64_t __attribute__((noinline)) alu_straight(void)
{
	uint64_t out;
	__asm__ volatile(
		"	mov	x9,  #1\n	mov	x10, #2\n"
		"	mov	x11, #3\n	mov	x12, #4\n"
		"	mov	x13, #5\n	mov	x14, #6\n"
		"	mov	x15, #7\n	mov	x16, #8\n"
		"	.align	6\n"
		"	.rept	128\n"
		"	add	x9,  x9,  #1\n	add	x10, x10, #1\n"
		"	add	x11, x11, #1\n	add	x12, x12, #1\n"
		"	add	x13, x13, #1\n	add	x14, x14, #1\n"
		"	add	x15, x15, #1\n	add	x16, x16, #1\n"
		"	.endr\n"
		"	add	%[o], x9, x16\n"
		: [o] "=&r"(out) :
		: "x9","x10","x11","x12","x13","x14","x15","x16");
	return out;
}

/* (d) 읽기 스트림 (L8g·L8h 와 같은 루프) */
static uint64_t __attribute__((noinline))
read_stream(const volatile uint64_t *p, size_t n_u64, uint64_t *out_sum)
{
	uint64_t a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,a6=0,a7=0;
	for (size_t i = 0; i + 8 <= n_u64; i += 8) {
		a0 += p[i+0]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3];
		a4 += p[i+4]; a5 += p[i+5]; a6 += p[i+6]; a7 += p[i+7];
	}
	*out_sum = a0+a1+a2+a3+a4+a5+a6+a7;
	return 0;
}

#ifdef CONFIG_BOARD_RADXA_ZERO_3E
static void walk(const char *what, uint64_t va)
{
	const uint64_t ttbr0 = SYSREG_R("ttbr0_el1") & ~0xfffULL;
	const uint64_t mair  = SYSREG_R("mair_el1");
	uint64_t tbl = ttbr0, desc = 0;

	desc = ((volatile uint64_t *)tbl)[(va >> 30) & 0x3];
	if ((desc & 3) != 3) { goto done; }
	tbl = desc & 0x0000fffffffff000ULL;
	desc = ((volatile uint64_t *)tbl)[(va >> 21) & 0x1ff];
	if ((desc & 3) == 1) { goto done; }
	if ((desc & 3) != 3) { goto done; }
	tbl = desc & 0x0000fffffffff000ULL;
	desc = ((volatile uint64_t *)tbl)[(va >> 12) & 0x1ff];
done:
	{
		const unsigned attr = (unsigned)((desc >> 2) & 7);
		const unsigned sh   = (unsigned)((desc >> 8) & 3);
		const unsigned xn   = (unsigned)((desc >> 54) & 1);  /* UXN */
		const unsigned pxn  = (unsigned)((desc >> 53) & 1);
		const unsigned mb   = (unsigned)((mair >> (8 * attr)) & 0xff);
		printk("      %-10s VA 0x%016" PRIx64 " desc 0x%016" PRIx64 "\n",
		       what, va, desc);
		printk("                 AttrIndx=%u → MAIR 0x%02x · SH=%u · PXN=%u UXN=%u%s\n",
		       attr, mb, sh, pxn, xn,
		       (mb == 0xff) ? "   (Normal WB R/W-alloc)"
				    : ((mb == 0x44) ? "   ⚠ Normal NON-CACHEABLE"
						    : "   ⚠ WB 가 아니다"));
	}
}
#endif

int main(void)
{
	const uint64_t cntfrq = SYSREG_R("cntfrq_el0");
	pmu_init();

	printk("=== L8i: PMU 이벤트로 센다 (추론이 아니라 계수) ===\n");

	/* [0] 주파수 + PMU 능력 */
	{
		const uint64_t v0 = SYSREG_R("cntvct_el0"), c0 = cyc();
		while (SYSREG_R("cntvct_el0") - v0 < cntfrq / 50) { }
		const uint64_t c1 = cyc(), v1 = SYSREG_R("cntvct_el0");
		const uint64_t hz = (v1 - v0) ? ((c1 - c0) * cntfrq) / (v1 - v0) : 0;
		const uint64_t pmcr = SYSREG_R("pmcr_el0");
		printk("  [0] CPU %" PRIu64 " MHz · PMCR 0x%08" PRIx64
		       " (N=%u 카운터, D=%u)\n",
		       hz / 1000000, pmcr, (unsigned)((pmcr >> 11) & 0x1f),
		       (unsigned)((pmcr >> 3) & 1));
		const uint64_t ceid0 = SYSREG_R("pmceid0_el0");
		printk("      PMCEID0_EL0 0x%016" PRIx64 " — 구현 여부\n", ceid0);
		static const struct { uint8_t ev; const char *nm; } chk[] = {
			{EV_L1I_REFILL,"L1I_REFILL"}, {EV_L1D_REFILL,"L1D_REFILL"},
			{EV_INST_RETIRED,"INST_RETIRED"}, {EV_EXC_TAKEN,"EXC_TAKEN"},
			{EV_BR_MIS_PRED,"BR_MIS_PRED"}, {EV_L2D_REFILL,"L2D_REFILL"},
			{EV_STALL_FRONT,"STALL_FRONTEND"}, {EV_STALL_BACK,"STALL_BACKEND"},
		};
		printk("      ");
		for (unsigned i = 0; i < ARRAY_SIZE(chk); i++) {
			printk("%s=%s ", chk[i].nm,
			       (ceid0 >> chk[i].ev) & 1 ? "있음" : "**없음**");
		}
		printk("\n      ⚠ '없음' 인 이벤트의 0 은 '사건이 없다' 가 아니다\n");
	}

	/* [1] 코드·스택·데이터의 PTE */
	printk("  [1] PTE — 코드까지 본다 (지금까지 데이터만 봤다)\n");
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
	{
		uint64_t here;
		__asm__ volatile("adr %0, ." : "=r"(here));
		walk("코드(main)", here);
		walk("코드(alu1)", (uint64_t)(uintptr_t)&alu1);
		walk("스택",       (uint64_t)(uintptr_t)&here);
	}
#else
	printk("      (생략 — QEMU 매핑은 보드와 달라 뜻이 없다)\n");
#endif

	/* ── 이벤트 묶음 1 ─────────────────────────────────────────────── */
	static const uint8_t set1[NEV] = {
		EV_INST_RETIRED, EV_L1I_REFILL, EV_L1D_REFILL,
		EV_BR_MIS_PRED, EV_EXC_TAKEN, EV_STALL_FRONT };
	static const char *const nm1[NEV] = {
		"INS", "L1I_RF", "L1D_RF", "BRMIS", "EXC", "STALL_F" };
	pmu_set_events(set1, nm1);
	printk("  [2] 묶음1: INS · L1I_RF · L1D_RF · BRMIS · EXC · STALL_F\n");

	struct meas m;
	const uint64_t N1 = 500000;
	volatile uint64_t sink = 0;

	(void)alu1(1000);
	meas_begin(&m); sink += alu1(N1); meas_end(&m);
	meas_print("(a) ALU 1x", &m, N1, 10);

	(void)alu8(1000);
	meas_begin(&m); sink += alu8(N1); meas_end(&m);
	meas_print("(b) ALU 8x", &m, N1, 66);

	/* (a') 인터럽트를 막고 같은 루프 — 이것이 [A] 의 핵심이다 */
	{
		uint64_t hppir = 0xffff;
		__asm__ volatile("msr daifset, #0xf" ::: "memory");
		meas_begin(&m); sink += alu1(N1); meas_end(&m);
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
		/* ICC_HPPIR1_EL1 = S3_0_C12_C12_2 — 대기 중 INTID (1023=없음) */
		hppir = SYSREG_R("S3_0_C12_C12_2");
#endif
		__asm__ volatile("msr daifclr, #0xf" ::: "memory");
		meas_print("(a') ALU 1x IRQ막음", &m, N1, 10);
		printk("                       ICC_HPPIR1_EL1 = %" PRIu64
		       "%s\n", hppir,
		       (hppir == 1023) ? "  (대기 인터럽트 없음)" :
		       (hppir == 0xffff) ? "  (QEMU — 읽지 않았다)" :
		       "  ⚠⚠ **대기 중인 인터럽트가 있다**");
	}

	/* (c) 분기 없는 직선 코드 — 연달아 두 번 */
	meas_begin(&m); sink += alu_straight(); meas_end(&m);
	meas_print("(c1) 직선 냉/명령", &m, 1024, 1);
	meas_begin(&m); sink += alu_straight(); meas_end(&m);
	meas_print("(c2) 직선 온/명령", &m, 1024, 1);

	/* (d) 16 KiB 읽기 */
	{
		const size_t SZ = 16u << 10;
		volatile uint64_t *b = malloc(SZ);
		if (b) {
			uint64_t s = 0;
			for (size_t i = 0; i < SZ/8; i++) { b[i] = i; }
			(void)read_stream(b, SZ/8, &s);
			/* 바퀴 = 64바이트 묶음 하나 */
			const uint64_t iters = (SZ/8) / 8;
			meas_begin(&m);
			for (int r = 0; r < 64; r++) { (void)read_stream(b, SZ/8, &s); }
			meas_end(&m);
			meas_print("(d) 16KiB 읽기x64", &m, iters * 64, 10);
			printk("                       (바퀴 = 64바이트 · sum %016" PRIx64 ")\n", s);
			free((void *)b);
		}
	}

	/* ── 이벤트 묶음 2 ─────────────────────────────────────────────── */
	static const uint8_t set2[NEV] = {
		EV_INST_RETIRED, EV_L2D_REFILL, EV_STALL_BACK,
		EV_STALL_FRONT, EV_EXC_TAKEN, EV_L1I_REFILL };
	static const char *const nm2[NEV] = {
		"INS", "L2D_RF", "STALL_B", "STALL_F", "EXC", "L1I_RF" };
	pmu_set_events(set2, nm2);
	printk("  [3] 묶음2: INS · L2D_RF · STALL_B · STALL_F · EXC · L1I_RF\n");

	(void)alu1(1000);
	meas_begin(&m); sink += alu1(N1); meas_end(&m);
	meas_print("(a) ALU 1x", &m, N1, 10);
	meas_begin(&m); sink += alu_straight(); meas_end(&m);
	meas_print("(c) 직선/명령", &m, 1024, 1);

	printk("  [4] 읽는 법 — 판정표\n");
	printk("      · EXC 가 0 이 아니고 (a') 의 IPC 가 (a) 보다 크게 높다\n");
	printk("        → **인터럽트다.** HPPIR 가 출처를 알려 준다. Zephyr 쪽 일\n");
	printk("      · L1I_RF 가 바퀴당 1 근처 + STALL_F 가 높다\n");
	printk("        → **명령 인출이 캐시되지 않는다**\n");
	printk("      · BRMIS 가 바퀴당 1 근처이고 (b)·(c) 가 빠르다\n");
	printk("        → **분기 예측기다**\n");
	printk("      · (c) 는 '바퀴당' 이 **명령당 사이클**이다 (분기가 없다).\n");
	printk("        예측기 가설이면 0.5 근처, 인출 비캐시 가설이면 1.25 이상\n");
	printk("      · (c1) 과 (c2) 가 다르다 → **L1-I 가 유지되지 않는다**\n");
	printk("        ⚠ 단 (c1) 은 첫 접근 비용(번역·TLB)도 포함한다. QEMU 에서는\n");
	printk("          JIT 번역 탓에 (c1) 이 20배 크게 나온다 — 실기에서만 본다\n");
	printk("      · STALL_B 만 높고 나머지가 조용하다 → **실행 자체가 느리다**\n");
	printk("      · (b) 의 바퀴당 사이클이 (a) 와 비슷 → 비용은 **분기당**\n");
	printk("        6.6배 가까이 → 비용은 **명령당**\n");
	printk("=== L8i 측정 끝 ===  (sink %" PRIu64 ")\n", sink);
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
