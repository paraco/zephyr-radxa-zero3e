/*
 * L8c — 캐시 가설을 **구분되는 방법으로** 가린다.
 *
 * ── 왜 ──────────────────────────────────────────────────────────────────
 *
 * L8b 에서 모델 창의 크기별 읽기가 거의 평평했다:
 *     32 KiB 827 · 256 KiB 729 · 4 MiB 708 · 32 MiB 706 MiB/s
 * 815 MHz A55 의 L1 이라면 수 GiB/s 여야 하므로 **캐시가 안 먹는** 꼴이다.
 *
 * 그런데 L8a 에서 나는 "모델 창 695 = sram0 695, 같으니 속성 문제 아니다"
 * 로 결론 냈었다. **32 MiB 한 점으로는 캐시 여부를 구분할 수 없다** —
 * L2(512 KiB)를 넘는 스트리밍은 캐시와 무관하게 DRAM 대역에 묶인다.
 * 그 측정은 가설을 지우지 못했다.
 *
 * 이 칸은 **구분되는 세 가지**를 한다:
 *
 *   [2] **MMU 서술자를 직접 읽는다.** 타이밍 해석이 필요 없다 —
 *       AttrIndx 와 MAIR_EL1 이 Normal-WB 인지 Device 인지 **글자로** 말해 준다.
 *       이것이 결정적이다.
 *   [3] 크기 쓸기를 **sram0 과 모델 창에 나란히** 한다. 한쪽만 평평하면
 *       그 영역의 속성이 범인이다.
 *   [4] 같은 쓸기를 **volatile 없이** 한다.
 *       ⚠ L8b 의 느린 L1 이 **측정 기법의 탓**일 수 있다 — volatile 은
 *         컴파일러가 적재를 묶거나 펼치지 못하게 한다. 8바이트당 7.9사이클은
 *         "메모리가 느리다" 로도, "루프가 느리다" 로도 설명된다.
 *         volatile 없는 쪽이 훨씬 빠르면 **내 측정이 문제였다.**
 *
 * ⚠ 측정만 한다. 판정하지 않는다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define SRAM_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(sram0)))

#define SYSREG_R(name)                                                         \
	({                                                                     \
		uint64_t _v;                                                   \
		__asm__ volatile("mrs %0, " name : "=r"(_v));                  \
		_v;                                                            \
	})

static inline uint64_t cnt(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

/* MAIR 한 바이트를 사람 말로 */
static const char *mair_name(uint8_t a)
{
	if (a == 0x00) return "Device-nGnRnE";
	if (a == 0x04) return "Device-nGnRE";
	if (a == 0x08) return "Device-nGRE";
	if (a == 0x0c) return "Device-GRE";
	if (a == 0x44) return "Normal **NonCacheable**";
	if (a == 0xff) return "Normal WB RW-alloc (캐시됨)";
	if ((a & 0xf0) && (a & 0x0f)) return "Normal (조합)";
	return "모름";
}

/*
 * 페이지 표를 걷는다.
 *
 * ⚠ 이 보드는 CONFIG_ARM64_VA_BITS=32 · 4 KiB 그래뉼이다. 그래서 L1 이
 *   시작 단계이고 VA[31:30] 로 4개 항목을 고른다. L2 는 VA[29:21](2 MiB
 *   블록), L3 는 VA[20:12](4 KiB 페이지)다.
 *   VA_BITS 가 바뀌면 이 함수도 바뀌어야 한다 — BUILD_ASSERT 로 막는다.
 */
BUILD_ASSERT(CONFIG_ARM64_VA_BITS == 32, "이 PTE 걷기는 VA 32비트 전용이다");

