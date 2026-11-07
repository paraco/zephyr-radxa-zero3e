/* L8m — **힙과 모델 창을 나란히** 재어 L1 이 안 먹는 이유를 가린다.
 *
 * ── 지금까지의 사실 ────────────────────────────────────────────────────
 *
 * UART 폭주를 고친 뒤 L8e(4코어)가 이렇게 나왔다:
 *     32 KiB 2.52~3.52 B/cyc · 32 MiB 3.44~3.45 B/cyc   (네 코어 모두)
 * 폭주 전(0.65)보다 5배 좋아졌지만 **L1 안(32 KiB)과 DRAM(32 MiB)이
 * 여전히 같다.** A55 의 L1 히트는 8~16 B/cyc 여야 한다.
 *
 * ⚠ 그런데 그 측정은 **모델 창(0x58000000)** 을 읽었다. L8h 가 PTE 를
 *   확인한 힙(sram0, AttrIndx=4 → MAIR 0xff · SH=3)이 **아니다.**
 *   두 영역의 속성이 다를 수 있다. 그리고 모델 가중치는 그 창에서
 *   **제자리로** 읽히므로 거기의 캐시 가능성은 추론 속도에 직접 걸린다.
 *
 * ── 이 칸이 답할 것 ────────────────────────────────────────────────────
 *
 *   [1] 두 영역의 PTE 를 **각각** 워크한다 (속성·공유도·MAIR 바이트)
 *   [2] **같은 루프**로 두 영역을 번갈아 잰다 — 크기 스윕
 *   [3] ALU IPC 도 같이 찍는다 (폭주가 정말 없는지 확인하는 양성 대조)
 *
 * 판정:
 *   · 두 영역의 MAIR 가 다르다            → 원인은 **매핑**이다
 *   · 둘 다 0xff 인데 둘 다 L1 이 안 먹는다 → 매핑이 아니다. 루프나 코어 쪽
 *   · 힙은 L1 이 먹고 블롭은 안 먹는다     → **블롭 창만** 고치면 된다
 *
 * ⚠ 블롭 창은 512 MiB 다. 전체를 읽지 않는다 — 스윕 크기만 읽는다.
 * ⚠ 구현 정의 레지스터는 실기에서만 읽는다 (QEMU 는 S3_0_C15_* 를 구현하지
 *   않아 미정의 명령으로 죽는다 — L8g 에서 겪었다).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdlib.h>
#include <inttypes.h>

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(DT_NODELABEL(model_blob)))

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
/* ⚠ **정정 3 (2026-11-07).** 이 루프의 포인터가 `const volatile` 이었다.
 *   volatile 은 로드를 지워지지 않게 하지만 동시에 **순서대로 직렬화**한다 —
 *   독립 누산기 8개를 둔 의미가 사라진다. 실기에서 4 KiB(L1 안)와 32 MiB
 *   (DRAM)가 **똑같이 3.5 B/cyc** 로 나온 이유다. 캐시가 꺼졌다면 32 MiB 가
 *   훨씬 느려야 하므로, 같다는 것은 **루프가 병목**이라는 뜻이었다.
 *
 *   이 계열에서 세 번째 측정 버그다:
 *     (1) 바이트 단위 로드        → L8g 에서 고침
 *     (2) 의존성 체인             → L8g 에서 고침
 *     (3) volatile 직렬화         → 여기
 *
 *   고침: 포인터를 **일반 const** 로 두고, 로드가 지워지지 않게 두 가지를
 *   쓴다 —
 *     · 합을 호출자에게 돌려주고 호출자가 찍는다 (죽은 코드가 아니다)
 *     · reps 사이에 **컴파일러 장벽** `__asm__ volatile("" ::: "memory")` 을
 *       둔다. 그러면 같은 데이터를 다시 읽는 것을 "곱셈으로 접는" 최적화를
 *       막으면서도 루프 **안**의 로드는 자유롭게 파이프라인된다.
 */
static uint64_t __attribute__((noinline))
read_stream(const uint64_t *p, size_t n_u64, uint64_t *out_sum)
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
write_stream(uint64_t *p, size_t n_u64)
{
	const uint64_t t0 = cyc();
	for (size_t i = 0; i + 8 <= n_u64; i += 8) {
		p[i+0]=i; p[i+1]=i; p[i+2]=i; p[i+3]=i;
		p[i+4]=i; p[i+5]=i; p[i+6]=i; p[i+7]=i;
	}
	return cyc() - t0;
}

