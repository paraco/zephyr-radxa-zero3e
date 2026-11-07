/* RK356x 의 DesignWare busy-detect 인터럽트 폭주를 막는다.
 *
 * ── 왜 필요한가 (2026-11-07, L8i·L8j 로 실기 확정) ──────────────────────
 *
 * RK3566 의 UART 는 표준 16550 이 아니라 **Synopsys DW_APB_UART** 다.
 * 그런데 `dts/arm64/rockchip/rk3568.dtsi` 가
 *     compatible = "rockchip,rk3568-uart", "ns16550"
 * 로 선언해 Zephyr 의 **범용** `uart_ns16550.c` 에 바인딩된다. 그 드라이버는
 * DW 전용 인터럽트를 모른다:
 *
 *     IIR[3:0] == 0x7   "busy detect"
 *
 * 이것은 **USR(오프셋 0x7C) 읽기로만 지워진다.** `LSR`·`IIR` 읽기로는 안
 * 지워지고, **`IER` 로 마스크할 수도 없다.** 전송 중에 `LCR` 을 쓰면 걸리는데,
 * U-Boot 가 1.5 Mbps 로 쓰던 UART 를 Zephyr 가 115200 으로 다시 설정하는
 * 바로 그 순간이다.
 *
 * 결과는 **영구 인터럽트 폭주**다. 실기 측정(L8j, PMU 이벤트 계수):
 *
 *     기준선            21.96 사이클/바퀴 · IPC 0.45 · EXC_TAKEN 21,971
 *                       IER 0x00 (전부 마스크) · IIR 0xc7 (busy detect)
 *     IIR 읽은 뒤       21.96 · EXC 21,970   ← 안 지워진다
 *     **USR 읽은 뒤**    5.00 · IPC 1.99 · EXC 0 · IIR 0xc1
 *     irq_disable        5.00 (양성 대조)
 *     irq_enable         5.00 ← 다시 켜도 안 돌아온다
 *
 * 예외당 약 386 사이클, **사이클의 약 77%** 다. 출력은 멀쩡하므로 증상이
 * 오류가 아니라 **조용한 4~17배 감속**이다 — 가장 찾기 어려운 종류다.
 * 사다리 앱(llama.cpp)은 토큰당 23.6초 → 1.423초(16.6배)가 됐다.
 *
 * Linux 는 전용 드라이버로 처리한다 —
 * `drivers/tty/serial/8250/8250_dw.c` 의 `dw8250_handle_irq()` 가
 * `(iir & 0x3f) == UART_IIR_BUSY` 일 때 USR 을 읽는다.
 *
 * ── 왜 init 한 번으로 되는가 (그리고 그 한계) ──────────────────────────
 *
 * busy-detect 는 **LCR 쓰기마다 새로 걸린다.** Zephyr 는 init 에서 한 번만
 * LCR 을 쓰므로, 그 뒤 USR 을 한 번 읽으면 다시 걸릴 일이 없다. L8j 의
 * `[6]` 에서 `irq_enable` 뒤에도 5.00 을 유지하는 것이 그 증거다.
 *
 * ⚠ **보율을 바꾸면 다시 걸린다.** `uart_configure()` 를 부르는 경로가
 *   생기면 그 뒤 `rk3566_dw_uart_clear_busy()` 를 다시 불러야 한다.
 *   올바른 고침은 ISR 안에서 처리하는 것이지만, 그러려면 업스트림
 *   `uart_ns16550.c` 를 고쳐야 한다. 이 저장소는 업스트림 편집 0줄을
 *   유지하므로 여기서는 init 쪽을 잡고 한계를 적어 둔다.
 *   업스트림 보고는 `docs/upstream-report.md` 의 (5) 다.
 *
 * ── 왜 POST_KERNEL priority 0 인가 ─────────────────────────────────────
 *
 *   · UART 드라이버보다 **뒤**여야 한다 — LCR 을 쓴 뒤에 지워야 하니까.
 *     (ns16550 은 PRE_KERNEL_1 이다)
 *   · dwc3/USB 보다 **앞**이어야 한다. udc_dwc3 는
 *     CONFIG_KERNEL_INIT_PRIORITY_DEVICE(기본 50)에서 초기화되고, USB 열거는
 *     SET_ADDRESS 에 50 ms 예산이 있다(docs/hardware-verified.md). 폭주가
 *     사이클의 77% 를 먹는 상태로 열거하면 그 예산을 넘길 수 있다 —
 *     이 세션에서 CDC 가 간헐적으로 안 붙은 것과도 관계있을 수 있다
 *     (⚠ 확인되지 않았다. 추측이다).
 *   · device_map() 을 쓰므로 커널 메모리 하위 시스템이 올라온 뒤여야 한다.
 *     PRE_KERNEL_2 에서 생주소를 직접 읽는 쪽이 더 이르지만, 그 주소가
 *     그 시점에 매핑됐는지 확인되지 않았다 — 틀리면 콘솔 전에 조용히 죽는다.
 *     그 위험을 지지 않는다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>

#include "rk3566_dw_uart.h"

/* DW_APB_UART: UART Status Register. reg-shift 와 무관한 **바이트 오프셋**이다
 * (DTS 의 reg-shift=2 는 16550 레지스터 묶음에만 적용된다). */
#define DW_UART_USR_OFFSET 0x7c

/* 이 보드에서 status = "okay" 인 rockchip UART 전부를 돈다. 지금은 uart2
 * 하나지만, 포트를 더 켤 때 이 파일을 고칠 일이 없게 해 둔다. */
#define DW_UART_ENTRY(node)                                                    \
	{ .phys = DT_REG_ADDR(node), .size = DT_REG_SIZE(node) },

static const struct dw_uart {
	uintptr_t phys;
	size_t    size;
} dw_uarts[] = {
	DT_FOREACH_STATUS_OKAY(rockchip_rk3568_uart, DW_UART_ENTRY)
};

void rk3566_dw_uart_clear_busy(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dw_uarts); i++) {
		mm_reg_t base;

		device_map(&base, dw_uarts[i].phys, dw_uarts[i].size,
			   K_MEM_CACHE_NONE);
		/* 읽기만 한다. 지울 것이 없으면 무해하다. */
		(void)sys_read32(base + DW_UART_USR_OFFSET);
	}
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
/* ⚠ CONFIG_UART_INTERRUPT_DRIVEN=n 이면 GIC 에서 IRQ 가 켜지지 않아 폭주가
 *   없다. 그때는 UART 를 건드릴 이유가 없으므로 등록하지 않는다
 *   (사다리 앱이 그 경우다 — apps/llm_common/ladder.conf). */
static int rk3566_dw_uart_init(void)
{
	rk3566_dw_uart_clear_busy();
	return 0;
}

SYS_INIT(rk3566_dw_uart_init, POST_KERNEL, 0);
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