static void walk(const char *what, uint64_t va, uint64_t mair)
{
	const uint64_t ttbr0 = SYSREG_R("ttbr0_el1") & ~0xfffULL;
	printk("      %s VA 0x%08" PRIx64 "  (TTBR0 0x%08" PRIx64 ")\n",
	       what, va, ttbr0);

	const uint64_t *l1 = (const uint64_t *)(uintptr_t)ttbr0;
	const uint32_t i1 = (uint32_t)((va >> 30) & 0x3);
	const uint64_t d1 = l1[i1];
	printk("        L1[%u] = 0x%016" PRIx64 "  ", i1, d1);
	if ((d1 & 3) == 0) {
		printk("(무효)\n");
		return;
	}
	if ((d1 & 3) == 1) {
		printk("1 GiB 블록\n");
		goto decode;
	}
	printk("표\n");

	{
		const uint64_t *l2 = (const uint64_t *)(uintptr_t)(d1 & 0xfffffffff000ULL);
		const uint32_t i2 = (uint32_t)((va >> 21) & 0x1ff);
		const uint64_t d2 = l2[i2];
		printk("        L2[%u] = 0x%016" PRIx64 "  ", i2, d2);
		if ((d2 & 3) == 0) {
			printk("(무효)\n");
			return;
		}
		if ((d2 & 3) == 1) {
			printk("2 MiB 블록\n");
			uint64_t d = d2;
			const uint32_t ai = (uint32_t)((d >> 2) & 7);
			const uint8_t ab = (uint8_t)((mair >> (ai * 8)) & 0xff);
			printk("        → AttrIndx=%u → MAIR 바이트 0x%02x = **%s**\n",
			       ai, ab, mair_name(ab));
			printk("          SH=%u (0=비공유 2=외부공유 3=내부공유) · AF=%u · AP=%u\n",
			       (unsigned)((d >> 8) & 3), (unsigned)((d >> 10) & 1),
			       (unsigned)((d >> 6) & 3));
			return;
		}
		printk("표\n");
		const uint64_t *l3 = (const uint64_t *)(uintptr_t)(d2 & 0xfffffffff000ULL);
		const uint32_t i3 = (uint32_t)((va >> 12) & 0x1ff);
		const uint64_t d3 = l3[i3];
		printk("        L3[%u] = 0x%016" PRIx64 "  4 KiB 페이지\n", i3, d3);
		const uint32_t ai = (uint32_t)((d3 >> 2) & 7);
		const uint8_t ab = (uint8_t)((mair >> (ai * 8)) & 0xff);
		printk("        → AttrIndx=%u → MAIR 바이트 0x%02x = **%s**\n",
		       ai, ab, mair_name(ab));
		printk("          SH=%u · AF=%u · AP=%u\n",
		       (unsigned)((d3 >> 8) & 3), (unsigned)((d3 >> 10) & 1),
		       (unsigned)((d3 >> 6) & 3));
		return;
	}
decode:
	{
		const uint32_t ai = (uint32_t)((d1 >> 2) & 7);
		const uint8_t ab = (uint8_t)((mair >> (ai * 8)) & 0xff);
		printk("        → AttrIndx=%u → MAIR 바이트 0x%02x = **%s**\n",
		       ai, ab, mair_name(ab));
	}
}

/* volatile 쓸기 — L8b 와 같은 기법 */
static uint64_t sweep_vol(uintptr_t base, size_t sz, uint64_t frq)
{
	const volatile uint64_t *p = (const volatile uint64_t *)base;
	const size_t n = sz / 8;
	uint64_t acc = 0;
	for (size_t i = 0; i < n; i++) {
		acc += p[i];
	}
	const int reps = (int)(64u * 1024u * 1024u / sz);
	const uint64_t t0 = cnt();
	for (int r = 0; r < reps; r++) {
		for (size_t i = 0; i + 8 <= n; i += 8) {
			acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3]
			     + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
		}
	}
	const uint64_t tk = cnt() - t0;
	if (acc == 0x5a5a5a5aULL) {
		printk("(안 일어난다)");
	}
	return tk ? (uint64_t)sz * (uint64_t)reps * frq / tk / 1048576 : 0;
}

/* volatile **없이** — 컴파일러가 묶고 펼칠 수 있다 */
static uint64_t sweep_novol(uintptr_t base, size_t sz, uint64_t frq)
{
	const uint64_t *p = (const uint64_t *)base;
	const size_t n = sz / 8;
	uint64_t acc = 0;
	for (size_t i = 0; i < n; i++) {
		acc += p[i];
	}
	const int reps = (int)(64u * 1024u * 1024u / sz);
	const uint64_t t0 = cnt();
	for (int r = 0; r < reps; r++) {
		for (size_t i = 0; i + 8 <= n; i += 8) {
			acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3]
			     + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
		}
	}
	const uint64_t tk = cnt() - t0;
	/* acc 를 반드시 써야 루프가 안 지워진다 */
	__asm__ volatile("" ::"r"(acc));
	return tk ? (uint64_t)sz * (uint64_t)reps * frq / tk / 1048576 : 0;
}

