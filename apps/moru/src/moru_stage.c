/*
 * 단계 표시 LED — 시리얼 없이 "어디까지 가다 멈췄는지" 를 읽는 장치.
 *
 * 왜 필요한가. main.c 의 LED 스레드는 우선순위 7 이라 main 스레드(0)가
 * 양보해야 비로소 돌아간다. 그래서 init 중에 어디서든 멈추면 LED 가 한 번도
 * 켜지지 않고, 셸이 못 뜨면 로그도 안 나와 보드가 완전히 무음이 된다.
 * 이 파일은 각 init 레벨에서 LED 를 그 단계 번호만큼 깜박인다. 사람이 세어
 * 본 마지막 횟수가 마지막으로 통과한 단계다.
 *
 *   1회 → PRE_KERNEL_1 통과
 *   2회 → PRE_KERNEL_2 통과
 *   3회 → POST_KERNEL 통과   (셸 백엔드가 여기서 뜬다)
 *   4회 → APPLICATION 통과   (USB 스택이 여기서 뜬다)
 *   그 뒤 2초 주기 느린 깜박임 → main() 도달, 정상
 *
 * 각 묶음 사이에 1.2초를 쉬어 횟수를 셀 수 있게 한다.
 *
 * 지연은 k_busy_wait() 가 아니라 nop 스핀 루프다 — 시스템 타이머는
 * PRE_KERNEL_2 우선순위 0 에서야 올라오므로 PRE_KERNEL_1 에서는 쓸 수 없다.
 * 그래서 시간이 정확하지 않다. 횟수만 세면 된다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/device.h>

#define GPIO0_PHYS		0xfdd60000UL
#define GPIO0_SIZE		0x100
#define RK_GPIO_SWPORT_DR_L	0x00
#define RK_GPIO_SWPORT_DDR_L	0x08
#define LED_PIN			0U

/* 상위 16비트가 비트별 쓰기 허용(hiword mask)이다 */
static inline uint32_t rk_gpio_val(uint32_t pin, bool on)
{
	uint32_t we = BIT(pin + 16);

	return on ? (we | BIT(pin)) : we;
}

static mm_reg_t stage_gpio;
static bool stage_ready;

/* nop 스핀. 1 단위 ≈ 수십 µs 수준(정확하지 않다). */
static void stage_spin(uint32_t units)
{
	for (volatile uint32_t u = 0; u < units; u++) {
		for (volatile uint32_t i = 0; i < 2000U; i++) {
			__asm__ volatile("nop");
		}
	}
}

static void stage_led(bool on)
{
	if (!stage_ready) {
		return;
	}
	sys_write32(rk_gpio_val(LED_PIN, on), stage_gpio + RK_GPIO_SWPORT_DR_L);
}

static void stage_mark(uint32_t count)
{
	for (uint32_t n = 0; n < count; n++) {
		stage_led(true);
		stage_spin(60);
		stage_led(false);
		stage_spin(60);
	}
	stage_spin(600);	/* 묶음 사이 쉼 — 횟수를 셀 수 있게 */
}

/* ── 단계 1: 가장 먼저. GPIO 를 매핑하고 1회 깜박인다 ──────────────────── */
static int stage_pre_kernel_1(void)
{
	device_map(&stage_gpio, GPIO0_PHYS, GPIO0_SIZE, K_MEM_CACHE_NONE);
	sys_write32(rk_gpio_val(LED_PIN, true), stage_gpio + RK_GPIO_SWPORT_DDR_L);
	stage_ready = true;

	stage_mark(1);
	return 0;
}
SYS_INIT(stage_pre_kernel_1, PRE_KERNEL_1, 1);

/* ── 단계 2 ─────────────────────────────────────────────────────────────── */
static int stage_pre_kernel_2(void)
{
	stage_mark(2);
	return 0;
}
SYS_INIT(stage_pre_kernel_2, PRE_KERNEL_2, 99);

/* ── 단계 3: 셸 백엔드(POST_KERNEL 90)보다 뒤에 둔다 ────────────────────── */
static int stage_post_kernel(void)
{
	stage_mark(3);
	return 0;
}
SYS_INIT(stage_post_kernel, POST_KERNEL, 99);

/* ── 단계 4: USB 스택(APPLICATION 90)보다 뒤에 둔다 ─────────────────────── */
static int stage_application(void)
{
	stage_mark(4);
	return 0;
}
SYS_INIT(stage_application, APPLICATION, 99);
