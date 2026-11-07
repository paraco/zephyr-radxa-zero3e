/* RK356x UART 는 DesignWare DW_APB_UART 다 — busy-detect 를 지우는 길.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RK3566_DW_UART_H_
#define RK3566_DW_UART_H_

/**
 * @brief DW_APB_UART 의 busy-detect 인터럽트를 지운다.
 *
 * `IIR[3:0] == 0x7`("busy detect")은 **USR(0x7C) 읽기로만** 지워진다.
 * `LSR`·`IIR` 읽기로는 안 지워지고 **`IER` 로 마스크할 수도 없다.**
 * 전송 중에 `LCR` 을 쓰면 걸린다.
 *
 * 이 모듈이 `POST_KERNEL` 에서 한 번 부르므로 보통은 직접 부를 일이 없다.
 * **보율·프레임을 바꾼 뒤에는(uart_configure 등) 반드시 다시 불러야 한다** —
 * 그때 busy-detect 가 새로 걸린다.
 *
 * 비용은 UART 하나당 레지스터 읽기 한 번이다. 지울 것이 없으면 무해하다.
 */
void rk3566_dw_uart_clear_busy(void);

#endif /* RK3566_DW_UART_H_ */
