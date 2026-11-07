/*
 * L8d — `CPUECTLR_EL1.SMPEN` 을 읽고, 0 이면 켜서 **전후를 잰다**.
 *
 * ── 어디까지 왔나 ───────────────────────────────────────────────────────
 *
 * L8c 가 페이지 표를 직접 걸어 셋 다 **Normal WB RW-alloc · SH=3** 임을
 * 확인했다. 즉 메모리는 캐시 가능이고, 모델 창과 sram0 이 똑같고,
 * `volatile` 도 범인이 아니었다(없는 쪽이 오히려 느렸다).
 *
 * 그런데 L1(32 KiB) 이 32 MiB 와 1.17배 차이뿐이다 — 8바이트 적재당
 * **7.8 사이클**(815 MHz 기준)이고 L1 이면 1~2 사이클이어야 한다.
 *
 * ── 남은 설명 ───────────────────────────────────────────────────────────
 *
 * 페이지가 `SH=3`(내부 공유)다. Cortex-A55 에서 `CPUECTLR_EL1.SMPEN`(bit 6)
 * 이 0 이면 **내부 공유 캐시 가능 메모리가 사실상 비캐시로 동작한다.**
 * ARM 문서가 "캐시·MMU 를 켜기 전에 SMPEN 을 설정해야 하며 그러지 않으면
 * 캐시가 쓰이지 않을 수 있다" 고 적은 그 비트다. 증상이 정확히 맞는다.
 *
 * TF-A 가 보통 설정하지만 **우리는 확인한 바가 없다.** 읽어 본다.
 *
 * ⚠ **쓰기는 조심스럽다.** `CPUECTLR_EL1` 은 구현 정의 레지스터(S3_0_C15_C1_0)
 *   이고 EL3 가 `ACTLR_EL3` 로 접근을 막아 두면 EL1 쓰기가 **예외**로 간다.
 *   Zephyr 가 치명 오류를 찍으므로 조용히 틀리지는 않지만, 그 경우 보드가
 *   멈춘다 — `apply try` 슬롯이라 전원 재투입으로 복구된다.
 *
 * ⚠ 이미 1 이면 **쓰지 않는다.** 그러면 이 가설도 죽고, 원인은 더 깊은 데
 *   있다(L2 클럭·인터커넥트).
 *
 * ⚠ 측정만 한다. 판정하지 않는다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <inttypes.h>

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))

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

/* Cortex-A55: CPUECTLR_EL1 = S3_0_C15_C1_0 */
static inline uint64_t rd_cpuectlr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, S3_0_C15_C1_0" : "=r"(v));
	return v;
}
static inline void wr_cpuectlr(uint64_t v)
{
	__asm__ volatile("msr S3_0_C15_C1_0, %0" ::"r"(v));
	__asm__ volatile("isb; dsb sy");
}

/* 캐시 계층을 CLIDR/CCSIDR 로 확인한다 — "L1 이 있다" 를 하드웨어에 묻는다 */
static void dump_caches(void)
{
	const uint64_t clidr = SYSREG_R("clidr_el1");
	printk("      CLIDR_EL1 0x%016" PRIx64 "  (LoUIS=%u LoC=%u)\n", clidr,
	       (unsigned)((clidr >> 21) & 7), (unsigned)((clidr >> 24) & 7));
	for (int lvl = 0; lvl < 3; lvl++) {
		const uint32_t ctype = (uint32_t)((clidr >> (lvl * 3)) & 7);
		if (ctype == 0) {
			continue;
		}
		static const char *k[] = { "없음", "I만", "D만", "I+D(분리)",
					   "통합", "?", "?", "?" };
		printk("        L%d: %s", lvl + 1, k[ctype]);
		/* D 캐시(또는 통합)의 크기 — CSSELR 로 고르고 CCSIDR 을 읽는다 */
		if (ctype == 2 || ctype == 3 || ctype == 4) {
			__asm__ volatile("msr csselr_el1, %0; isb"
					 ::"r"((uint64_t)(lvl << 1)));
			const uint64_t cc = SYSREG_R("ccsidr_el1");
			const uint32_t line = 4u << ((cc & 7) + 2);
			const uint32_t assoc = (uint32_t)(((cc >> 3) & 0x3ff) + 1);
			const uint32_t sets = (uint32_t)(((cc >> 13) & 0x7fff) + 1);
			printk("  D: %u B 줄 × %u way × %u set = %u KiB",
			       line, assoc, sets, line * assoc * sets / 1024);
		}
		printk("\n");
	}
}

static uint64_t sweep(uintptr_t base, size_t sz, uint64_t frq)
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

static const size_t SIZES[] = { 32u * 1024, 256u * 1024, 32u * 1024 * 1024 };
static const char *NAMES[] = { "32 KiB (L1)", "256 KiB (L2)", "32 MiB (DRAM)" };

static void sweep_all(const char *tag, uint64_t frq)
{
	printk("      %-8s", tag);
	for (int k = 0; k < 3; k++) {
		printk(" %-14s %5" PRIu64 " MiB/s", NAMES[k],
		       sweep(BLOB_ADDR, SIZES[k], frq));
	}
	printk("\n");
}

int main(void)
{
	const uint64_t frq = SYSREG_R("cntfrq_el0");
	printk("=== L8d: SMPEN 을 읽고, 0 이면 켜서 전후를 잰다 ===\n");

	printk("  [1] 캐시 계층 (하드웨어에 묻는다)\n");
	dump_caches();

	printk("  [2] CPUECTLR_EL1 (S3_0_C15_C1_0)\n");
	const uint64_t before = rd_cpuectlr();
	printk("      값 0x%016" PRIx64 "\n", before);
	printk("      SMPEN(bit 6) = **%u**\n", (unsigned)((before >> 6) & 1));

	printk("  [3] 지금 상태의 읽기 대역\n");
	sweep_all("현재", frq);

	if ((before >> 6) & 1) {
		printk("  [4] SMPEN 이 **이미 1** 이다 — 이 가설은 죽었다.\n");
		printk("      캐시가 안 먹는 이유는 더 깊은 데 있다:\n");
		printk("      · L2/인터커넥트 클럭이 낮다 (U-Boot 가 남긴 값)\n");
		printk("      · 또는 내 대역 측정 자체가 L1 을 안 쓰고 있다\n");
		printk("        (8바이트 × 8 = 64 B 는 줄 하나다. 줄마다 1회 적재면\n");
		printk("         줄 지연이 그대로 보인다 — 선행적재가 필요하다)\n");
	} else {
		printk("  [4] SMPEN 이 0 이다 — **켜고 다시 잰다**\n");
		printk("      ⚠ 쓰기가 EL3 에서 막히면 예외가 난다. 그러면 전원\n");
		printk("        재투입으로 복구된다 (apply try 슬롯).\n");
		wr_cpuectlr(before | (1ULL << 6));
		const uint64_t after = rd_cpuectlr();
		printk("      쓴 뒤 0x%016" PRIx64 "  SMPEN=%u %s\n", after,
		       (unsigned)((after >> 6) & 1),
		       ((after >> 6) & 1) ? "✓ 먹었다" : "✗ 안 먹었다(무시됨)");
		printk("  [5] SMPEN 켠 뒤의 읽기 대역\n");
		sweep_all("켠 뒤", frq);
		printk("      → [3]보다 **몇 배** 빠르면 SMPEN 이 원인이었다.\n");
		printk("        그러면 Zephyr 부팅 초기에 설정해야 한다(상류 보고감).\n");
	}

	printk("=== L8d 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
