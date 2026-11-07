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

static uint64_t sweep(uintptr_t base, size_t sz, uint64_t frq)
{
	const volatile uint64_t *p = (const volatile uint64_t *)base;
	const size_t n = sz / 8;
	uint64_t acc = 0;
	for (size_t i = 0; i < n; i++) {
		acc += p[i];
	}
	const int reps = (int)(16u * 1024u * 1024u / sz);
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

#define STACK_SZ 4096
static K_THREAD_STACK_ARRAY_DEFINE(stacks, 4, STACK_SZ);
static struct k_thread threads[4];
static K_SEM_DEFINE(turn, 0, 1);	/* 출력 직렬화 */
static K_SEM_DEFINE(done, 0, 1);

static void probe_core(void *a, void *b, void *c)
{
	const int want = (int)(uintptr_t)a;
	const uint64_t frq = (uint64_t)(uintptr_t)b;
	ARG_UNUSED(c);

	k_sem_take(&turn, K_FOREVER);

	const uint64_t mpidr = SYSREG_R("mpidr_el1");
	const uint64_t ectlr = SYSREG_R("S3_0_C15_C1_0");
	const uint64_t sctlr = SYSREG_R("sctlr_el1");

	printk("      코어 %d (Zephyr cpu %d · MPIDR 0x%08x · Aff0=%u)\n",
	       want, (int)arch_curr_cpu()->id, (uint32_t)mpidr,
	       (unsigned)(mpidr & 0xff));
	printk("        CPUECTLR 0x%016" PRIx64 "  **SMPEN=%u**  (SCTLR C=%u I=%u)\n",
	       ectlr, (unsigned)((ectlr >> 6) & 1),
	       (unsigned)((sctlr >> 2) & 1), (unsigned)((sctlr >> 12) & 1));
	printk("        읽기 32 KiB %5" PRIu64 " MiB/s · 32 MiB %5" PRIu64 " MiB/s",
	       sweep(BLOB_ADDR, 32u * 1024, frq),
	       sweep(BLOB_ADDR, 32u * 1024 * 1024, frq));
	printk("   ← 둘이 크게 다르면 **이 코어는 캐시가 먹는다**\n");

	k_sem_give(&done);
}

int main(void)
{
	const uint64_t frq = SYSREG_R("cntfrq_el0");
	printk("=== L8k: SMP 만 (PSCI 없음) — 실기에서 PSCI 를 분리한다 ===\n");
	printk("  CONFIG_MP_MAX_NUM_CPUS=%d · 실제 올라온 코어 %d개\n",
	       CONFIG_MP_MAX_NUM_CPUS, (int)arch_num_cpus());

	/* L8f 는 코어 1개가 **의도**다. 보조 코어를 올리지 않는다 — CPU_ON 을
	 * 한 번도 부르지 않는 판이다. 이 배너가 보이면
	 *   SMP 골격 + psci_init(PRE_KERNEL_1 의 PSCI_VERSION SMC) 은 무죄이고
	 *   L8e 의 침묵은 **CPU_ON** 때문이다.
	 * 이 배너도 안 보이면 SMP 골격이나 psci_init 이 범인이다.
	 */

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
	printk("=== L8k 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
