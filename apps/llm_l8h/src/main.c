/* L8h — "캐시가 켜져 있는데 먹지 않는다" 의 원인을 가린다.
 *
 * L8g 실기 결과: SCTLR_EL1.C=1 인데 16 KiB(L1 안)와 32 MiB(DRAM)가
 * 0.83 대 0.65 B/cyc 로 사실상 같다. 같은 코드가 QEMU 에서 10~12 B/cyc 를
 * 내므로 루프는 무죄다.
 *
 * 후보 셋을 **싼 순서로** 가린다. 순서가 중요하다 — 3번을 배제하지 않고
 * 1·2 를 파면 헛수고일 수 있다.
 *
 *   [3] CPU 가 전역적으로 느린가   → 메모리를 전혀 안 쓰는 ALU 루프의 IPC
 *   [2] 공유도(SH)가 문제인가       → PTE 의 SH 필드
 *   [1] PTE 속성이 WB 가 아닌가     → **malloc 이 준 정확한 VA** 로 워크
 *
 * ⚠ L8c 의 페이지 워크는 `AttrIndx`→MAIR 0xff(Normal WB) 라고 했지만,
 *   그 워크가 **이 버퍼의 VA** 를 본 것인지 확인되지 않았다. 다른 VA 를
 *   봤다면 그 결론은 이 질문에 아무 말도 하지 않는다. 그래서 여기서는
 *   측정에 쓰는 바로 그 주소를 걸어 본다.
 *
 * ⚠ 구현 정의 레지스터는 **실기에서만** 읽는다 — QEMU 의 cortex-a55 는
 *   S3_0_C15_* 를 구현하지 않아 미정의 명령으로 죽는다 (L8g 에서 겪었다).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdlib.h>
#include <inttypes.h>

#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })

/* ── PMU 사이클 카운터 ──────────────────────────────────────────────── */
static void pmu_enable(void)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	v |= (1U << 0) | (1U << 2) | (1U << 6);   /* E | C(reset) | LC(64bit) */
	__asm__ volatile("msr pmcr_el0, %0" ::"r"(v));
	__asm__ volatile("msr pmcntenset_el0, %0" ::"r"((uint64_t)1 << 31));
	__asm__ volatile("isb");
}
static inline uint64_t cyc(void) { return SYSREG_R("pmccntr_el0"); }

/* ── [3] 메모리를 전혀 쓰지 않는 ALU 루프 ───────────────────────────────
 *
 * 왜 어셈블리인가: C 로 쓰면 컴파일러가 접어 버리거나 레지스터를 스택에
 * 쏟아 **메모리 접근이 섞인다.** 그러면 재려는 것을 못 잰다.
 *
 * 한 바퀴에 독립된 add 8개 + sub 1개 + cbnz 1개 = 명령 10개.
 * 의존성 체인이 없으므로 A55(2-wide)라면 **사이클당 2 명령**에 가까워야
 * 한다. 즉 한 바퀴가 약 5 사이클이다.
 */
static uint64_t __attribute__((noinline)) alu_loop(uint64_t iters)
{
	uint64_t out;
	__asm__ volatile(
		"	mov	x9,  #1\n"
		"	mov	x10, #2\n"
		"	mov	x11, #3\n"
		"	mov	x12, #4\n"
		"	mov	x13, #5\n"
		"	mov	x14, #6\n"
		"	mov	x15, #7\n"
		"	mov	x16, #8\n"
		"1:	add	x9,  x9,  #1\n"
		"	add	x10, x10, #1\n"
		"	add	x11, x11, #1\n"
		"	add	x12, x12, #1\n"
		"	add	x13, x13, #1\n"
		"	add	x14, x14, #1\n"
		"	add	x15, x15, #1\n"
		"	add	x16, x16, #1\n"
		"	subs	%[n], %[n], #1\n"
		"	cbnz	%[n], 1b\n"
		"	add	x9,  x9,  x10\n"
		"	add	x11, x11, x12\n"
		"	add	x13, x13, x14\n"
		"	add	x15, x15, x16\n"
		"	add	x9,  x9,  x11\n"
		"	add	x13, x13, x15\n"
		"	add	%[o], x9,  x13\n"
		: [o] "=&r"(out), [n] "+&r"(iters)
		:
		: "x9","x10","x11","x12","x13","x14","x15","x16","cc");
	return out;
}

