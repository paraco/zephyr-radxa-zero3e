/* L8g — L8 측정의 **정정판**.
 *
 * ⚠ 왜 다시 만드는가 (2026-11-07 밤). L8a~L8d 가 세 가지를 잘못했다.
 *
 * (1) **레지스터를 잘못 읽었다.** `S3_0_C15_C1_0` 을 `CPUECTLR_EL1` 이라고
 *     불렀는데, Cortex-A55 에서 그것은 **`CPUACTLR_EL1`** 이다. ARM 의
 *     TF-A 헤더가 정본이다 (2026-11-07 원문 확인,
 *     include/lib/cpus/aarch64/cortex_a55.h):
 *         CORTEX_A55_CPUECTLR_EL1   S3_0_C15_C1_4
 *         CORTEX_A55_CPUACTLR_EL1   S3_0_C15_C1_0
 *     그리고 **A55 의 CPUECTLR_EL1 에는 SMPEN 비트가 없다.** ARM 이 정의한
 *     필드는 L1WSCTL(비트 26:25) 하나다. A55 는 DynamIQ(DSU) 기반이라
 *     코히런시를 DSU 가 관리한다. SMPEN 은 A53/A57/A72 의 필드다.
 *     → "SMPEN=0 이라 캐시가 안 먹는다" 는 결론은 **근거가 없었다.**
 *
 * (2) **대역폭 루프가 자기 자신을 재고 있었다.** 815 MHz 에서 L1 읽기
 *     830 MiB/s = 사이클당 약 1 바이트다. A55 의 L1 히트로는 터무니없다
 *     (사이클당 16 바이트 급이어야 한다). 바이트 단위 로드에 의존성
 *     체인이면 캐시가 아니라 루프 지연을 잰다.
 *     → 여기서는 **64비트 로드 · 독립 누산기 8개 · 8단 언롤**로 잰다.
 *        그리고 MiB/s 가 아니라 **사이클당 바이트**로 보고한다 — 클럭이
 *        낮은 것과 캐시가 안 먹는 것을 섞지 않으려면 이 단위여야 한다.
 *
 * (3) **QEMU 에서 구현 정의 레지스터를 읽어 죽었다.** QEMU 의 cortex-a55 는
 *     S3_0_C15_* 를 구현하지 않아 미정의 명령으로 떨어진다(EC 0x0).
 *     → 구현 정의 레지스터는 **실기에서만** 읽는다. 아키텍처 레지스터
 *        (SCTLR·CTR·CLIDR·CCSIDR·MIDR)는 양쪽에서 읽는다.
 *
 * 이 칸이 답해야 할 질문은 하나다:
 *   **D캐시가 실제로 켜져 있고 먹고 있는가?**
 * SCTLR_EL1.C 와 PTE 속성으로 "켜졌는가" 를, 크기 스윕의 사이클당 바이트로
 * "먹는가" 를 따로 본다. 둘은 다른 질문이다.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdlib.h>
#include <string.h>
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

/* ── 제대로 된 읽기 대역 측정 ────────────────────────────────────────
 *
 * 독립 누산기 8개로 의존성 체인을 끊는다. 한 바퀴에 64바이트(캐시 줄 하나)를
 * 읽는다. 컴파일러가 지우지 못하게 결과를 돌려준다.
 */
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

static void sweep(volatile uint64_t *buf, size_t bytes, const char *label,
		  uint64_t hz)
{
	const size_t n = bytes / 8;
	/* 예열 — 첫 바퀴는 캐시를 채운다. 그걸 재면 전부 미스가 섞인다. */
	uint64_t sum = 0;
	(void)read_stream(buf, n, &sum);

	/* 작은 버퍼는 여러 번 돌려 측정 분해능을 확보한다 */
	int reps = 1;
	while ((size_t)reps * bytes < (4u << 20)) { reps *= 2; }

	uint64_t cycles = 0;
	for (int r = 0; r < reps; r++) { cycles += read_stream(buf, n, &sum); }

	const uint64_t total = (uint64_t)reps * bytes;
	/* 사이클당 바이트 (×100 으로 소수 둘째자리까지) */
	const uint64_t bpc100 = cycles ? (total * 100) / cycles : 0;
	const uint64_t mibps  = cycles ? (total * hz) / cycles / (1024*1024) : 0;
	printk("      %-10s %4" PRIu64 ".%02" PRIu64 " B/cyc · %5" PRIu64 " MiB/s"
	       "  (%d회, %" PRIu64 " 사이클, sum %016" PRIx64 ")\n",
	       label, bpc100/100, bpc100%100, mibps, reps, cycles, sum);
}

