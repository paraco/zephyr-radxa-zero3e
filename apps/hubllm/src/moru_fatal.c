/*
 * 치명 오류를 uart2 에 **동기로** 찍는다.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 왜 필요한가
 * -----------
 * Zephyr 의 기본 fatal 경로는 LOG_ERR 로 찍는다. 이 앱은
 * `CONFIG_LOG_MODE_DEFERRED` + 셸 로그 백엔드(USB CDC)라서, **백엔드가 붙기
 * 전에 죽으면 그 메시지가 버퍼에 남고 아무것도 안 보인다.** 실제로
 * `CONFIG_THREAD_RUNTIME_STATS` 를 켠 판이 PRE_KERNEL_2 직후에 조용히
 * 죽었고, 마커 `[2]` 외에는 단 한 글자도 얻지 못했다 — 죽었는지 멈췄는지도
 * 구분할 수 없었다.
 *
 * 이 핸들러는 moru_mark 와 같은 길로 간다: uart2 에 `uart_poll_out` 으로
 * 직접, 동기로 밀어낸다. 로그·콘솔·USB·스케줄러가 전부 죽어도 나온다.
 *
 * ⚠ 이 안에서는 printf 계열과 LOG 를 쓰지 않는다. 16진수는 손으로 만든다 —
 *   fault 문맥에서 서식 엔진을 돌리는 것은 진단을 피검체로 만드는 일이다
 *   (규칙 5: 진단 코드는 피검체보다 단순해야 한다).
 *
 * ⚠ ESR/FAR 은 esf 에 없다. 시스템 레지스터에서 직접 읽는다. EL1 에서
 *   돌므로 ESR_EL1·FAR_EL1·ELR_EL1 이 유효하다.
 *
 * 읽는 법 (ESR_EL1):
 *   EC = ESR[31:26]  0x24/0x25 데이터 어보트 · 0x20/0x21 명령 어보트
 *                    0x22 PC 정렬 · 0x26 SP 정렬 · 0x2f SError
 *   FAR = 접근하려던 주소 (어보트일 때)
 *   ELR = 죽은 명령의 주소 → addr2line 으로 소스를 찾는다
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/arch/arm64/lib_helpers.h>

#include "moru.h"
#include <moru_led.h>

static void f_puts(const struct device *d, const char *s)
{
	while (*s != '\0') {
		if (*s == '\n') {
			uart_poll_out(d, '\r');
		}
		uart_poll_out(d, *s++);
	}
}

/* 64비트를 고정 16자리 16진수로. 서식 엔진을 쓰지 않는다. */
static void f_hex64(const struct device *d, uint64_t v)
{
	static const char hx[] = "0123456789abcdef";

	f_puts(d, "0x");
	for (int i = 60; i >= 0; i -= 4) {
		uart_poll_out(d, hx[(v >> i) & 0xfU]);
	}
}

static void f_line(const struct device *d, const char *name, uint64_t v)
{
	f_puts(d, "  ");
	f_puts(d, name);
	f_puts(d, " = ");
	f_hex64(d, v);
	f_puts(d, "\n");
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	/* ⚠ chosen 이 아니라 uart2 고정 — moru_mark.c 머리말과 같은 이유다 */
	const struct device *d = DEVICE_DT_GET(DT_NODELABEL(uart2));
	uint64_t esr = read_sysreg(esr_el1);
	uint64_t ec = (esr >> 26) & 0x3fULL;

	if (!device_is_ready(d)) {
		/* uart2 조차 아직이면 할 수 있는 것이 없다 */
		ARG_UNUSED(reason);
		moru_led_fatal();	/* LED 0.2 초 점멸로 멈춘다 */
	}

	f_puts(d, "\n=== MORU FATAL ===\n");
	f_line(d, "reason  ", (uint64_t)reason);
	f_line(d, "ESR_EL1 ", esr);
	f_line(d, "  EC    ", ec);
	f_line(d, "FAR_EL1 ", read_sysreg(far_el1));
	f_line(d, "ELR_EL1 ", read_sysreg(elr_el1));
	f_line(d, "CurrentEL", read_currentel());

	if (esf != NULL) {
		f_line(d, "esf.elr ", esf->elr);
		f_line(d, "esf.spsr", esf->spsr);
		f_line(d, "esf.lr  ", esf->lr);
		f_line(d, "esf.x0  ", esf->x0);
		f_line(d, "esf.x1  ", esf->x1);
	} else {
		f_puts(d, "  esf = NULL (어보트가 아니라 커널이 부른 것)\n");
	}

	/*
	 * EC 해독을 한 줄 붙인다 — 숫자만 보면 매번 TRM·ARM ARM 을 뒤진다.
	 * 출처: ARM ARM D17.2.37 ESR_EL1.
	 */
	f_puts(d, "  EC 뜻: ");
	switch (ec) {
	case 0x20: f_puts(d, "명령 어보트 (낮은 EL)\n"); break;
	case 0x21: f_puts(d, "명령 어보트 (같은 EL)\n"); break;
	case 0x22: f_puts(d, "PC 정렬 오류\n"); break;
	case 0x24: f_puts(d, "데이터 어보트 (낮은 EL)\n"); break;
	case 0x25: f_puts(d, "데이터 어보트 (같은 EL) — FAR 을 본다\n"); break;
	case 0x26: f_puts(d, "SP 정렬 오류\n"); break;
	case 0x2f: f_puts(d, "SError\n"); break;
	case 0x00: f_puts(d, "알 수 없음 (어보트가 아닐 수 있다)\n"); break;
	default:   f_puts(d, "위 EC 값을 ARM ARM 에서 찾는다\n"); break;
	}
	f_puts(d, "=== 멈춘다 ===\n");

	moru_led_fatal();	/* LED 0.2 초 점멸로 멈춘다 (돌아오지 않는다) */
}

/*
 * ⚠ 일부러 데이터 어보트를 낸다 — **진단기를 검증하기 위한 것**이다.
 *
 *   이 핸들러를 믿고 "조용히 죽었다 = fault 가 아니다" 라고 판정하려면,
 *   핸들러가 실제로 말을 하는지 먼저 증명해야 한다. 그러지 않으면 다음에
 *   또 침묵했을 때 "핸들러 고장" 과 "fault 아님" 을 구분할 수 없다 —
 *   오늘 uart2 점퍼로 똑같은 함정에 빠졌다(규칙 6, 체크리스트 9번).
 *
 *   되돌리기: 전원을 다시 넣으면 된다. 카드의 펌웨어는 그대로다.
 */
#include <zephyr/shell/shell.h>

static int cmd_moru_fault(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_warn(sh, "일부러 데이터 어보트를 낸다 — uart2 를 보라.");
	shell_warn(sh, "보드는 멈춘다. 전원을 다시 넣으면 살아난다.");
	/* 셸 출력이 실제로 나갈 시간을 준다 */
	k_msleep(300);

	/*
	 * 매핑되지 않은 주소를 읽는다. volatile 로 최적화 제거를 막는다.
	 * 0x00000000 은 이 보드에서 매핑돼 있지 않다 (SRAM 은 0x40000000).
	 */
	{
		volatile uint64_t *bad = (volatile uint64_t *)0x10ULL;

		(void)*bad;
	}

	shell_error(sh, "어보트가 나지 않았다 — 이 주소가 매핑돼 있다");
	return -EIO;
}

SHELL_CMD_REGISTER(moru_fault, NULL,
		   "⚠ 일부러 fault 를 내 moru_fatal 진단기를 검증한다",
		   cmd_moru_fault);