/* ── 읽기 대역 (L8g 와 같은 루프 — 독립 누산기 8개, 8단 언롤) ────────── */
static uint64_t __attribute__((noinline))
read_stream(const volatile uint64_t *p, size_t n_u64, uint64_t *out_sum)
{
	uint64_t a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,a6=0,a7=0;
	const uint64_t t0 = cyc();
	for (size_t i = 0; i + 8 <= n_u64; i += 8) {
		a0 += p[i+0]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3];
		a4 += p[i+4]; a5 += p[i+5]; a6 += p[i+6]; a7 += p[i+7];
	}
	const uint64_t t1 = cyc();
	*out_sum = a0+a1+a2+a3+a4+a5+a6+a7;
	return t1 - t0;
}

/* ── 쓰기 대역 ──────────────────────────────────────────────────────────
 * 읽기가 느린 것과 쓰기가 느린 것은 원인이 다르다. Device 매핑이면 둘 다
 * 느리고, write-streaming 억제 같은 문제면 쓰기만 다르게 나온다.
 */
static uint64_t __attribute__((noinline))
write_stream(volatile uint64_t *p, size_t n_u64)
{
	const uint64_t t0 = cyc();
	for (size_t i = 0; i + 8 <= n_u64; i += 8) {
		p[i+0]=i; p[i+1]=i; p[i+2]=i; p[i+3]=i;
		p[i+4]=i; p[i+5]=i; p[i+6]=i; p[i+7]=i;
	}
	return cyc() - t0;
}

static void sweep(volatile uint64_t *buf, size_t bytes, const char *label, int wr)
{
	const size_t n = bytes / 8;
	uint64_t sum = 0;
	if (wr) { (void)write_stream(buf, n); } else { (void)read_stream(buf, n, &sum); }

	int reps = 1;
	while ((size_t)reps * bytes < (4u << 20)) { reps *= 2; }

	uint64_t cycles = 0;
	for (int r = 0; r < reps; r++) {
		cycles += wr ? write_stream(buf, n) : read_stream(buf, n, &sum);
	}
	const uint64_t total = (uint64_t)reps * bytes;
	const uint64_t bpc100 = cycles ? (total * 100) / cycles : 0;
	printk("      %-9s %s %4" PRIu64 ".%02" PRIu64 " B/cyc  (%d회, %" PRIu64 " 사이클)\n",
	       label, wr ? "쓰기" : "읽기", bpc100/100, bpc100%100, reps, cycles);
}

#ifdef CONFIG_BOARD_RADXA_ZERO_3E
/* ── 페이지 테이블 워크 ─────────────────────────────────────────────────
 *
 * CONFIG_ARM64_VA_BITS=32, 4 KiB 그래뉼:
 *   L1 = VA[31:30] (4 엔트리) · L2 = VA[29:21] (2 MiB 블록) · L3 = VA[20:12]
 * 디스크립터 [1:0]: 0b11 = table(L1·L2) 또는 page(L3) · 0b01 = block
 */