int main(void)
{
	const uint64_t cntfrq = SYSREG_R("cntfrq_el0");

	printk("=== L8g: L8 측정의 정정판 — 캐시가 정말 켜져 있나 ===\n");

	/* ── [0] 진짜 CPU 주파수 (PMU 대 아키텍처 타이머) ───────────────── */
	pmu_enable();
	{
		const uint64_t v0 = SYSREG_R("cntvct_el0"), c0 = cyc();
		/* cntfrq/50 틱 = 20 ms 를 기다린다 */
		const uint64_t want = cntfrq / 50;
		while (SYSREG_R("cntvct_el0") - v0 < want) { /* 바쁜 대기 */ }
		const uint64_t c1 = cyc(), v1 = SYSREG_R("cntvct_el0");
		const uint64_t dv = v1 - v0, dc = c1 - c0;
		const uint64_t hz = dv ? (dc * cntfrq) / dv : 0;
		printk("  [0] CPU 주파수 (PMU/타이머 실측)\n");
		printk("      CNTFRQ %" PRIu64 " Hz · CPU **%" PRIu64 " MHz**"
		       "  (%" PRIu64 " 사이클 / %" PRIu64 " 틱)\n",
		       cntfrq, hz / 1000000, dc, dv);
		printk("      ⚠ 업스트림 U-Boot 는 armclk 를 APLL_HZ=816 MHz 로 둔다\n"
		       "        (cru_rk3568.h · clk_rk3568.c). 데이터시트 최대는 1.8 GHz\n");

		/* ── [1] 캐시가 **켜졌는가** — 아키텍처 레지스터 ────────────── */
		const uint64_t sctlr = SYSREG_R("sctlr_el1");
		const uint64_t ctr   = SYSREG_R("ctr_el0");
		const uint64_t clidr = SYSREG_R("clidr_el1");
		const uint64_t midr  = SYSREG_R("midr_el1");
		const uint64_t cel   = SYSREG_R("CurrentEL");
		printk("  [1] 캐시가 켜졌는가 (아키텍처 레지스터 — QEMU 에서도 읽힌다)\n");
		printk("      SCTLR_EL1 0x%016" PRIx64 "\n", sctlr);
		printk("        M(MMU)=%u  **C(D캐시)=%u**  I(I캐시)=%u\n",
		       (unsigned)(sctlr & 1), (unsigned)((sctlr >> 2) & 1),
		       (unsigned)((sctlr >> 12) & 1));
		printk("      CTR_EL0 0x%08" PRIx64 "  (DminLine %u B · IminLine %u B)\n",
		       ctr, 4u << ((unsigned)(ctr >> 16) & 0xf),
		       4u << ((unsigned)ctr & 0xf));
		printk("      CLIDR_EL1 0x%016" PRIx64 " · MIDR 0x%08" PRIx64
		       " · CurrentEL %u\n", clidr, midr,
		       (unsigned)((cel >> 2) & 3));
		if (((sctlr >> 2) & 1) == 0) {
			printk("      ⚠⚠ **C=0 — D캐시가 꺼져 있다.** 이것이 느림의 주원인이다\n");
		}

		/* ── [2] 구현 정의 레지스터 — 실기에서만 ────────────────────── */
		printk("  [2] 구현 정의 레지스터 (A55)\n");
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
		/* ⚠ 이름을 바로잡았다. TF-A cortex_a55.h 가 정본이다.
		 *   CPUECTLR_EL1 = S3_0_C15_C1_4  (L1WSCTL 비트 26:25 만 정의됨)
		 *   CPUACTLR_EL1 = S3_0_C15_C1_0
		 * SMPEN 비트는 A55 에 **없다** — A53/A57/A72 의 필드다.
		 * 그래서 여기서는 **읽기만** 하고 해석을 붙이지 않는다. */
		printk("      CPUECTLR_EL1 (S3_0_C15_C1_4) 0x%016" PRIx64
		       "  L1WSCTL=%u\n",
		       SYSREG_R("S3_0_C15_C1_4"),
		       (unsigned)((SYSREG_R("S3_0_C15_C1_4") >> 25) & 3));
		printk("      CPUACTLR_EL1 (S3_0_C15_C1_0) 0x%016" PRIx64 "\n",
		       SYSREG_R("S3_0_C15_C1_0"));
		printk("        ⚠ 전 판이 이것을 CPUECTLR 로 **오인**했다. bit6 은\n"
		       "          SMPEN 이 아니고 ARM 이 공개 정의한 바 없다\n");
#else
		printk("      (생략 — QEMU 의 cortex-a55 는 S3_0_C15_* 를 구현하지 않아\n"
		       "       읽으면 미정의 명령으로 죽는다. 실기에서만 읽는다)\n");
#endif

		/* ── [3] 캐시가 **먹는가** — 크기 스윕 ──────────────────────── */
		printk("  [3] 캐시가 먹는가 — 읽기 대역 스윕\n");
		printk("      기준: A55 의 L1 히트는 사이클당 8~16 B 여야 한다.\n"
		       "            L1 과 DRAM 이 비슷하면 캐시가 먹지 않는 것이다.\n");
		const size_t SZ = 32u << 20;
		volatile uint64_t *buf = malloc(SZ);
		if (!buf) {
			printk("      ⚠ 32 MiB 할당 실패 — 스윕을 못 한다\n");
		} else {
			for (size_t i = 0; i < SZ / 8; i++) { buf[i] = i; }
			sweep(buf, 16u << 10,  "16 KiB",  hz);
			sweep(buf, 32u << 10,  "32 KiB",  hz);
			sweep(buf, 128u << 10, "128 KiB", hz);
			sweep(buf, 256u << 10, "256 KiB", hz);
			sweep(buf, 1u << 20,   "1 MiB",   hz);
			sweep(buf, 8u << 20,   "8 MiB",   hz);
			sweep(buf, SZ,         "32 MiB",  hz);
			free((void *)buf);
		}
	}

	printk("  [4] 읽는 법\n");
	printk("      · C=1 이고 16~32 KiB 가 8 B/cyc 이상이면 → **캐시는 정상**.\n");
	printk("        그러면 느림의 원인은 캐시가 아니라 클럭(2.2×)과 코어 수다\n");
	printk("      · C=1 인데 16 KiB 도 1~2 B/cyc 이면 → 캐시가 켜졌는데\n");
	printk("        먹지 않는다. MMU 속성·공유도(PTE SH)를 다음에 본다\n");
	printk("      · C=0 이면 → D캐시가 꺼졌다. 가장 큰 단일 원인이다\n");
	printk("=== L8g 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
