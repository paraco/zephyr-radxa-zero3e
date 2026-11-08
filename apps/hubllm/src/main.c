/*
 * MoruLab Zephyr RTOS — Radxa ZERO 3E.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 셸·로그는 UART2. 부팅 메시지는 moru_boot.c. LED 는 soc_init/moru_led.c.
 * printk 는 쓰지 않는다 (동기 출력이 USB 열거를 깨뜨렸다).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#include "moru.h"

/* LED 는 보드 공용 모듈(soc_init/moru_led.c)이 상태별로 깜박인다.
 * 부팅 완료(READY, 2 초 점멸)는 그 모듈이 main 직전에 스스로 정한다. */

int main(void)
{
	moru_mark("[M]");
	LOG_INF("MoruLab Zephyr RTOS is up — 'help' 로 명령 목록");
	k_sleep(K_FOREVER);
	return 0;
}