static void sweep(uint64_t *buf, size_t bytes, const char *label, int wr)
{
	const size_t n = bytes / 8;
	uint64_t sum = 0;
	if (wr) { (void)write_stream(buf, n); } else { (void)read_stream(buf, n, &sum); }

	int reps = 1;
	while ((size_t)reps * bytes < (4u << 20)) { reps *= 2; }

	uint64_t cycles = 0;
	for (int r = 0; r < reps; r++) {
		/* ⚠ 장벽. 없으면 컴파일러가 "같은 데이터를 reps 번 더한 것" 을
		 *   곱셈으로 접어 로드를 한 번만 할 수 있다. 루프 **안**은
		 *   건드리지 않으므로 로드는 그대로 파이프라인된다. */
		__asm__ volatile("" ::: "memory");
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

	printk("=== L8n: 루프의 volatile 을 떼고 다시 — 캐시가 정말 먹나 ===\n");

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
	uint64_t *buf = malloc(SZ);
	if (!buf) {
		printk("  ⚠ 32 MiB 할당 실패 — 나머지를 못 한다\n");
		goto fin;
	}
	for (size_t i = 0; i < SZ / 8; i++) { buf[i] = i; }

	/* ── [1] 두 영역의 PTE ───────────────────────────────────────────── */
	printk("  [1] 두 영역의 PTE 를 **각각** 본다\n");
	printk("      힙(malloc)   0x%016" PRIx64 "\n", (uint64_t)(uintptr_t)buf);
	printk("      모델 창      0x%016" PRIx64 "  (크기 %u MiB)\n",
	       (uint64_t)BLOB_ADDR, (unsigned)(BLOB_SIZE >> 20));
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
	printk("      MAIR_EL1 0x%016" PRIx64 " · TCR_EL1 0x%016" PRIx64 "\n",
	       SYSREG_R("mair_el1"), SYSREG_R("tcr_el1"));
	walk((uint64_t)(uintptr_t)buf);
	printk("      ─ 모델 창 시작\n");
	walk((uint64_t)BLOB_ADDR);
	printk("      ─ 모델 창 +256 MiB (큰 창이 쪼개졌나)\n");
	walk((uint64_t)BLOB_ADDR + (256u << 20));
#else
	printk("      (생략 — QEMU 매핑은 보드와 달라 뜻이 없다)\n");
#endif

	/* ── [2] 같은 루프로 두 영역을 번갈아 잰다 ───────────────────────── */
	printk("  [2] **같은 루프**로 두 영역을 번갈아 — 사이클당 바이트\n");
	printk("      기준: A55 의 L1(32 KiB) 히트는 8~16 B/cyc 여야 한다\n");
		printk("      ⚠ L8m 은 **3.50 평평** 이었다 — volatile 직렬화 탓이다.\n"
		       "        여기서 4 KiB 가 32 MiB 보다 뚜렷이 빠르면 캐시는 정상이다\n");
	{
		static const struct { size_t sz; const char *nm; } steps[] = {
			{ 4u << 10,   "4 KiB" },
			{ 16u << 10,  "16 KiB" },
			{ 32u << 10,  "32 KiB" },
			{ 256u << 10, "256 KiB" },
			{ 32u << 20,  "32 MiB" },
		};
		for (unsigned k = 0; k < ARRAY_SIZE(steps); k++) {
			printk("      ── %s\n", steps[k].nm);
			sweep(buf, steps[k].sz, "  힙", 0);
			sweep((uint64_t *)BLOB_ADDR, steps[k].sz,
			      "  블롭", 0);
		}
		printk("      ── 쓰기 (블롭 창은 **쓰지 않는다** — 모델이 들어 있다)\n");
		sweep(buf, 16u << 10, "  힙", 1);
		sweep(buf, 32u << 20, "  힙", 1);
	}
	free(buf);

fin:
	printk("  [6] 읽는 법\n");
	printk("      · 두 영역의 MAIR 가 **다르다** → 원인은 **매핑**이다.\n");
	printk("        블롭 창의 선언(apps/llm_common/model_blob.overlay)을 고친다\n");
	printk("      · 둘 다 0xff 인데 **둘 다** L1 이 안 먹는다 → 매핑이 아니다.\n");
	printk("        루프나 코어 쪽이다. [3] 의 IPC 가 정상이면 루프를 의심한다\n");
	printk("      · 힙은 L1 이 먹고 블롭은 안 먹는다 → **블롭 창만** 고치면 된다\n");
	printk("      · [3] 의 IPC 가 1.5 미만이면 폭주가 남아 있다는 뜻이다 —\n");
	printk("        그러면 아래 대역 숫자 전부를 믿지 않는다\n");
	printk("=== L8n 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
