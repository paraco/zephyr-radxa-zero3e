/*
 * Radxa ZERO 3E — "Zephyr 가 실행되고 있는가" 를 USB·UART 없이 가리는 진단.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ── 왜 필요한가 ───────────────────────────────────────────────────────
 * 이 보드에서 쓸 수 있는 출력 채널이 둘뿐이다:
 *   - UART2 @ 1,500,000 baud — 손에 있는 CP2102 가 최대 1 Mbps 라 못 읽는다
 *   - USB OTG — 우리가 만들고 있는 dwc3 드라이버 자신이 담당한다
 * 그래서 dwc3 가 조용할 때 "Zephyr 가 안 뜬 것" 인지 "뜨지만 dwc3 가 열거를
 * 못 하는 것" 인지 구분할 방법이 없었다. LED 는 그 둘과 무관한 제3의 채널이다.
 *
 * ── GPIO 레지스터 배치의 출처 ─────────────────────────────────────────
 * Zephyr 에는 Rockchip GPIO 드라이버가 없다 (인트리 rk3568.dtsi 에 gic 와
 * uart2 뿐). 그래서 레지스터를 직접 쓴다. 배치는 FreeBSD 의
 * sys/arm64/rockchip/rk_gpio.c (SPDX: BSD-2-Clause, Copyright (c) 2018
 * Emmanuel Vadot, (c) 2021 Soren Schmidt) 에서 확인했다 — RK_GPIO_TYPE_V2:
 *
 *   SWPORTA_DR  = 0x00   핀 0~15, 핀 16~31 은 +4
 *   SWPORTA_DDR = 0x08   핀 0~15, 핀 16~31 은 +4
 *   VERSION     = 0x78   V2 == 0x01000c2b
 *
 * rk3568/rk3566 의 GPIO 는 "상위 16비트 write-enable + 하위 16비트 값" 형식
 * 이다. 즉 비트 n 을 1 로 만들려면 (1 << n) | (1 << (n + 16)) 을 쓰고,
 * 0 으로 만들려면 (1 << (n + 16)) 만 쓴다. read-modify-write 가 필요 없고
 * 다른 핀을 건드리지 않는다 — 이 진단에서 중요한 성질이다.
 *
 * ── 대상 핀 ───────────────────────────────────────────────────────────
 * 보드 LED = gpio0 @ 0xfdd60000, 핀 0, ACTIVE_HIGH.
 * 보드에서 돌던 Linux 의 라이브 디바이스트리를 ADB 로 읽어 확인했다:
 *   /gpio-leds/board-led/gpios = <0x37 0x00 0x00>
 *   phandle 0x37 == /pinctrl/gpio0@fdd60000
 *
 * ⚠ 깜박이지 않는다고 해서 "Zephyr 가 안 떴다" 로 단정할 수 없다. GPIO0 의
 *   클럭이 게이팅돼 있거나 핀 먹스가 GPIO 가 아닐 수도 있다 (U-Boot 가 어떻게
 *   두고 가는지 모른다). 즉 깜박이면 결정적이고, 안 깜박이면 미결이다.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ledblink, LOG_LEVEL_INF);

#define GPIO0_PHYS		0xfdd60000UL
#define GPIO0_SIZE		0x100

#define RK_GPIO_SWPORT_DR_L	0x00
#define RK_GPIO_SWPORT_DDR_L	0x08
#define RK_GPIO_VER_ID		0x78
#define RK_GPIO_VER_ID_V2	0x01000c2b

#define LED_PIN			0U

/* 상위 16비트가 write-enable. 해당 비트만 바뀌고 나머지는 보존된다. */
static inline uint32_t rk_gpio_val(uint32_t pin, bool on)
{
	uint32_t we = BIT(pin + 16);

	return on ? (we | BIT(pin)) : we;
}

int main(void)
{
	mm_reg_t base;
	uint32_t ver;

	LOG_INF("ledblink: GPIO0 @ 0x%lx 매핑", GPIO0_PHYS);
	device_map(&base, GPIO0_PHYS, GPIO0_SIZE, K_MEM_CACHE_NONE);

	ver = sys_read32(base + RK_GPIO_VER_ID);
	LOG_INF("GPIO VER_ID = 0x%08x (%s)", ver,
		(ver == RK_GPIO_VER_ID_V2) ? "v2 — 기대값" : "예상과 다름");

	/* 핀 0 을 출력으로 */
	sys_write32(rk_gpio_val(LED_PIN, true), base + RK_GPIO_SWPORT_DDR_L);

	LOG_INF("깜박임 시작 — 2초 켜짐 / 2초 꺼짐 (느린 네모파)");

	while (1) {
		/*
		 * ⚠ 패턴을 '느리게' 쓰는 것이 중요하다.
		 *
		 *   이 보드의 Linux 는 같은 LED 에 heartbeat 트리거를 걸어
		 *   둔다 (/sys/class/leds/board-led/trigger = [heartbeat]).
		 *   그건 '빠른 두 번 + 쉼' 이라, 빠른 패턴을 쓰면 눈으로
		 *   구분되지 않는다 — 실제로 한 번 오판했다.
		 *
		 *   2초 켜짐 / 2초 꺼짐의 느린 네모파는 Linux 의 어떤 트리거도
		 *   내지 않는 모양이라 헷갈릴 수 없다.
		 */
		sys_write32(rk_gpio_val(LED_PIN, true),
			    base + RK_GPIO_SWPORT_DR_L);
		k_msleep(2000);
		sys_write32(rk_gpio_val(LED_PIN, false),
			    base + RK_GPIO_SWPORT_DR_L);
		k_msleep(2000);
	}

	return 0;
}
