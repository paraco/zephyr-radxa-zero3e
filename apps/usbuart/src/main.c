/*
 * Radxa ZERO 3E — dwc3 실기 진단.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 콘솔은 UART2 @ 115200 (보드 DTS 의 기본값), USB 스택은 켜져 있다.
 * 목적은 dwc3 드라이버가 초기화하면서 찍는 값들을 사람이 읽는 것이다 —
 * GSNPSID, GHWPARAMS3, 코어 리셋 결과, PRTCAPDIR 반영 여부, DEPCMD 상태,
 * 그리고 버스 이벤트(USB_RESET)가 오는지.
 *
 * LED 도 함께 깜박인다. 시리얼이 조용하거나 글자가 깨져도 Zephyr 가
 * 살아 있는지는 LED 로 알 수 있다 — 보율 문제와 기동 실패를 구분해 준다.
 *
 * 배선: 어댑터 RXD -> 보드 8번 · TXD -> 10번 · GND -> 6번
 *       (핀 이름은 보드 기준이라 어댑터와 교차한다)
 * 읽기: tio /dev/cu.usbserial-0001 -b 115200
 *       글자가 깨지면 -b 125000 으로 재시도 (UART 클럭이 12 MHz 인 경우)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bringup, LOG_LEVEL_INF);

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

	printk("\n");
	printk("=====================================\n");
	printk(" PiZZa — Radxa Zero 3E dwc3 bringup\n");
	printk(" console: uart2 @ 115200\n");
	printk("=====================================\n");

	while (1) {
		LOG_INF("alive %u", n++);
		k_msleep(5000);
	}

	return 0;
}