static void walk(uint64_t va)
{
	const uint64_t ttbr0 = SYSREG_R("ttbr0_el1") & ~0xfffULL;
	const uint64_t mair  = SYSREG_R("mair_el1");
	uint64_t tbl = ttbr0;
	uint64_t desc = 0;
	int level = 1;

	printk("        VA 0x%016" PRIx64 "  TTBR0 0x%016" PRIx64 "\n", va, ttbr0);

	/* L1 */
	desc = ((volatile uint64_t *)tbl)[(va >> 30) & 0x3];
	printk("        L1[%u] = 0x%016" PRIx64 "\n", (unsigned)((va >> 30) & 0x3), desc);
	if ((desc & 3) != 3) { goto done; }
	tbl = desc & 0x0000fffffffff000ULL;

	/* L2 */
	level = 2;
	desc = ((volatile uint64_t *)tbl)[(va >> 21) & 0x1ff];
	printk("        L2[%u] = 0x%016" PRIx64 "%s\n",
	       (unsigned)((va >> 21) & 0x1ff), desc,
	       ((desc & 3) == 1) ? "   (2 MiB 블록 — 여기서 끝)" : "");
	if ((desc & 3) == 1) { goto done; }
	if ((desc & 3) != 3) { goto done; }
	tbl = desc & 0x0000fffffffff000ULL;

	/* L3 */
	level = 3;
	desc = ((volatile uint64_t *)tbl)[(va >> 12) & 0x1ff];
	printk("        L3[%u] = 0x%016" PRIx64 "\n", (unsigned)((va >> 12) & 0x1ff), desc);

done:
	if ((desc & 1) == 0) {
		printk("        ⚠⚠ L%d 디스크립터가 **무효**다 (bit0=0)\n", level);
		return;
	}
	{
		const unsigned attr = (unsigned)((desc >> 2) & 7);   /* AttrIndx */
		const unsigned sh   = (unsigned)((desc >> 8) & 3);   /* SH */
		const unsigned ap   = (unsigned)((desc >> 6) & 3);   /* AP */
		const unsigned af   = (unsigned)((desc >> 10) & 1);  /* AF */
		const unsigned mb   = (unsigned)((mair >> (8 * attr)) & 0xff);
		static const char *shname[4] = {
			"Non-shareable", "예약(0b01)", "Outer Shareable", "Inner Shareable"
		};
		printk("        AttrIndx=%u → MAIR 바이트 **0x%02x**  SH=%u(%s)  AP=%u  AF=%u\n",
		       attr, mb, sh, shname[sh], ap, af);
		if (mb == 0xff) {
			printk("        → Normal · Inner/Outer 모두 Write-Back R/W-allocate"
			       "  **캐시 가능이 맞다**\n");
		} else if (mb == 0x44) {
			printk("        ⚠⚠ → Normal **Non-Cacheable**. 캐시가 먹지 않는 이유다\n");
		} else if ((mb & 0xf0) == 0x00) {
			printk("        ⚠⚠ → **Device** 메모리(0x%02x). 캐시가 먹지 않는 이유다\n", mb);
		} else {
			printk("        ⚠ → MAIR 0x%02x — Normal 이지만 WB R/W-alloc 이 아니다\n", mb);
		}
		if (sh == 3 && mb == 0xff) {
			printk("        ⚠ 캐시 가능 + Inner Shareable 이다. 그런데도 캐시가\n"
			       "          먹지 않으면 코어가 코히런시 도메인에 참여하지 않는\n"
			       "          쪽을 본다 (DSU/ACE). EL1 에서는 확인할 수 없다\n");
		}
	}
}
#endif

