/*
 * 동기 마커 — 어디까지 가다 멈추는지 콘솔로 직접 찍는다.
 *
 * 왜 이 방식인가.
 *  - printk 는 CONFIG_LOG_PRINTK 경로로 지연 로깅에 실려 갈 수 있어
 *    멈춤 직전의 출력이 버퍼에 남고 안 나온다.
 *  - LOG_INF 도 같은 이유로 못 쓴다 (백엔드가 뜨기 전엔 버퍼에만 쌓인다).
 *  - device_map 은 쓰지 않는다 — PRE_KERNEL_1 에서 그걸 불렀다가 보드를
 *    죽였다 (z_mem_manage_init 이 그보다 뒤에 돈다, kernel/init.c:293).
 *
 * uart_poll_out 은 LSR_THRE 를 폴링하고 스핀락만 쓴다
 * (uart_ns16550.c:979-996). uart2 는 PRE_KERNEL_1 우선순위 50 에서
 * 초기화되므로 PRE_KERNEL_2 이후라면 안전하다. 이 보드에서 지금까지
 * 모든 콘솔 출력이 이 경로로 나갔다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

void moru_mark(const char *s)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(con)) {
		return;
	}
	while (*s != '\0') {
		uart_poll_out(con, (unsigned char)*s++);
	}
}

static int m_pk2(void)      { moru_mark("[2]");  return 0; }
static int m_pre_shell(void){ moru_mark("[<S]"); return 0; }
static int m_post_shell(void){ moru_mark("[S>]"); return 0; }
static int m_pre_usb(void)  { moru_mark("[<U]"); return 0; }
static int m_post_usb(void) { moru_mark("[U>]"); return 0; }

SYS_INIT(m_pk2,        PRE_KERNEL_2, 99);
SYS_INIT(m_pre_shell,  POST_KERNEL,  89);	/* enable_shell_uart 는 90 */
SYS_INIT(m_post_shell, POST_KERNEL,  91);
SYS_INIT(m_pre_usb,    APPLICATION,  89);	/* cdc_acm_serial_init_device 는 90 */
SYS_INIT(m_post_usb,   APPLICATION,  91);
