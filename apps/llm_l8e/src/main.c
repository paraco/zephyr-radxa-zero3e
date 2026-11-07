/*
 * L8e — 보조 코어를 올려 **코어마다 SMPEN 과 캐시 대역을 잰다.**
 *
 * ── 왜 이것이 먼저인가 ──────────────────────────────────────────────────
 *
 * L8d 가 확정한 것: 주 코어(EL2 진입)의 `CPUECTLR_EL1.SMPEN = 0` 이고,
 * EL3 가 쓰기를 막아 Zephyr 에서 켤 수 없다. 그래서 캐시가 사실상 무력하고
 * 읽기 대역이 크기와 무관하게 평평하다 (L1 837 · DRAM 707 MiB/s).
 *
 * U-Boot/TF-A 를 다시 빌드하는 길(옵션 C)로 가기 전에 **훨씬 싼 가능성**이
 * 있다:
 *
 *   PSCI `CPU_ON` 으로 보조 코어를 올리면 **BL31 이 그 코어의 리셋 경로를
 *   돈다.** 거기서 SMPEN 을 켜는 것이 보통이다. 즉 **보조 코어는 캐시가
 *   살아 있을 수 있다.**
 *
 * 맞다면 멀티코어가 4배가 아니라 **20~40배**(코어 4배 × 캐시 5~10배)가 되고
 * 재빌드가 필요 없다. 틀리다면 C 로 간다. 어느 쪽이든 **재서** 안다.
 *
 * ── 무엇을 재나 ─────────────────────────────────────────────────────────
 *
 * 코어마다 스레드를 고정해(`k_thread_cpu_pin`) 그 코어에서:
 *   · MPIDR_EL1 — 어느 코어인지
 *   · CPUECTLR_EL1 의 SMPEN — **이 코어는 켜져 있나**
 *   · 32 KiB / 32 MiB 읽기 대역 — **이 코어는 캐시가 먹나**
 *
 * ⚠ 출력을 세마포로 직렬화한다. 네 코어가 printk 를 동시에 하면 줄이 섞여
 *   읽을 수 없다.
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

/* ── PMU 사이클 카운터 — **코어마다** 켜야 한다 ───────────────────────
 * PMCR_EL0·PMCNTENSET_EL0 는 per-CPU 레지스터다. 주 코어에서 켜도 보조
 * 코어에는 적용되지 않는다. probe_core 가 자기 코어에서 부른다.
 */
static void pmu_enable_this_core(void)
{
	uint64_t v = SYSREG_R("pmcr_el0");
	v &= ~(uint64_t)(1U << 3);                           /* D(64분주)를 지운다 */
	v |= (1U << 0) | (1U << 2) | (1U << 6);              /* E | C(리셋) | LC */
	__asm__ volatile("msr pmcr_el0, %0" ::"r"(v));
	__asm__ volatile("msr pmcntenset_el0, %0" ::"r"((uint64_t)1 << 31));
	__asm__ volatile("isb");
}
static inline uint64_t pmccnt(void)
{
	return SYSREG_R("pmccntr_el0");
}

/* 읽기 대역을 **사이클당 바이트 × 100** 으로 돌려준다.
 *
 * ⚠ 전 판은 바이트 단위 로드에 의존성 체인이 있어 캐시가 아니라 **루프
 *   지연**을 쟀다 (815 MHz 에서 L1 이 0.83 B/cyc 로 나왔다 — A55 의 L1
 *   히트로는 터무니없다). 여기서는 L8h 에서 검증된 꼴을 쓴다:
 *   64비트 로드 · 독립 누산기 8개 · 8단 언롤. 그리고 MiB/s 가 아니라
 *   **사이클당 바이트**로 돌려준다 — 클럭이 낮은 것과 캐시가 안 먹는
 *   것을 섞지 않으려면 이 단위여야 한다.
 */
static uint64_t sweep(uintptr_t base, size_t sz)
{
	const volatile uint64_t *p = (const volatile uint64_t *)base;
	const size_t n = sz / 8;
	uint64_t a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,a6=0,a7=0;

	/* 예열 — 첫 바퀴는 캐시를 채운다. 그걸 재면 전부 미스가 섞인다. */
	for (size_t i = 0; i + 8 <= n; i += 8) {
		a0 += p[i]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3];
		a4 += p[i+4]; a5 += p[i+5]; a6 += p[i+6]; a7 += p[i+7];
	}

	int reps = 1;
	while ((size_t)reps * sz < (4u << 20)) { reps *= 2; }

	const uint64_t t0 = pmccnt();
	for (int r = 0; r < reps; r++) {
		for (size_t i = 0; i + 8 <= n; i += 8) {
			a0 += p[i]; a1 += p[i+1]; a2 += p[i+2]; a3 += p[i+3];
			a4 += p[i+4]; a5 += p[i+5]; a6 += p[i+6]; a7 += p[i+7];
		}
	}
	const uint64_t dc = pmccnt() - t0;

	if ((a0+a1+a2+a3+a4+a5+a6+a7) == 0x5a5a5a5aULL) {
		printk("(안 일어난다)");
	}
	return dc ? ((uint64_t)sz * (uint64_t)reps * 100) / dc : 0;
}

#define STACK_SZ 4096
static K_THREAD_STACK_ARRAY_DEFINE(stacks, 4, STACK_SZ);
static struct k_thread threads[4];
static K_SEM_DEFINE(turn, 0, 1);	/* 출력 직렬화 */
static K_SEM_DEFINE(done, 0, 1);

