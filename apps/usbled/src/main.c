/*
 * Radxa ZERO 3E — USB CDC-ACM 콘솔 + LED 생존 신호.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 콘솔이 USB 로 나가는데(chosen zephyr,console = &cdc_acm_uart0) 그 USB 를
 * 우리가 만들고 있는 dwc3 드라이버가 담당한다. 즉 드라이버가 실패하면
 * 실패 원인을 알려 줄 로그 자체가 나오지 않는다.
 *
 * 그래서 LED 를 함께 깜박인다 — USB 와 무관한 채널이라 다음을 구분해 준다:
 *   LED 깜박임 + USB 포트 등장  -> 전부 성공
 *   LED 깜박임 + USB 조용       -> Zephyr 는 돌지만 dwc3/PHY 가 실패
 *   LED 도 조용                 -> Zephyr 가 기동하지 못함
 *
 * LED 는 gpio0 @ 0xfdd60000 핀 0. 레지스터 배치 출처와 주의사항은
 * apps/ledblink/src/main.c 의 머리말에 자세히 적어 두었다.
 *
 * ⚠ 패턴은 2초 켜짐 / 2초 꺼짐의 느린 네모파다. 이 보드의 Linux 가 같은
 *   LED 에 heartbeat 트리거(빠른 두 번 + 쉼)를 걸어 두기 때문에, 빠른
 *   패턴을 쓰면 눈으로 구분되지 않는다 — 실제로 한 번 오판했다.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usbled, LOG_LEVEL_INF);

#define GPIO0_PHYS		0xfdd60000UL
#define GPIO0_SIZE		0x100
#define RK_GPIO_SWPORT_DR_L	0x00
#define RK_GPIO_SWPORT_DDR_L	0x08
#define LED_PIN			0U

/* 상위 16비트가 write-enable — 해당 비트만 바뀐다 */
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

/*
 * ⚠ 스택 4096 — 1024 로 두었다가 실기에서 크래시했다.
 *
 *   arm64 는 64비트 레지스터 31개라 문맥 저장만 256바이트를 넘고,
 *   device_map() 은 MMU 페이지테이블 작업을 한다. 1024 로는 넘친다.
 *   증상: CPU 예외, ELR=FAR=lr=0x0 (복귀 주소가 0 으로 덮임),
 *   x7~x18 전부 쓰레기, Current thread: led_tid.
 *
 *   ⚠ 진단용으로 넣은 이 스레드 자체가 크래시 원인이었다. 드라이버
 *     스택(CONFIG_UDC_DWC3_STACK_SIZE)을 늘려도 변화가 없던 이유다.
 */
K_THREAD_DEFINE(led_tid, 4096, led_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	uint32_t n = 0;

	/*
	 * 콘솔이 USB 로 나가므로, 호스트가 포트를 열기 전의 출력은 버려진다.
	 * 그래서 주기적으로 계속 찍는다 — 나중에 붙어도 살아 있음을 알 수
	 * 있게.
	 */
	while (1) {
		LOG_INF("usbled alive %u", n++);
		k_msleep(1000);
	}

	return 0;
}
