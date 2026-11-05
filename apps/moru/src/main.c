/*
 * MoruLab Zephyr RTOS — Radxa ZERO 3E.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 셸·로그는 UART2. 부팅 메시지는 moru_boot.c. 여기서는 생존 신호 LED 만 돌린다.
 * printk 는 쓰지 않는다 (동기 출력이 USB 열거를 깨뜨렸다).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

void moru_mark(const char *s);

#define GPIO0_PHYS		0xfdd60000UL
#define GPIO0_SIZE		0x100
#define RK_GPIO_SWPORT_DR_L	0x00
#define RK_GPIO_SWPORT_DDR_L	0x08
#define LED_PIN			0U

static inline uint32_t rk_gpio_val(uint32_t pin, bool on)
{
	uint32_t we = BIT(pin + 16);

	return on ? (we | BIT(pin)) : we;
}

static void led_thread(void *a, void *b, void *c)
{
	mm_reg_t base;

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	device_map(&base, GPIO0_PHYS, GPIO0_SIZE, K_MEM_CACHE_NONE);
	sys_write32(rk_gpio_val(LED_PIN, true), base + RK_GPIO_SWPORT_DDR_L);

	while (1) {
		sys_write32(rk_gpio_val(LED_PIN, true),
			    base + RK_GPIO_SWPORT_DR_L);
		k_msleep(2000);
		sys_write32(rk_gpio_val(LED_PIN, false),
			    base + RK_GPIO_SWPORT_DR_L);
		k_msleep(2000);
	}
}

/* ⚠ 스택 4096 — arm64 에서 1024 는 크래시한다 (usbuart 앱 주석 참고) */
K_THREAD_DEFINE(led_tid, 4096, led_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	moru_mark("[M]");
	LOG_INF("MoruLab Zephyr RTOS is up — 'help' 로 명령 목록");
	k_sleep(K_FOREVER);
	return 0;
}
