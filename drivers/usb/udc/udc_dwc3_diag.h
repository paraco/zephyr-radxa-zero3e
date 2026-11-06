/*
 * dwc3 진단 스냅샷 — 셸이 상태와 카운터를 한 번에 읽는다.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ── 왜 스냅샷 하나인가 ───────────────────────────────────────────────
 * 개별 getter 를 여러 개 열면 읽는 사이에 링크 상태가 바뀌어 **찢긴 상태**
 * 를 보게 된다. 한 번의 호출로 일관된 묶음을 준다.
 *
 * ── 왜 드라이버에 셸 명령을 넣지 않는가 ──────────────────────────────
 * 드라이버가 CONFIG_SHELL 에 의존하면 안 되고, 3000줄 드라이버에 UI 코드가
 * 늘어나는 것도 피한다. 출력 형식은 앱(apps/moru/src/moru_shell.c)이 갖는다.
 *
 * ⚠ 카운터는 **동작을 바꾸지 않는다.** 이벤트 처리 지점에서 ++ 만 한다.
 *   드라이버가 깨지면 USB 셸을 잃으므로(= OTA 도 못 쓴다) 변경을 읽기와
 *   증가로만 제한했다.
 */

#ifndef MORU_UDC_DWC3_DIAG_H_
#define MORU_UDC_DWC3_DIAG_H_

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>

/**
 * 이벤트 카운터.
 *
 * `cmd_timeout` 과 `trb_hw_owned` 는 **bring-up 에서 쫓던 두 증상**이다.
 * 로그를 세는 대신 이 값을 본다 — 과거에 그 둘을 로그에서 손으로 셌다.
 */
struct udc_dwc3_stats {
	uint32_t reset;		/* USB_RESET */
	uint32_t connect_done;	/* CONNECTION_DONE */
	uint32_t disconnect;	/* DISCONNECT */
	uint32_t setup;		/* SETUP 패킷 수신 */
	uint32_t xfer_complete;	/* XferComplete (EP0 + 일반) */
	uint32_t xfer_not_ready;/* XferNotReady */
	uint32_t cmd_timeout;	/* DEPCMD 타임아웃 */
	uint32_t trb_hw_owned;	/* 완료 처리 시 TRB 가 아직 HW 소유 */
	uint32_t no_queued_buf;	/* XferComplete 인데 큐에 버퍼가 없음 */
	/*
	 * 전원 관리 — 1단계는 **세기만** 한다 (2026-11-06).
	 * 호스트가 실제로 suspend 를 보내는지 먼저 알아야 스택에 올릴
	 * 가치가 있다. 올리는 것은 2단계다.
	 */
	uint32_t suspend;	/* U3/L2/L1 진입 (DWC3_DEVT_SUSPEND_ENTRY) */
	uint32_t wakeup;	/* REMOTE_WAKEUP */
	uint32_t link_state;	/* LINK_STATE_CHANGE — 빈번할 수 있어 로그 없음 */
	uint8_t  last_linkst;	/* 마지막으로 본 DSTS.USBLNKST */
};

/** 한 번에 읽는 상태 묶음. */
struct udc_dwc3_diag {
	/* 식별 */
	uint32_t gsnpsid;
	uint32_t ghwparams3;
	uint8_t  hw_num_eps;
	/* 모드·링크 */
	uint32_t gctl;
	uint32_t dctl;
	uint32_t dcfg;
	uint32_t dsts;
	uint32_t dalepena;
	uint32_t gusb2phycfg0;
	/* EP0 상태 기계 */
	uint8_t  ep0_state;
	uint16_t ep0_wlength;
	/*
	 * 벌크 EP 의 mps — **HS 적용 여부를 판정하는 값**이다.
	 * HS 는 512, FS 는 64. 로그로 확인하려 했지만 터미널 출력이 매번
	 * 중간에 잘려 보이지 않았다(2026-11-06). 그래서 여기에 넣는다.
	 * 0 이면 그 EP 가 아직 켜지지 않았다.
	 */
	uint16_t bulk_in_mps;	/* 0x82 — 첫 CDC 의 bulk IN */
	uint16_t bulk_out_mps;	/* 0x01 — 첫 CDC 의 bulk OUT */
	uint16_t bulk_in1_mps;	/* 0x84 — 둘째 CDC (OTA 데이터) */
	uint16_t bulk_out1_mps;	/* 0x02 */
	/* 카운터 */
	struct udc_dwc3_stats stats;
};

/**
 * 스냅샷을 읽는다.
 *
 * @retval 0 성공
 * @retval -ENODEV dev 가 준비되지 않았거나 레지스터가 매핑되지 않았다
 * @retval -EINVAL out 이 NULL
 */
int udc_dwc3_diag_get(const struct device *dev, struct udc_dwc3_diag *out);

#endif /* MORU_UDC_DWC3_DIAG_H_ */
