/*
 * L9c — llm_heap 후보 구역 검증 (0x10000000 + 768 MiB) + DRAM 대역폭.
 *
 *   [1] 64 MiB 덩이마다: 주소에서 만든 64 비트 패턴을 쓰고 → 캐시를 밀어내고 → 다시 읽어 대조.
 *       덩이마다 한 줄 — 보안 구역이라 접근이 막히면 그 덩이에서 fault 로 멈춘다(위치가 남는다).
 *   [2] 대역폭 (한 코어): 쓰기 · 읽기 · memcpy (llm_heap 안), 블롭 구역 읽기.
 *
 * ⚠ 구역 안의 내용은 지운다 — 쓰는 곳이 없어야 하는 자리라 지워도 되는지 자체가 시험이다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/cache.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <inttypes.h>
#include <moru_led.h>

#define HEAP_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(llm_heap)))
#define HEAP_SIZE ((size_t)DT_REG_SIZE(DT_NODELABEL(llm_heap)))
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define CHUNK (64u << 20)

static inline uint64_t pat(uintptr_t a) { return (uint64_t)a * 0x9e3779b97f4a7c15ull ^ 0x5a5a5a5a5a5a5a5aull; }

static double mbps(size_t bytes, uint64_t cyc)
{
	return (double)bytes / 1048576.0 / ((double)cyc / sys_clock_hw_cycles_per_sec());
}

int main(void)
{
	printk("=== L9c: llm_heap 0x%08lx + %zu MiB 검증 ===\n", (unsigned long)HEAP_ADDR, HEAP_SIZE >> 20);
	moru_led_set(MORU_LED_INFER);
	int bad_total = 0;
	for (size_t off = 0; off < HEAP_SIZE; off += CHUNK) {
		volatile uint64_t *p = (volatile uint64_t *)(HEAP_ADDR + off);
		const size_t n = CHUNK / 8;
		printk("  [1] 0x%08lx … ", (unsigned long)(HEAP_ADDR + off));
		for (size_t i = 0; i < n; i++) { p[i] = pat((uintptr_t)&p[i]); }
		sys_cache_data_flush_and_invd_range((void *)p, CHUNK);
		int bad = 0;
		for (size_t i = 0; i < n; i++) {
			if (p[i] != pat((uintptr_t)&p[i])) {
				if (bad < 3) { printk("\n      ✗ 0x%08lx 읽음 %016" PRIx64, (unsigned long)(uintptr_t)&p[i], (uint64_t)p[i]); }
				bad++;
			}
		}
		bad_total += bad;
		printk("%s\n", bad ? " ✗" : "OK");
	}
	printk("  [1] 결과: 틀린 64비트 %d 개 / %zu 개\n", bad_total, HEAP_SIZE / 8);

	/* [2] 대역폭 — 256 MiB 쓰기·읽기·복사 (한 코어) */
	const size_t B = 256u << 20;
	uint64_t *h = (uint64_t *)HEAP_ADDR;
	uint64_t t0 = k_cycle_get_64();
	memset(h, 0x11, B);
	uint64_t t1 = k_cycle_get_64();
	volatile uint64_t s = 0;
	for (size_t i = 0; i < B / 8; i += 8) {
		s += h[i] + h[i + 1] + h[i + 2] + h[i + 3] + h[i + 4] + h[i + 5] + h[i + 6] + h[i + 7];
	}
	uint64_t t2 = k_cycle_get_64();
	memcpy((uint8_t *)h + B, h, B);
	uint64_t t3 = k_cycle_get_64();
	const uint64_t *b = (const uint64_t *)BLOB_ADDR;
	for (size_t i = 0; i < B / 8; i += 8) {
		s += b[i] + b[i + 1] + b[i + 2] + b[i + 3] + b[i + 4] + b[i + 5] + b[i + 6] + b[i + 7];
	}
	uint64_t t4 = k_cycle_get_64();
	printk("  [2] 한 코어 대역폭 (256 MiB): 쓰기 %.0f MB/s · 읽기 %.0f MB/s · 복사 %.0f MB/s · 블롭 읽기 %.0f MB/s\n",
	       mbps(B, t1 - t0), mbps(B, t2 - t1), mbps(B, t3 - t2), mbps(B, t4 - t3));
	printk("=== L9c 측정 끝 (%s) ===\n", bad_total ? "✗ 구역 이상" : "✓ 구역 정상");
	moru_led_set(bad_total ? MORU_LED_FAIL : MORU_LED_READY);
	(void)s;
	k_sleep(K_FOREVER);
	return 0;
}
