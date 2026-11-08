/*
 * llm_bigheap — 큰 할당(≥ 1 MiB)을 llm_heap 구역(0x10000000 + 768 MiB, L9c 검증)으로 돌린다.
 *
 * ggml 은 큰 버퍼를 ggml_aligned_malloc → posix_memalign 으로 받고 ggml_aligned_free → free 로
 * 돌려준다. 링커 --wrap=posix_memalign · --wrap=free 로 두 함수를 감싼다:
 *   posix_memalign(size ≥ 1 MiB) → sys_heap(llm_heap) · 그 밖 → 원래 함수
 *   free(구역 안 주소) → sys_heap · 그 밖 → 원래 함수
 * repack 가중치(약 296 MiB)와 큰 계산 버퍼가 여기에 놓인다. malloc 아레나(240 MiB)는 그대로.
 *
 * 쓰는 앱 CMakeLists:
 *   EXTRA_DTC_OVERLAY_FILE 에 llm_common/llm_heap.overlay
 *   target_sources(app PRIVATE …/llm_bigheap.c)
 *   zephyr_ld_options(-Wl,--wrap=posix_memalign -Wl,--wrap=free)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/init.h>
#include <errno.h>
#include <stdlib.h>
#include "llm_bigheap.h"

#define BH_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(llm_heap)))
#define BH_SIZE ((size_t)DT_REG_SIZE(DT_NODELABEL(llm_heap)))
#define BH_MIN  (1u << 20)

static struct sys_heap bh;
static struct k_spinlock lock;
static bool ready;
static size_t used, peak;

int __real_posix_memalign(void **p, size_t al, size_t sz);
void __real_free(void *p);

static inline bool inside(const void *p)
{
	return (uintptr_t)p >= BH_ADDR && (uintptr_t)p < BH_ADDR + BH_SIZE;
}

int __wrap_posix_memalign(void **p, size_t al, size_t sz)
{
	if (ready && sz >= BH_MIN) {
		k_spinlock_key_t k = k_spin_lock(&lock);
		void *m = sys_heap_aligned_alloc(&bh, al < 64 ? 64 : al, sz);
		if (m) {
			used += sys_heap_usable_size(&bh, m);
			if (used > peak) { peak = used; }
		}
		k_spin_unlock(&lock, k);
		if (m) { *p = m; return 0; }
		/* 구역이 차면 원래 아레나로 넘어간다 */
	}
	return __real_posix_memalign(p, al, sz);
}

void __wrap_free(void *p)
{
	if (p && inside(p)) {
		k_spinlock_key_t k = k_spin_lock(&lock);
		used -= sys_heap_usable_size(&bh, p);
		sys_heap_free(&bh, p);
		k_spin_unlock(&lock, k);
		return;
	}
	__real_free(p);
}

size_t llm_bigheap_used(void) { return used; }
size_t llm_bigheap_peak(void) { return peak; }
size_t llm_bigheap_size(void) { return BH_SIZE; }

static int llm_bigheap_init(void)
{
	sys_heap_init(&bh, (void *)BH_ADDR, BH_SIZE);
	ready = true;
	return 0;
}
SYS_INIT(llm_bigheap_init, POST_KERNEL, 10);
