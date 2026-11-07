/*
 * L8b — **CPU 클럭을 직접 잰다.** 추정이 아니라 사이클 카운터로.
 *
 * ── 왜 ──────────────────────────────────────────────────────────────────
 *
 * L8a 에서 캐시 가설이 **깨졌다**:
 *     SCTLR_EL1 M=1 C=1 I=1        — 캐시 켜져 있다
 *     sram0 읽기     695 MiB/s
 *     모델 창 읽기   695 MiB/s     — 같다. MMU 속성 문제가 아니다
 * 대신 계산 루프가 느렸다 — 5천만 반복(의존 곱 사슬)이 1616 ms.
 * 반복당 4~5 사이클로 보면 **120~155 MHz** 가 나온다. A55 는 1.8 GHz 다.
 *
 * ⚠ 그것은 **내 사이클 추정에 의존한 결론**이다. 추정을 빼고 잰다 —
 *   PMU 사이클 카운터(PMCCNTR_EL0)와 24 MHz arch 타이머를 같이 읽으면
 *   CPU 주파수가 **직접** 나온다.
 *
 * 짚이는 것: RK3566 은 U-Boot 가 보수적인 저주파로 두고 리눅스 cpufreq 가
 * 1.8 GHz 로 올린다. Zephyr 에는 이 SoC 용 CPU 클럭 드라이버가 없으니
 * **부트로더 기본값 그대로** 돌 것이다. 맞다면 L6c 의 23.6초/토큰 중
 * 큰 몫이 그것으로 설명된다.
 *
 * ⚠ 이 칸도 **판정하지 않는다.** 재서 보고한다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <inttypes.h>

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))

static inline uint64_t rd(const char *unused) { (void)unused; return 0; }

#define SYSREG_R(name)                                                         \
	({                                                                     \
		uint64_t _v;                                                   \
		__asm__ volatile("mrs %0, " name : "=r"(_v));                  \
		_v;                                                            \
	})
#define SYSREG_W(name, v) __asm__ volatile("msr " name ", %0" ::"r"((uint64_t)(v)))

static inline uint64_t cntvct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}
static inline uint64_t pmccntr(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, pmccntr_el0" : "=r"(v));
	return v;
}

/*
 * PMU 사이클 카운터를 켠다.
 *   PMCR_EL0.E (bit 0)      = 1   카운터 전체 활성
 *   PMCR_EL0.LC (bit 6)     = 1   64비트 사이클 카운터
 *   PMCNTENSET_EL0 bit 31   = 1   사이클 카운터 활성
 *   PMUSERENR_EL0           는 EL0 접근용 — 우리는 EL1 이라 필요 없다
 *
 * ⚠ 이 레지스터가 **트랩될 수 있다.** EL2/EL3 가 MDCR_EL2.TPM 또는
 *   MDCR_EL3.TPM 을 켜 두면 접근이 예외로 간다. TF-A 가 그렇게 둘 수
 *   있으므로, 읽은 값이 **움직이지 않으면** 그것으로 판단한다 —
 *   예외가 나면 Zephyr 가 치명 오류를 찍으므로 조용히 틀리지는 않는다.
 */
static bool pmu_enable(void)
{
	SYSREG_W("pmcr_el0", (1ULL << 6) | 1ULL);	/* LC | E */
	SYSREG_W("pmcntenset_el0", 1ULL << 31);
	__asm__ volatile("isb");
	const uint64_t a = pmccntr();
	/* 조금 일하게 둔다 */
	volatile uint64_t x = 0;
	for (int i = 0; i < 10000; i++) {
		x += (uint64_t)i;
	}
	(void)x;
	const uint64_t b = pmccntr();
	return b != a;
}