static const size_t SIZES[] = { 32u * 1024, 256u * 1024, 4u * 1024 * 1024,
				32u * 1024 * 1024 };
static const char *NAMES[] = { "32 KiB (L1)", "256 KiB (L2)", "4 MiB", "32 MiB" };

int main(void)
{
	const uint64_t frq = SYSREG_R("cntfrq_el0");
	const uint64_t mair = SYSREG_R("mair_el1");

	printk("=== L8c: 캐시 가설을 구분되는 방법으로 가린다 ===\n");

	printk("  [1] MMU 설정\n");
	printk("      MAIR_EL1 0x%016" PRIx64 "\n", mair);
	for (int i = 0; i < 8; i++) {
		const uint8_t a = (uint8_t)((mair >> (i * 8)) & 0xff);
		printk("        Attr%d = 0x%02x  %s\n", i, a, mair_name(a));
	}
	printk("      TCR_EL1  0x%016" PRIx64 "\n", SYSREG_R("tcr_el1"));
	printk("      VA_BITS  %d (CONFIG_ARM64_VA_BITS)\n", CONFIG_ARM64_VA_BITS);

	printk("  [2] 페이지 표를 직접 걷는다 (타이밍 해석이 필요 없다)\n");
	walk("sram0 ", (uint64_t)SRAM_ADDR, mair);
	walk("모델창", (uint64_t)BLOB_ADDR, mair);

	/* sram0 안의 **힙 버퍼**도 본다 — 아레나가 쓰는 실제 주소다 */
	void *buf = malloc(32u * 1024 * 1024);
	if (buf == NULL) {
		printk("      힙 32 MiB 할당 실패 — sram0 쪽 측정을 건너뛴다\n");
	} else {
		uint64_t *q = (uint64_t *)buf;
		for (size_t i = 0; i < 32u * 1024 * 1024 / 8; i++) {
			q[i] = i;
		}
		walk("힙버퍼", (uint64_t)(uintptr_t)buf, mair);
	}

	printk("  [3] 크기 쓸기 — volatile (L8b 와 같은 기법)\n");
	printk("      %-14s %12s %12s\n", "크기", "모델창", "힙(sram0)");
	for (int k = 0; k < 4; k++) {
		const uint64_t a = sweep_vol(BLOB_ADDR, SIZES[k], frq);
		const uint64_t b = buf ? sweep_vol((uintptr_t)buf, SIZES[k], frq) : 0;
		printk("      %-14s %9" PRIu64 " MiB/s %9" PRIu64 " MiB/s\n",
		       NAMES[k], a, b);
	}

	printk("  [4] 같은 쓸기 — **volatile 없이** (컴파일러가 묶을 수 있다)\n");
	printk("      %-14s %12s %12s\n", "크기", "모델창", "힙(sram0)");
	for (int k = 0; k < 4; k++) {
		const uint64_t a = sweep_novol(BLOB_ADDR, SIZES[k], frq);
		const uint64_t b = buf ? sweep_novol((uintptr_t)buf, SIZES[k], frq) : 0;
		printk("      %-14s %9" PRIu64 " MiB/s %9" PRIu64 " MiB/s\n",
		       NAMES[k], a, b);
	}

	printk("  [5] 읽는 법\n");
	printk("      · [2]가 **Normal WB** 라고 하면 속성은 정상이다 — 타이밍으로\n");
	printk("        반박하지 않는다. 서술자가 사실이다.\n");
	printk("      · [4]가 [3]보다 훨씬 빠르면 **L8b 의 느린 L1 은 내 측정 탓**이다\n");
	printk("        (volatile 이 적재를 묶지 못하게 했다).\n");
	printk("      · [4]에서도 L1 이 32 MiB 와 비슷하면 캐시가 정말 안 먹는 것이다.\n");
	printk("=== L8c 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
