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
 * ── ⚠ chosen 이 아니라 uart2 에 직접 찍는다 (2026-11-06) ──────────────
 * main 이 USB CDC-ACM 으로 바뀌었다 (app.overlay). chosen zephyr,console
 * 을 따르면 이 마커가 CDC 로 가는데, USB 열거 전에는 한 글자도 나가지
 * 않아 **초기 관측 수단이 사라진다** — 그게 바로 이 파일의 존재 이유다.
 *
 * 그래서 노드를 uart2 로 고정한다. 보율은 걱정하지 않아도 된다:
 * ns16550 드라이버가 DT 의 current-speed(115200)를 스스로 적용한다
 * (uart_ns16550.c:2079 .uart_config.baudrate = DT_INST_PROP_OR(
 *  current_speed) → init → pm_action(TURN_ON) → configure →
 *  set_baud_rate, :654). 즉 **콘솔로 쓰이지 않아도 115200 으로 나온다.**
 *
 * 따라서 USB 가 무음일 때도 UART 어댑터만 꽂으면 어디까지 갔는지 보인다.
 * 진단 빌드(diag-uart.overlay)로 카드를 바꾸기 전에 먼저 이것을 본다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

void moru_mark(const char *s)
{
	/* chosen 이 아니라 uart2 고정 — 위 머리말 참고 */
	const struct device *con = DEVICE_DT_GET(DT_NODELABEL(uart2));

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