int main(void)
{
	printk("=== L8b: CPU 클럭을 직접 잰다 ===\n");

	const uint64_t frq = SYSREG_R("cntfrq_el0");
	printk("  arch 타이머 CNTFRQ = %" PRIu64 " Hz\n", frq);

	printk("  PMU 사이클 카운터 ... ");
	if (!pmu_enable()) {
		printk("움직이지 않는다 — EL2/EL3 가 PMU 접근을 트랩하는 듯\n");
		printk("      → 사이클로는 못 잰다. 아래 간접 측정만 쓴다.\n");
	} else {
		printk("OK\n");
		/*
		 * CPU 주파수 = 사이클 / 시간.
		 * arch 타이머는 CPU 클럭과 **무관한** 24 MHz 이므로 기준이 된다.
		 */
		for (int round = 0; round < 3; round++) {
			const uint64_t c0 = pmccntr();
			const uint64_t t0 = cntvct();
			/* 0.2초쯤 바쁘게 */
			volatile uint64_t s = 0;
			while (cntvct() - t0 < frq / 5) {
				s += 1;
			}
			const uint64_t c1 = pmccntr();
			const uint64_t t1 = cntvct();
			(void)s;
			const uint64_t cyc = c1 - c0;
			const uint64_t tk = t1 - t0;
			const uint64_t hz = tk ? (uint64_t)((__uint128_t)cyc * frq / tk) : 0;
			printk("      %d회차: 사이클 %" PRIu64 " / 타이머 %" PRIu64
			       " tick → **%" PRIu64 " MHz**\n",
			       round + 1, cyc, tk, hz / 1000000);
		}
	}

	/*
	 * 간접 측정 — PMU 가 막혀도 쓸 수 있다.
	 * 의존 사슬 하나짜리 루프. A55 의 64비트 MUL 지연은 3사이클이고
	 * ADD 가 1사이클이라 반복당 **약 4사이클**이다(사슬이 끊기지 않는다).
	 * ⚠ 이것은 추정이 들어간 값이다. PMU 가 되면 위 값을 믿는다.
	 */
	printk("  간접 측정 (의존 곱 사슬, 반복당 ~4사이클 가정)\n");
	{
		const uint64_t t0 = cntvct();
		uint64_t a = 1;
		for (uint32_t i = 0; i < 20u * 1000u * 1000u; i++) {
			a = a * 6364136223846793005ULL + 1442695040888963407ULL;
		}
		const uint64_t tk = cntvct() - t0;
		volatile uint64_t sink = a;
		(void)sink;
		const uint64_t ns = tk * 1000000000ULL / frq;
		printk("      2천만 반복 %" PRIu64 " ms → 반복당 %" PRIu64 " ns"
		       " → 4사이클이면 **%" PRIu64 " MHz**\n",
		       ns / 1000000, ns / 20000000,
		       tk ? (4ULL * 20000000ULL * frq / tk) / 1000000 : 0);
	}

	/* 메모리 대역을 타이머로 다시 — ms 해상도로는 작은 영역을 못 쟀다 */
	printk("  메모리 읽기 (arch 타이머, 41.7 ns 해상도)\n");
	{
		const size_t sizes[] = { 32u * 1024, 256u * 1024, 4u * 1024 * 1024,
					 32u * 1024 * 1024 };
		const char *names[] = { "32 KiB (L1)", "256 KiB (L2)", "4 MiB", "32 MiB" };
		for (int k = 0; k < 4; k++) {
			const volatile uint64_t *p = (const volatile uint64_t *)BLOB_ADDR;
			const size_t n = sizes[k] / 8;
			/* 한 번 훑어 캐시에 올린다 */
			uint64_t acc = 0;
			for (size_t i = 0; i < n; i++) {
				acc += p[i];
			}
			const uint64_t t0 = cntvct();
			const int reps = (int)(64u * 1024u * 1024u / sizes[k]);
			for (int r = 0; r < reps; r++) {
				for (size_t i = 0; i + 8 <= n; i += 8) {
					acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3]
					     + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
				}
			}
			const uint64_t tk = cntvct() - t0;
			const uint64_t bytes = (uint64_t)sizes[k] * (uint64_t)reps;
			const uint64_t mibps = tk ? bytes * frq / tk / 1048576 : 0;
			printk("      %-14s %" PRIu64 " MiB/s  (%" PRIu64 " MiB 를 %" PRIu64 " ms)\n",
			       names[k], mibps, bytes / 1048576, tk * 1000 / frq);
			if (acc == 0x5a5a5a5a5a5a5a5aULL) {
				printk("(일어나지 않는다)");
			}
		}
	}

	printk("  읽는 법: L1/L2 가 32 MiB 보다 **훨씬** 빠르면 캐시 계층은 정상이다.\n");
	printk("           그런데 클럭이 1.8 GHz 보다 낮으면 그것이 느림의 주원인이다.\n");
	printk("=== L8b 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
