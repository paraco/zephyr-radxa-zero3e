/*
 * L8a — **왜 느린가**를 재는 칸. 성능 플래그를 뒤집기 전에 온다.
 *
 * ── 왜 이 칸이 먼저인가 ─────────────────────────────────────────────────
 *
 * L6c 가 실기에서 토큰당 23.6초를 냈다. 가중치 497.39 MiB 를 매 토큰 읽으니
 * 유효 처리율이 **21.1 MiB/s** 다. A55 한 코어로 0.8B Q4 는 보통 1~4 tok/s
 * 이므로 우리는 **25~100배 느리다**. "한 코어라서" 로는 설명되지 않는다.
 *
 * 더 수상한 것은 L1 의 수치다:
 *     전 영역 쓰기 512 MiB ... 2173 ms  (235 MiB/s)
 *     CRC32 ............... 43260 ms  (**11.8 MiB/s**)
 * 바이트 테이블 CRC32 은 A55 에서 100~200 MiB/s 가 나와야 한다. 10배 느리면
 * **캐시가 먹지 않는다**는 뜻이다 — 줄마다 DRAM 왕복이고 재사용이 없다.
 *
 * 그래서 플래그(repack · -ffp-contract · 멀티코어)를 건드리기 전에
 * **메모리가 캐시되는지**를 가린다. 그것이 틀렸으면 플래그는 아무 효과가 없고,
 * 고치면 한 번에 몇 배가 바뀐다.
 *
 * ⚠ 이 칸은 **판정하지 않는다** — 재서 보고한다. 기준값을 모르는 상태에서
 *   PASS/FAIL 을 매기면 그 기준이 곧 추측이 된다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/cache.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(BLOB_NODE))

#define TEST_MB   32u
#define TEST_SIZE (TEST_MB * 1024u * 1024u)

static inline uint64_t rd_sctlr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
	return v;
}
static inline uint64_t rd_ctr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(v));
	return v;
}
static inline uint64_t rd_midr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(v));
	return v;
}
static inline uint64_t rd_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}
static inline uint64_t rd_cntvct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

/*
 * 순차 읽기 대역. 64비트씩 8개를 한 묶음으로 읽어 더한다.
 * ⚠ volatile 로 읽어 컴파일러가 루프를 지우지 못하게 한다. 지워지면
 *   "무한 대역" 이 나와 **측정이 거짓말을 한다**.
 */
static uint64_t read_bw_mibps(const volatile uint64_t *p, size_t bytes, int64_t *ms_out)
{
	const size_t n = bytes / sizeof(uint64_t);
	uint64_t acc = 0;
	const int64_t t0 = k_uptime_get();
	for (size_t i = 0; i + 8 <= n; i += 8) {
		acc += p[i] + p[i + 1] + p[i + 2] + p[i + 3]
		     + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
	}
	const int64_t ms = k_uptime_get() - t0;
	*ms_out = ms;
	/* acc 를 쓰지 않으면 루프가 지워진다 */
	if (acc == 0x0123456789abcdefULL) {
		printk("(일어나지 않는다)");
	}
	return ms > 0 ? (uint64_t)(bytes / 1024 / 1024) * 1000 / (uint64_t)ms : 0;
}