static void probe_core(void *a, void *b, void *c)
{
	const int want = (int)(uintptr_t)a;
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_sem_take(&turn, K_FOREVER);

	/* ⚠ PMU 는 per-CPU 다 — 이 코어에서 켠다. 주 코어에서 켜도 여기엔
	 *   적용되지 않아 사이클이 0 으로 읽히고 대역이 0 으로 나온다. */
	pmu_enable_this_core();

	const uint64_t mpidr = SYSREG_R("mpidr_el1");
	/* ⚠ **정정 (2026-11-07).** 전 판은 S3_0_C15_C1_0 을 `CPUECTLR` 이라 부르고
	 *   bit 6 을 SMPEN 으로 읽었다. 둘 다 틀렸다. ARM 의 TF-A 헤더가 정본이다
	 *   (include/lib/cpus/aarch64/cortex_a55.h):
	 *       CORTEX_A55_CPUECTLR_EL1 = S3_0_C15_C1_4
	 *       CORTEX_A55_CPUACTLR_EL1 = S3_0_C15_C1_0
	 *   그리고 **A55 의 CPUECTLR 에는 SMPEN 비트가 없다** — A55 는 DynamIQ(DSU)
	 *   기반이라 코히런시를 DSU 가 관리한다. SMPEN 은 A53/A57/A72 의 필드다.
	 *   그래서 둘을 **읽기만** 하고 해석을 붙이지 않는다. */
	const uint64_t ectlr = SYSREG_R("S3_0_C15_C1_4");	/* 진짜 CPUECTLR */
	const uint64_t actlr = SYSREG_R("S3_0_C15_C1_0");	/* CPUACTLR */
	const uint64_t sctlr = SYSREG_R("sctlr_el1");

	printk("      코어 %d (Zephyr cpu %d · MPIDR 0x%08x · Aff1=%u Aff0=%u)\n",
	       want, (int)arch_curr_cpu()->id, (uint32_t)mpidr,
	       (unsigned)((mpidr >> 8) & 0xff), (unsigned)(mpidr & 0xff));
	printk("        CPUECTLR(C1_4) 0x%016" PRIx64 " L1WSCTL=%u · "
	       "CPUACTLR(C1_0) 0x%016" PRIx64 "\n",
	       ectlr, (unsigned)((ectlr >> 25) & 3), actlr);
	printk("        SCTLR C=%u I=%u M=%u\n",
	       (unsigned)((sctlr >> 2) & 1), (unsigned)((sctlr >> 12) & 1),
	       (unsigned)(sctlr & 1));
	{
		const uint64_t s32k = sweep(BLOB_ADDR, 32u * 1024);
		const uint64_t s32m = sweep(BLOB_ADDR, 32u * 1024 * 1024);
		printk("        읽기 32 KiB %" PRIu64 ".%02" PRIu64 " B/cyc · "
		       "32 MiB %" PRIu64 ".%02" PRIu64 " B/cyc\n",
		       s32k/100, s32k%100, s32m/100, s32m%100);
		printk("        기준: A55 의 L1 히트는 8~16 B/cyc 여야 한다\n");
	}

	k_sem_give(&done);
}

int main(void)
{
	const uint64_t frq = SYSREG_R("cntfrq_el0");
	printk("=== L8e: 코어마다 SMPEN·캐시를 잰다 ===\n");
	printk("  CONFIG_MP_MAX_NUM_CPUS=%d · 실제 올라온 코어 %d개\n",
	       CONFIG_MP_MAX_NUM_CPUS, (int)arch_num_cpus());

	if (arch_num_cpus() < 2) {
		printk("  ⚠ 보조 코어가 안 올라왔다 — PSCI CPU_ON 이 실패했다.\n");
		printk("    PSCI_VERSION 은 1.1 로 응답했으니 CPU_ON 만 막힌 것이다.\n");
		printk("    (BL31 이 보조 코어 진입점을 거부했을 수 있다)\n");
	}

	printk("  [1] 코어별 측정 (출력은 세마포로 직렬화한다)\n");
	for (int i = 0; i < (int)arch_num_cpus(); i++) {
		k_tid_t t = k_thread_create(&threads[i], stacks[i], STACK_SZ,
					    probe_core, (void *)(uintptr_t)i,
					    (void *)(uintptr_t)frq, NULL,
					    K_PRIO_COOP(5), 0, K_FOREVER);
		/*
		 * ⚠ 고정이 **실패할 수 있다** — 그러면 그 코어가 아니라 아무
		 *   코어에서 돌아 측정이 거짓말을 한다. 그래서 스레드가 자기
		 *   `arch_curr_cpu()->id` 를 함께 찍는다. 요청한 번호와 다르면
		 *   그 줄은 믿지 않는다.
		 */
		const int rc = k_thread_cpu_pin(t, i);
		if (rc != 0) {
			printk("      ⚠ 코어 %d 고정 실패 (%d) — 그 줄의 코어 번호를"
			       " 믿지 마라\n", i, rc);
		}
		k_thread_start(t);
		k_sem_give(&turn);
		k_sem_take(&done, K_FOREVER);
	}

	printk("  [2] 읽는 법\n");
	printk("      · 보조 코어가 SMPEN=1 이고 32 KiB 가 32 MiB 보다 훨씬 빠르면\n");
	printk("        → BL31 이 CPU_ON 경로에서 켜 준 것이다. **C(재빌드)가 불필요**\n");
	printk("      · 네 코어 전부 SMPEN=0 이면 → BL31 도 안 켠다. C 로 간다\n");
	printk("=== L8e 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
