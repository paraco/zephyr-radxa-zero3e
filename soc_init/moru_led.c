/*
 * moru_led — 상태별 LED 패턴. 표는 include/moru_led.h 머리말에 있다.
 *
 * GPIO0_A0, 1 = 켜짐 (apps/moru 의 이전 하트비트와 같은 핀·극성).
 * 레지스터: GPIO0 0xfdd60000 · SWPORT_DR_L 0x00 · SWPORT_DDR_L 0x08,
 * 위 16 비트가 쓰기 허가(rockchip "hiword" 형식).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <moru_led.h>

#define GPIO0_PHYS   0xfdd60000UL
#define GPIO_DR_L    0x00
#define GPIO_DDR_L   0x08
#define LED_PIN      0U

struct step { uint16_t on_ms, off_ms; };

static const struct step p_ready[] = { {2000, 2000} };
static const struct step p_blob[]  = { {500, 500}, {500, 500}, {1000, 1000} };
static const struct step p_infer[] = { {100, 900} };
static const struct step p_ota[]   = { {200, 200}, {200, 200}, {200, 200}, {200, 200},
				       {200, 200}, {2000, 2000} };
static const struct step p_fail[]  = { {200, 200}, {200, 200}, {200, 1200} };
static const struct step p_crash[] = { {200, 200} };

struct pattern { const struct step *s; uint8_t n; };
#define PAT(a) { a, ARRAY_SIZE(a) }
static const struct pattern pats[] = {
	[MORU_LED_BOOT]  = { NULL, 0 },		/* 계속 켜짐 */
	[MORU_LED_READY] = PAT(p_ready),
	[MORU_LED_BLOB]  = PAT(p_blob),
	[MORU_LED_INFER] = PAT(p_infer),
	[MORU_LED_OTA]   = PAT(p_ota),
	[MORU_LED_FAIL]  = PAT(p_fail),
	[MORU_LED_CRASH] = PAT(p_crash),
};

static mm_reg_t gpio;
static struct k_timer led_timer;
static struct k_spinlock led_lock;
static volatile enum moru_led_state cur = MORU_LED_BOOT;
static uint8_t idx;
static bool phase_on;
static volatile bool fatal;

static inline void led(bool on)
{
	sys_write32(BIT(LED_PIN + 16) | (on ? BIT(LED_PIN) : 0), gpio + GPIO_DR_L);
}

/* 타이머 ISR: 지금 단계의 켜짐 → 꺼짐 → 다음 단계 */
static void led_tick(struct k_timer *t)
{
	k_spinlock_key_t k = k_spin_lock(&led_lock);
	const struct pattern *p = &pats[cur];

	if (fatal || p->n == 0) {
		k_spin_unlock(&led_lock, k);
		return;
	}
	if (phase_on) {
		phase_on = false;
		led(false);
		k_timer_start(t, K_MSEC(p->s[idx].off_ms), K_NO_WAIT);
	} else {
		idx = (idx + 1) % p->n;
		phase_on = true;
		led(true);
		k_timer_start(t, K_MSEC(p->s[idx].on_ms), K_NO_WAIT);
	}
	k_spin_unlock(&led_lock, k);
}

void moru_led_set(enum moru_led_state s)
{
	if (gpio == 0 || fatal || (unsigned)s >= ARRAY_SIZE(pats)) {
		return;
	}
	k_spinlock_key_t k = k_spin_lock(&led_lock);

	if (s != cur) {
		cur = s;
		idx = 0;
		phase_on = true;
		led(true);
		if (pats[s].n) {
			k_timer_start(&led_timer, K_MSEC(pats[s].s[0].on_ms), K_NO_WAIT);
		} else {
			k_timer_stop(&led_timer);
		}
	}
	k_spin_unlock(&led_lock, k);
}

enum moru_led_state moru_led_get(void)
{
	return cur;
}

FUNC_NORETURN void moru_led_fatal(void)
{
	(void)arch_irq_lock();
	fatal = true;
	cur = MORU_LED_CRASH;
	if (gpio != 0) {
		for (;;) {
			led(true);
			k_busy_wait(200 * 1000);
			led(false);
			k_busy_wait(200 * 1000);
		}
	}
	for (;;) {
		__asm__ volatile("wfi");
	}
}

#ifdef CONFIG_MORU_LED_FATAL_HANDLER
/* 앱이 자기 핸들러를 두지 않을 때. 한 줄만 찍고(printk 는 동기다) 점멸로 간다. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	printk("\n=== FATAL reason %u — LED 0.2 초 점멸로 멈춘다 ===\n", reason);
	moru_led_fatal();
}
#endif

/* 가장 이른 시점: PRE_KERNEL_1 의 맨 앞에서 LED 를 켠다 (U-Boot 가 켜 둔 것을 이어 받는다) */
static int moru_led_early(void)
{
	k_timer_init(&led_timer, led_tick, NULL);	/* 구조체 초기화뿐 — 시작은 READY 에서 */
	device_map(&gpio, GPIO0_PHYS, 0x100, K_MEM_CACHE_NONE);
	sys_write32(BIT(LED_PIN + 16) | BIT(LED_PIN), gpio + GPIO_DDR_L);
	led(true);
	return 0;
}
SYS_INIT(moru_led_early, PRE_KERNEL_1, 0);

/* 부팅 완료: APPLICATION 의 맨 끝 = main 직전. 앱이 이어서 상태를 바꾼다 */
static int moru_led_ready(void)
{
	moru_led_set(MORU_LED_READY);
	return 0;
}
SYS_INIT(moru_led_ready, APPLICATION, 99);