int main(void)
{
	printk("=== L8a: 왜 느린가 (측정만, 판정 없음) ===\n");

	/* [1] 하드웨어·캐시 상태 */
	const uint64_t sctlr = rd_sctlr();
	const uint64_t ctr   = rd_ctr();
	printk("  [1] CPU·캐시 상태\n");
	printk("      MIDR      0x%08x\n", (uint32_t)rd_midr());
	printk("      SCTLR_EL1 0x%016" PRIx64 "\n", sctlr);
	printk("        M(MMU)=%u  C(D캐시)=%u  I(I캐시)=%u\n",
	       (unsigned)(sctlr & 1u), (unsigned)((sctlr >> 2) & 1u),
	       (unsigned)((sctlr >> 12) & 1u));
	printk("      CTR_EL0   0x%08x  (D줄 %u B · I줄 %u B)\n",
	       (uint32_t)ctr, 4u << ((ctr >> 16) & 0xf), 4u << (ctr & 0xf));
	printk("      Zephyr    DCACHE_LINE_SIZE=%d · sys_cache_data_line_size_get()=%zu\n",
	       CONFIG_DCACHE_LINE_SIZE, sys_cache_data_line_size_get());
	printk("      CNTFRQ    %" PRIu64 " Hz (arch 타이머)\n", rd_cntfrq());
	printk("      CPU 수    %d (CONFIG_MP_MAX_NUM_CPUS)\n", CONFIG_MP_MAX_NUM_CPUS);

	/*
	 * [2] 계산만 — 메모리를 거의 안 쓴다. 클럭이 정상인지 가린다.
	 *     캐시가 문제라면 이 값은 **정상**이어야 한다.
	 */
	printk("  [2] 계산 루프 (메모리 없음) ... ");
	{
		const uint64_t c0 = rd_cntvct();
		const int64_t t0 = k_uptime_get();
		volatile uint64_t x = 1;
		uint64_t a = 1, b = 3;
		for (uint32_t i = 0; i < 50u * 1000u * 1000u; i++) {
			a = a * 6364136223846793005ULL + 1442695040888963407ULL;
			b ^= a >> 17;
		}
		x = b;
		const int64_t ms = k_uptime_get() - t0;
		const uint64_t cyc = rd_cntvct() - c0;
		printk("%" PRId64 " ms · 타이머 %" PRIu64 " tick\n", ms, cyc);
		printk("      → 5천만 반복 (곱+xor 2연산) = %" PRId64 " Mops/s\n",
		       ms > 0 ? (int64_t)(100 * 1000 / ms) : 0);
		(void)x;
	}

	/* [3] sram0(힙) 읽기 대역 — 아레나는 sram0 안이다 */
	printk("  [3] sram0(힙) 읽기 %u MiB ... ", TEST_MB);
	{
		void *buf = malloc(TEST_SIZE);
		if (buf == NULL) {
			printk("할당 실패 (아레나 %u MiB)\n",
			       (unsigned)(CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE / 1024 / 1024));
		} else {
			/* 먼저 채워 둔다 — 안 쓴 페이지를 읽으면 뜻이 없다 */
			uint64_t *q = (uint64_t *)buf;
			for (size_t i = 0; i < TEST_SIZE / 8; i++) {
				q[i] = i;
			}
			int64_t ms = 0;
			uint64_t bw = read_bw_mibps((const volatile uint64_t *)buf,
						    TEST_SIZE, &ms);
			printk("%" PRIu64 " MiB/s (%" PRId64 " ms)\n", bw, ms);
			/* 2회차 — 32 MiB 는 L2(512 KiB)보다 크므로 거의 같아야 한다 */
			bw = read_bw_mibps((const volatile uint64_t *)buf, TEST_SIZE, &ms);
			printk("      2회차 %" PRIu64 " MiB/s (%" PRId64 " ms)\n", bw, ms);
			/* 작은 영역 — L2 에 들어간다. 캐시가 먹으면 **훨씬** 빨라야 한다 */
			bw = read_bw_mibps((const volatile uint64_t *)buf,
					   256u * 1024u, &ms);
			printk("      256 KiB 반복 (L2 안) %" PRIu64 " MiB/s"
			       " — 32 MiB 보다 훨씬 빨라야 캐시가 먹는 것이다\n", bw);
			free(buf);
		}
	}

	/*
	 * [4] 모델 창 읽기 대역 — ggml 이 가중치를 읽는 바로 그 메모리다.
	 *
	 * ⚠ [3]과 **크게 다르면** 창의 MMU 속성이 문제다
	 *   (apps/llm_common/model_blob.overlay 의 zephyr,memory-attr).
	 */
	printk("  [4] 모델 창 읽기 %u MiB (0x%08" PRIxPTR ") ... ",
	       TEST_MB, (uintptr_t)BLOB_ADDR);
	{
		int64_t ms = 0;
		uint64_t bw = read_bw_mibps((const volatile uint64_t *)BLOB_ADDR,
					    TEST_SIZE, &ms);
		printk("%" PRIu64 " MiB/s (%" PRId64 " ms)\n", bw, ms);
		bw = read_bw_mibps((const volatile uint64_t *)BLOB_ADDR,
				   256u * 1024u, &ms);
		printk("      256 KiB 반복 (L2 안) %" PRIu64 " MiB/s\n", bw);
	}

	/* [5] 작은 영역 256 KiB 를 sram0 과 모델 창에서 — 캐시 효과 대조 */
	printk("  [5] 읽은 것을 어떻게 보나\n");
	printk("      · [2]가 느리면 클럭·전원 문제다 (캐시가 아니다)\n");
	printk("      · [3] 32 MiB 가 100 MiB/s 미만이면 **D캐시가 안 먹는다**\n");
	printk("      · [3] 256 KiB 가 32 MiB 와 비슷하면 **캐시가 꺼져 있다**\n");
	printk("      · [4]가 [3]보다 크게 느리면 **모델 창의 MMU 속성**이 문제다\n");
	printk("=== L8a 측정 끝 ===\n");
	printk("    (여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
