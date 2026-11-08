/*
 * 부팅 슬롯 표식 읽기 — include/rk3566_boot_slot.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/devicetree.h>
#include <rk3566_boot_slot.h>

#define RELAY_PHYS 0x51000000UL		/* boot.cmd 의 중계 구역 (L9a 와 같다) */
#define SLOT_MAGIC 0x544f4c53u		/* 'SLOT' */

static const char *slot = "-";

const char *rk3566_boot_slot(void) { return slot; }

static int boot_slot_init(void)
{
	mm_reg_t r;
#if DT_NODE_EXISTS(DT_NODELABEL(hub_relay))
	/* ⚠ hubllm 은 0x51000000 을 DT 구역(hub_relay, Normal-NC)으로 이미 identity 매핑한다.
	 *   이 Zephyr 의 device_map 도 identity 라, 같은 주소를 Device 속성으로 다시 매핑하면
	 *   set_mapping 이 -EBUSY 로 거절하고 arch_mem_map 이 패닉한다(2026-11-08 실기 두 번).
	 *   그 구역이 있으면 그대로 읽는다. */
	r = (mm_reg_t)DT_REG_ADDR(DT_NODELABEL(hub_relay));
	BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(hub_relay)) == RELAY_PHYS, "hub_relay 는 0x51000000 이어야 한다");
#else
	device_map(&r, RELAY_PHYS, 0x1000, K_MEM_CACHE_NONE);
#endif
	if (sys_read32(r + 0x18) == SLOT_MAGIC) {
		switch (sys_read32(r + 0x1c)) {
		case 1: slot = "try"; break;
		case 2: slot = "main"; break;
		case 3: slot = "main-alt"; break;
		default: break;
		}
	}
	/* 다음 부팅에 남지 않게 지운다 — boot.cmd 도 시작할 때 지우지만 겹쳐 둔다 */
	sys_write32(0, r + 0x18);
	return 0;
}
SYS_INIT(boot_slot_init, POST_KERNEL, 1);