int main(void)
{
	const uint64_t cntfrq = SYSREG_R("cntfrq_el0");
	pmu_enable();

	printk("=== L8h: 캐시가 켜졌는데 왜 안 먹는가 ===\n");

	/* 진짜 CPU 주파수 */
	uint64_t hz = 0;
	{
		const uint64_t v0 = SYSREG_R("cntvct_el0"), c0 = cyc();
		const uint64_t want = cntfrq / 50;              /* 20 ms */
		while (SYSREG_R("cntvct_el0") - v0 < want) { }
		const uint64_t c1 = cyc(), v1 = SYSREG_R("cntvct_el0");
		const uint64_t dv = v1 - v0, dc = c1 - c0;
		hz = dv ? (dc * cntfrq) / dv : 0;
		printk("  [0] CPU %" PRIu64 " MHz (PMU/타이머 실측) · CNTFRQ %" PRIu64 " Hz\n",
		       hz / 1000000, cntfrq);
	}

	/* ── [3] 먼저: CPU 가 전역적으로 느린가 ──────────────────────────── */
	printk("  [3] **메모리를 전혀 쓰지 않는 ALU 루프** — 이것이 갈림길이다\n");
	{
		const uint64_t N = 2000000;
		(void)alu_loop(1000);                     /* 예열 (I캐시) */
		const uint64_t t0 = cyc();
		const uint64_t r = alu_loop(N);
		const uint64_t dc = cyc() - t0;
		/* 한 바퀴 = 명령 10개 */
		const uint64_t ipc100 = dc ? (N * 10 * 100) / dc : 0;
		const uint64_t cpi100 = N ? (dc * 100) / N : 0;
		printk("      %" PRIu64 " 바퀴 · %" PRIu64 " 사이클 · 한 바퀴 %"
		       PRIu64 ".%02" PRIu64 " 사이클\n", N, dc, cpi100/100, cpi100%100);
		printk("      **IPC = %" PRIu64 ".%02" PRIu64 "**  (결과 %" PRIu64 ")\n",
		       ipc100/100, ipc100%100, r);
		printk("      기준: A55 는 2-wide 다. 의존성 없는 add 8개면 IPC 1.5~2,\n"
		       "            한 바퀴 5~7 사이클이어야 한다\n");
		printk("      ⚠ QEMU 의 값은 **기준이 아니다** — 파이프라인을 모델링하지\n"
		       "        않아 IPC 가 4 를 넘는다. 아래 판정은 실기에만 적용한다\n");
		if (ipc100 < 50) {
			printk("      ⚠⚠ IPC < 0.5 — **CPU 자체가 느리다.** 메모리는 무죄다.\n"
			       "         원인을 명령 인출·I캐시·전역 설정에서 찾아야 한다\n");
		} else if (ipc100 >= 100) {
			printk("      → IPC 가 정상이다. **CPU 는 무죄.** 문제는 메모리\n"
			       "         하위 시스템이다 — 아래 [1][2] 로 간다\n");
		} else {
			printk("      ⚠ IPC 0.5~1.0 — 애매하다. 정상도 아니고 치명적이지도\n"
			       "        않다. 메모리 결과와 함께 봐야 한다\n");
		}
	}

	/* ── 버퍼를 잡는다. 이 **정확한 주소**를 워크한다 ─────────────────── */
	const size_t SZ = 32u << 20;
	volatile uint64_t *buf = malloc(SZ);
	if (!buf) {
		printk("  ⚠ 32 MiB 할당 실패 — 나머지를 못 한다\n");
		goto fin;
	}
	for (size_t i = 0; i < SZ / 8; i++) { buf[i] = i; }

	/* ── [1][2] 그 주소의 PTE ────────────────────────────────────────── */
	printk("  [1][2] 측정에 쓰는 **바로 그 주소**의 PTE\n");
	printk("      malloc 이 준 주소: 0x%016" PRIx64 "  (크기 32 MiB)\n",
	       (uint64_t)(uintptr_t)buf);
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
	printk("      MAIR_EL1 0x%016" PRIx64 " · TCR_EL1 0x%016" PRIx64 "\n",
	       SYSREG_R("mair_el1"), SYSREG_R("tcr_el1"));
	walk((uint64_t)(uintptr_t)buf);
	printk("      ─ 비교: 측정 버퍼의 **끝쪽** 주소도 본다 (매핑이 섞였나)\n");
	walk((uint64_t)(uintptr_t)buf + (31u << 20));
#else
	printk("      (생략 — QEMU 는 S3_0_C15_* 를 구현하지 않고, 워크 자체는\n"
	       "       아키텍처 레지스터만 쓰지만 보드 매핑과 달라 뜻이 없다)\n");
#endif

	/* ── 대역 스윕 (읽기 + 쓰기) ─────────────────────────────────────── */
	printk("  [5] 읽기·쓰기 대역 스윕\n");
	sweep(buf, 4u << 10,   "4 KiB",   0);
	sweep(buf, 16u << 10,  "16 KiB",  0);
	sweep(buf, 32u << 10,  "32 KiB",  0);
	sweep(buf, 256u << 10, "256 KiB", 0);
	sweep(buf, 32u << 20,  "32 MiB",  0);
	sweep(buf, 16u << 10,  "16 KiB",  1);
	sweep(buf, 32u << 20,  "32 MiB",  1);
	free((void *)buf);

fin:
	printk("  [6] 읽는 법 — 순서대로\n");
	printk("      · [3] IPC 가 1 이상이면 CPU 는 무죄. [1] 의 MAIR 바이트를 본다\n");
	printk("      · MAIR 가 0x44(Normal NC) 또는 0x0x(Device)면 **원인 확정**이다\n");
	printk("      · MAIR 가 0xff(WB) 인데도 4 KiB 가 1 B/cyc 이면 → 속성은\n");
	printk("        맞다. 코어가 코히런시 도메인에 없는 쪽(DSU/ACE)이다.\n");
	printk("        EL1 에서는 못 고친다 — 부트 펌웨어 쪽 일이 된다\n");
	printk("      · [3] IPC 가 0.5 미만이면 메모리 이야기는 전부 접고\n");
	printk("        CPU 쪽(명령 인출·전역 설정)으로 간다\n");
	printk("=== L8h 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
