/*
 * L9d — U-Boot 는 자신을 DRAM 꼭대기로 옮긴 뒤 블롭을 0x58000000 에 읽는다. 블롭 창을
 * 512 → 576 MiB(끝 0x7C000000)로 늘려도 U-Boot 와 겹치지 않는지 본다. 읽기만 한다.
 *
 *   [1] 0x78000000 ~ 0x80000000 에서 "U-Boot" 문자열 위치 (옮겨진 U-Boot 본체)
 *   [2] 1 MiB 마다 0 인 64비트 비율 — U-Boot 는 malloc 구역을 0 으로 채운다.
 *       켠 직후 DRAM 은 0 이 아닌 값이 많아, 거의 0 인 구역이 U-Boot 가 쓴 곳이다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/device_mmio.h>
#include <string.h>
#include <moru_led.h>

#define BASE 0x78000000UL
#define SIZE (128u << 20)
#define MB   (1u << 20)

int main(void)
{
	printk("=== L9d: DRAM 꼭대기 0x%08lx ~ 0x80000000 조사 ===\n", BASE);
	moru_led_set(MORU_LED_INFER);
	mm_reg_t m;
	device_map(&m, BASE, SIZE, K_MEM_CACHE_NONE);
	const uint8_t *p = (const uint8_t *)m;

	printk("  [1] \"U-Boot\" 문자열\n");
	int hits = 0;
	for (size_t i = 0; i + 6 < SIZE && hits < 12; i++) {
		if (p[i] == 'U' && !memcmp(p + i, "U-Boot ", 7)) {
			char s[48] = {0};
			for (int k = 0; k < 47 && p[i + k] >= 0x20 && p[i + k] < 0x7f; k++) { s[k] = p[i + k]; }
			printk("      0x%08lx  %s\n", BASE + i, s);
			hits++;
			i += 64;
		}
	}
	printk("  [2] 1 MiB 마다 0 인 64비트 비율 (%%) — 연속 구간으로 묶는다\n");
	int prev = -1;
	size_t run0 = 0;
	for (size_t off = 0; off <= SIZE; off += MB) {
		int pct = -2;
		if (off < SIZE) {
			const uint64_t *q = (const uint64_t *)(p + off);
			size_t z = 0;
			for (size_t k = 0; k < MB / 8; k++) { z += (q[k] == 0); }
			pct = (int)(z * 100 / (MB / 8));
			pct = pct >= 99 ? 100 : (pct <= 1 ? 0 : 50);	/* 0 · 섞임 · 전부 0 으로 묶는다 */
		}
		if (pct != prev) {
			if (prev != -1) {
				printk("      0x%08lx ~ 0x%08lx  %s\n", BASE + run0, BASE + off,
				       prev == 100 ? "전부 0 (U-Boot malloc 등)" : prev == 0 ? "0 아님 (켠 직후 값/데이터)" : "섞임");
			}
			prev = pct;
			run0 = off;
		}
	}
	printk("=== L9d 조사 끝 ===\n");
	moru_led_set(MORU_LED_READY);
	k_sleep(K_FOREVER);
	return 0;
}
