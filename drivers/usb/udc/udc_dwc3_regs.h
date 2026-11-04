/*
 * Copyright (c) 2017 The Fuchsia Authors
 * Copyright (c) 2026 paraco
 *
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 */

/*
 * Synopsys DWC3 레지스터 정의.
 *
 * ── 출처와 라이선스 (중요) ──────────────────────────────────────────────
 * 이 파일의 레지스터 오프셋과 비트 위치는 Fuchsia 의 DWC3 레지스터 헤더에서
 * 가져온 파생물이다. 따라서 이 파일 하나만 듀얼 라이선스다.
 *
 *   출처 : https://fuchsia.googlesource.com/fuchsia/+/refs/heads/main/
 *          src/devices/usb/drivers/dwc3/dwc3-regs.h      (레지스터·비트)
 *          src/devices/usb/drivers/dwc3/dwc3-types.h     (TRB·이벤트 구조)
 *          src/devices/usb/drivers/dwc3/dwc3-commands.cc (명령 인코딩)
 *   조건 : BSD-2-Clause — 같은 디렉터리의 LICENSE.fuchsia 에 전문을 동봉했다.
 *          저작권 고지 유지가 재배포 조건이다.
 *
 * ── 무엇을 가져왔고 무엇을 가져오지 않았나 ──────────────────────────────
 * 가져온 것: 레지스터 오프셋 · 비트 위치 · 필드 값 인코딩. 즉 하드웨어가
 *   어떤 숫자를 어떤 자리에서 기대하는가 하는 '사실' 이다.
 * 가져오지 않은 것: 드라이버 구조 · 상태 기계 · 전송 큐 · 오류 처리.
 *   udc_dwc3.c 의 코드는 이 프로젝트에서 직접 썼다.
 *
 * ⚠ 이전 판 머리말은 "Fuchsia 의 드라이버 소스(.cc)는 열지 않았다" 고
 *   적었으나, EP0 설정을 구현하면서 그 제약을 완화했다. 레지스터 헤더에
 *   없는 두 값 — EP_TYPE 의 인코딩과 DEPSTRTXFER 의 파라미터 배치 —
 *   이 dwc3-commands.cc 에만 있었고, 그 둘을 추측하면 조용히 실패하는
 *   자리였기 때문이다. BSD 계열 라이선스는 출처 표기로 포함이 가능하므로
 *   정책 문제이지 라이선스 위반은 아니지만, 사실과 다른 서술을 남겨 둘
 *   수는 없어 바로잡았다.
 *
 * ⚠ udc_dwc3.c 는 이 헤더를 include 할 뿐이고 자체 SPDX 는 Apache-2.0
 *   단독이다. 다만 그 파일의 명령 발행 '순서' 는 위 BSD-2 출처와 대조해
 *   확인한 것이다 (순서 자체는 databook 이 규정하는 하드웨어 요구사항).
 *   외부 공개·배포 전에 라이선스 표기를 한 번 더 확인할 것.
 *
 * ⚠ Linux 의 drivers/usb/dwc3 (GPL-2.0) 와 바인딩 문서는 참조하지 않았다.
 *   디바이스트리는 GPL-2.0+ OR MIT 라 MIT 선택지로 썼다.
 *
 * ── 검증 이력 ───────────────────────────────────────────────────────────
 * Fuchsia 의 2017년 zircon 스냅샷에는 GUSB2PHYCFG 가 0xc118 로 적혀 있어
 * GSTS(0xc118)와 충돌했다. 현재 main 브랜치에서 0xc200 으로 수정된 것을
 * 확인하고 그 값을 썼다. 2017년판은 쓰지 않는다.
 */

#ifndef ZEPHYR_DRIVERS_USB_UDC_DWC3_REGS_H
#define ZEPHYR_DRIVERS_USB_UDC_DWC3_REGS_H

#include <zephyr/sys/util.h>

/* ── Global 레지스터 ──────────────────────────────────────────────────── */
#define DWC3_GSBUSCFG0			0xc100
#define DWC3_GSBUSCFG1			0xc104
#define DWC3_GCTL			0xc110
#define DWC3_GSTS			0xc118
#define DWC3_GUCTL1			0xc11c
#define DWC3_GSNPSID			0xc120
#define DWC3_GHWPARAMS0			0xc140
#define DWC3_GHWPARAMS3			0xc14c
#define DWC3_GUCTL2			0xc19c
#define DWC3_GUSB2PHYCFG(n)		(0xc200 + (n) * 0x4)
#define DWC3_GUSB3PIPECTL(n)		(0xc2c0 + (n) * 0x4)
#define DWC3_GTXFIFOSIZ(n)		(0xc300 + (n) * 0x4)
#define DWC3_GRXFIFOSIZ(n)		(0xc380 + (n) * 0x4)
/* GEVNTADR 는 64비트 레지스터다. 32비트씩 나눠 쓴다. */
#define DWC3_GEVNTADR(n)		(0xc400 + (n) * 0x10)
#define DWC3_GEVNTADR_LO(n)		(0xc400 + (n) * 0x10)
#define DWC3_GEVNTADR_HI(n)		(0xc404 + (n) * 0x10)
#define DWC3_GEVNTSIZ(n)		(0xc408 + (n) * 0x10)
#define DWC3_GEVNTCOUNT(n)		(0xc40c + (n) * 0x10)
#define DWC3_GFLADJ			0xc630

/* ── Device 레지스터 ──────────────────────────────────────────────────── */
#define DWC3_DCFG			0xc700
#define DWC3_DCTL			0xc704
#define DWC3_DEVTEN			0xc708
#define DWC3_DSTS			0xc70c
#define DWC3_DALEPENA			0xc720
#define DWC3_DEPCMDPAR2(n)		(0xc800 + (n) * 0x10)
#define DWC3_DEPCMDPAR1(n)		(0xc804 + (n) * 0x10)
#define DWC3_DEPCMDPAR0(n)		(0xc808 + (n) * 0x10)
#define DWC3_DEPCMD(n)			(0xc80c + (n) * 0x10)
#define DWC3_DEVIMOD(n)			(0xca00 + (n) * 0x4)

/* ── GEVNTSIZ / GEVNTCOUNT ───────────────────────────────────────────── */
/* EVNTINTRPTMASK 는 '마스크' — 1 이면 인터럽트가 오지 않는다 */
#define DWC3_GEVNTSIZ_EVNTINTRPTMASK	BIT(31)
#define DWC3_GEVNTSIZ_EVENTSIZ		GENMASK(15, 0)
#define DWC3_GEVNTCOUNT_HANDLER_BUSY	BIT(31)
#define DWC3_GEVNTCOUNT_EVNTCOUNT	GENMASK(15, 0)

/* ── DEVTEN (디바이스 이벤트 인에이블) ───────────────────────────────── */
#define DWC3_DEVTEN_L1WKUPEVTEN		BIT(14)
#define DWC3_DEVTEN_VENDEVTSTRCVDEN	BIT(12)
#define DWC3_DEVTEN_ERRTICERREVTEN	BIT(9)
#define DWC3_DEVTEN_SOFTEVTEN		BIT(7)
#define DWC3_DEVTEN_U3L2L1SUSPEN	BIT(6)
#define DWC3_DEVTEN_WKUPEVTEN		BIT(4)
#define DWC3_DEVTEN_ULSTCNGEN		BIT(3)
#define DWC3_DEVTEN_CONNECTDONEEVTEN	BIT(2)
#define DWC3_DEVTEN_USBRSTEVTEN		BIT(1)
#define DWC3_DEVTEN_DISSCONNEVTEN	BIT(0)

/* ── GCTL ─────────────────────────────────────────────────────────────── */
#define DWC3_GCTL_PWRDNSCALE		GENMASK(31, 19)
#define DWC3_GCTL_PRTCAPDIR		GENMASK(13, 12)
#define DWC3_GCTL_CORESOFTRESET		BIT(11)
#define DWC3_GCTL_SCALEDOWN		GENMASK(5, 4)
#define DWC3_GCTL_DISSCRAMBLE		BIT(3)
#define DWC3_GCTL_DSBLCLKGTNG		BIT(0)

/* GCTL.PRTCAPDIR 값 */
#define DWC3_GCTL_PRTCAP_HOST		1
#define DWC3_GCTL_PRTCAP_DEVICE		2
#define DWC3_GCTL_PRTCAP_OTG		3

/* ── GSNPSID ──────────────────────────────────────────────────────────── */
#define DWC3_GSNPSID_CORE_ID		GENMASK(31, 16)
#define DWC3_GSNPSID_RELEASE		GENMASK(15, 0)
/* DWC3 계열 코어 ID 상위 16비트. 하위는 릴리스 번호라 고정값이 아니다. */
#define DWC3_CORE_ID_DWC3		0x5533

/* ── GHWPARAMS3 ───────────────────────────────────────────────────────── */
#define DWC3_GHWPARAMS3_NUM_IN_EPS	GENMASK(22, 18)
#define DWC3_GHWPARAMS3_NUM_EPS		GENMASK(17, 12)

/* ── GUSB2PHYCFG ──────────────────────────────────────────────────────── */
#define DWC3_GUSB2PHYCFG_PHYSOFTRST	BIT(31)
#define DWC3_GUSB2PHYCFG_USBTRDTIM	GENMASK(13, 10)
#define DWC3_GUSB2PHYCFG_ENBLSLPM	BIT(8)
#define DWC3_GUSB2PHYCFG_PHYSEL		BIT(7)
#define DWC3_GUSB2PHYCFG_SUSPENDUSB20	BIT(6)
#define DWC3_GUSB2PHYCFG_PHYIF		BIT(3)

/* ── DCFG ─────────────────────────────────────────────────────────────── */
#define DWC3_DCFG_NUMP			GENMASK(21, 17)
#define DWC3_DCFG_INTRNUM		GENMASK(16, 12)
#define DWC3_DCFG_DEVADDR		GENMASK(9, 3)
#define DWC3_DCFG_DEVSPD		GENMASK(2, 0)

/* ── DCTL ─────────────────────────────────────────────────────────────── */
#define DWC3_DCTL_RUN_STOP		BIT(31)
#define DWC3_DCTL_CSFTRST		BIT(30)
#define DWC3_DCTL_KEEPCONNECT		BIT(19)
#define DWC3_DCTL_ULSTCHNGREQ		GENMASK(8, 5)
#define DWC3_DCTL_TSTCTL		GENMASK(4, 1)

/* ── DEPCMD (엔드포인트 명령 인터페이스) ─────────────────────────────
 * dwc2 는 엔드포인트를 레지스터 직접 쓰기로 제어하지만, DWC3 는 EP 마다
 * 명령 큐를 쓴다: PAR2/1/0 에 인자를 넣고 DEPCMD 에 CMDTYP|CMDACT 를 써서
 * 발행한 뒤, 하드웨어가 CMDACT 를 스스로 클리어할 때까지 기다린다.
 */
#define DWC3_DEPCMD_COMMANDPARAM	GENMASK(31, 16)
#define DWC3_DEPCMD_CMDSTATUS		GENMASK(15, 12)
#define DWC3_DEPCMD_HIPRI_FORCERM	BIT(11)
#define DWC3_DEPCMD_CMDACT		BIT(10)
#define DWC3_DEPCMD_CMDIOC		BIT(8)
#define DWC3_DEPCMD_CMDTYP		GENMASK(3, 0)

/* CMDTYP 값 */
#define DWC3_DEPCMD_DEPCFG		1	/* Set Endpoint Configuration */
#define DWC3_DEPCMD_DEPXFERCFG		2	/* Set EP Transfer Resource Config */
#define DWC3_DEPCMD_DEPGETSTATE		3	/* Get Endpoint State */
#define DWC3_DEPCMD_DEPSSTALL		4	/* Set Stall */
#define DWC3_DEPCMD_DEPCSTALL		5	/* Clear Stall */
#define DWC3_DEPCMD_DEPSTRTXFER		6	/* Start Transfer */
#define DWC3_DEPCMD_DEPUPDXFER		7	/* Update Transfer */
#define DWC3_DEPCMD_DEPENDXFER		8	/* End Transfer */
#define DWC3_DEPCMD_DEPSTARTCFG		9	/* Start New Configuration */

/* DEPCFG 명령의 PAR0 (EP 설정) */
#define DWC3_DEPCFG_PAR0_ACTION		GENMASK(31, 30)
#define DWC3_DEPCFG_PAR0_BURST_SIZE	GENMASK(25, 22)	/* 실제값 - 1 */
#define DWC3_DEPCFG_PAR0_FIFO_NUM	GENMASK(21, 17)
#define DWC3_DEPCFG_PAR0_MAX_PACKET	GENMASK(13, 3)
#define DWC3_DEPCFG_PAR0_EP_TYPE	GENMASK(2, 1)
#define DWC3_DEPCFG_ACTION_INITIALIZE	0
#define DWC3_DEPCFG_ACTION_RESTORE	1
#define DWC3_DEPCFG_ACTION_MODIFY	2

/* DEPCFG 명령의 PAR1 */
#define DWC3_DEPCFG_PAR1_FIFO_BASED	BIT(31)
#define DWC3_DEPCFG_PAR1_EP_NUMBER	GENMASK(29, 25)
#define DWC3_DEPCFG_PAR1_INTERVAL	GENMASK(23, 16)
#define DWC3_DEPCFG_PAR1_XFER_NRDY_EN	BIT(10)
#define DWC3_DEPCFG_PAR1_XFER_IP_EN	BIT(9)
#define DWC3_DEPCFG_PAR1_XFER_CMPL_EN	BIT(8)
#define DWC3_DEPCFG_PAR1_INTR_NUM	GENMASK(4, 0)

/* ── DALEPENA ─────────────────────────────────────────────────────────── */
#define DWC3_DALEPENA_USBACTEP		GENMASK(31, 0)

/* ── DSTS ─────────────────────────────────────────────────────────────── */
#define DWC3_DSTS_COREIDLE		BIT(23)
#define DWC3_DSTS_DEVCTRLHLT		BIT(22)
#define DWC3_DSTS_USBLNKST		GENMASK(21, 18)
#define DWC3_DSTS_SOFFN			GENMASK(16, 3)
#define DWC3_DSTS_CONNECTSPD		GENMASK(2, 0)

/* DSTS.CONNECTSPD / DCFG.DEVSPD 값 */
/*
 * 속도 값은 아래 '속도 값' 절의 DWC3_DEVSPD_* / DWC3_CONNECTSPD_* 를 쓴다.
 * DCFG.DEVSPD 와 DSTS.CONNECTSPD 의 인코딩이 서로 달라(LOW 의 유무) 하나로
 * 묶으면 틀린다. 예전의 공용 DWC3_SPEED_* 는 그래서 제거했다.
 */

/* ── TRB (Transfer Request Block) ─────────────────────────────────────
 * DWC3 는 FIFO 직접 접근이 아니라 메모리의 디스크립터로 전송을 기술한다.
 * 하드웨어가 디스크립터를 DMA 로 읽으므로 CPU 쓰기는 반드시 flush 해야
 * 하고, 하드웨어가 돌려준 상태를 읽기 전에는 invalidate 해야 한다.
 *
 * 하드웨어는 TRB 주소의 하위 4비트가 0 일 것을 요구한다 → 16바이트 정렬.
 */
struct dwc3_trb {
	uint32_t ptr_low;
	uint32_t ptr_high;
	uint32_t status;
	uint32_t control;
} __packed __aligned(16);

/* TRB status 필드 */
#define DWC3_TRB_BUFSIZ			GENMASK(23, 0)
#define DWC3_TRB_PCM1			GENMASK(25, 24)
#define DWC3_TRB_SPR			BIT(26)		/* Short Packet Received */
#define DWC3_TRB_TRBSTS			GENMASK(31, 28)
#define DWC3_TRB_TRBSTS_SETUP_PENDING	2

/* TRB control 필드 */
#define DWC3_TRB_HWO			BIT(0)	/* Hardware Owner of Descriptor */
#define DWC3_TRB_LST			BIT(1)	/* Last TRB */
#define DWC3_TRB_CHN			BIT(2)	/* Chain Buffers */
#define DWC3_TRB_CSP			BIT(3)	/* Continue on Short Packet */
#define DWC3_TRB_TRBCTL			GENMASK(8, 4)
#define DWC3_TRB_ISP			BIT(10)	/* Interrupt on Short Packet */
#define DWC3_TRB_IMI			BIT(10)	/* Interrupt on Missed ISOC */
#define DWC3_TRB_IOC			BIT(11)	/* Interrupt on Complete */
#define DWC3_TRB_STREAM_ID		GENMASK(29, 14)
#define DWC3_TRB_SOF_NUM		GENMASK(29, 14)

/* TRBCTL 값 (control 의 8:4 자리에 들어간 상태로 정의) */
#define DWC3_TRBCTL_NORMAL		(1U << 4)
#define DWC3_TRBCTL_SETUP		(2U << 4)
#define DWC3_TRBCTL_STATUS_2		(3U << 4)
#define DWC3_TRBCTL_STATUS_3		(4U << 4)
#define DWC3_TRBCTL_CONTROL_DATA	(5U << 4)
#define DWC3_TRBCTL_ISOCH_FIRST		(6U << 4)
#define DWC3_TRBCTL_ISOCH		(7U << 4)
#define DWC3_TRBCTL_LINK		(8U << 4)

/* ── 이벤트 디코딩 ────────────────────────────────────────────────────
 * 이벤트 32비트의 bit0 이 1 이면 '엔드포인트 전용이 아님' = 디바이스 이벤트.
 */
#define DWC3_EVT_NON_EP			BIT(0)

/* DEVT — 디바이스 이벤트 */
#define DWC3_DEVT_TYPE			GENMASK(14, 8)
#define DWC3_DEVT_INFO			GENMASK(31, 16)

#define DWC3_DEVT_DISCONNECT		0
#define DWC3_DEVT_USB_RESET		1
#define DWC3_DEVT_CONNECTION_DONE	2
#define DWC3_DEVT_LINK_STATE_CHANGE	3
#define DWC3_DEVT_REMOTE_WAKEUP		4
#define DWC3_DEVT_HIBERNATE_REQUEST	5
#define DWC3_DEVT_SUSPEND_ENTRY		6
#define DWC3_DEVT_SOF			7
#define DWC3_DEVT_ERRATIC_ERROR		9
#define DWC3_DEVT_COMMAND_COMPLETE	10
#define DWC3_DEVT_EVENT_BUF_OVERFLOW	11
#define DWC3_DEVT_VENDOR_TEST_LMP	12
#define DWC3_DEVT_STOPPED_DISCONNECT	13
#define DWC3_DEVT_L1_RESUME_DETECT	14
#define DWC3_DEVT_LDM_RESPONSE		15

/*
 * DEPEVT — 엔드포인트 이벤트. 필드 '위치'만 정의한다.
 * ✅ type 값은 dwc3-types.h (BSD-2) 에서 확인했다 — 아래 DEPEVT 타입 값.
 *    status 는 타입마다 뜻이 달라 여전히 원시값으로 로그한다.
 */
#define DWC3_DEPEVT_PHYS_EP		GENMASK(5, 1)
#define DWC3_DEPEVT_TYPE		GENMASK(9, 6)
#define DWC3_DEPEVT_STATUS		GENMASK(15, 12)
#define DWC3_DEPEVT_PARAMS		GENMASK(31, 16)

/* ── DEPEVT 타입 값 ─────────────────────────────────────────────────────
 * 출처: Fuchsia dwc3-types.h (BSD-2-Clause, LICENSE.fuchsia 참고)
 */
#define DWC3_DEPEVT_XFER_COMPLETE	1
#define DWC3_DEPEVT_XFER_IN_PROGRESS	2
#define DWC3_DEPEVT_XFER_NOT_READY	3
#define DWC3_DEPEVT_STREAM_EVT		6
#define DWC3_DEPEVT_CMD_CMPLT		7

/* XFER_NOT_READY 의 이벤트 파라미터 (raw 이벤트 워드 기준 비트 위치) */
#define DWC3_DEPEVT_XNR_REASON		BIT(15)
#define DWC3_DEPEVT_XNR_STAGE		GENMASK(13, 12)
#define DWC3_DEPEVT_XNR_STAGE_DATA	1
#define DWC3_DEPEVT_XNR_STAGE_STATUS	2

/* CMD_CMPLT 의 이벤트 파라미터 */
#define DWC3_DEPEVT_CC_CMD_TYPE		GENMASK(27, 24)
#define DWC3_DEPEVT_CC_RSRC_ID		GENMASK(22, 16)

/* ── 엔드포인트 타입 값 (DEPCFG PAR0 의 EP_TYPE) ────────────────────────
 * DWC3 의 EP_TYPE 필드는 USB 2.0 규격의 bmAttributes[1:0] 인코딩을 그대로
 * 쓴다. Fuchsia 드라이버가 USB 엔드포인트 타입 enum 을 변환 없이 이 필드에
 * 넣는 것으로 확인했다 (dwc3-commands.cc 의 CmdEpSetConfig).
 * 따라서 Zephyr 의 USB_EP_TYPE_* 와 값이 같다.
 */
#define DWC3_DEPCFG_EP_TYPE_CONTROL	0
#define DWC3_DEPCFG_EP_TYPE_ISOC	1
#define DWC3_DEPCFG_EP_TYPE_BULK	2
#define DWC3_DEPCFG_EP_TYPE_INTERRUPT	3

/* ── DEPCFG 보강 ───────────────────────────────────────────────────────
 * 기존 정의에 빠져 있던 비트들. 지금 쓰지는 않지만, 같은 레지스터를 다시
 * 조사하지 않도록 함께 적어 둔다.
 */
#define DWC3_DEPCFG_PAR0_INTERNAL_RETRY	BIT(15)
#define DWC3_DEPCFG_PAR1_STREAM_CAPABLE	BIT(24)
#define DWC3_DEPCFG_PAR1_EBC		BIT(15)	/* External Buffer Control */
#define DWC3_DEPCFG_PAR1_EBC_NO_WB	BIT(14)	/* HWO 를 TRB 에 되쓰지 않음 */
#define DWC3_DEPCFG_PAR1_STREAM_EVT_EN	BIT(13)

/* ── DEPXFERCFG ────────────────────────────────────────────────────────
 * PAR0 = 이 엔드포인트에 할당할 전송 리소스 개수. EP0 은 1 이면 된다.
 */
#define DWC3_DEPXFERCFG_NUM_XFER_RES	GENMASK(15, 0)

/* ── DEPSTRTXFER 의 파라미터 배치 ───────────────────────────────────────
 * ⚠ 직관과 반대다. Synopsys DWC3 databook 규정상 64비트 TRB 물리주소는
 *
 *     DEPCMDPAR0 (오프셋 0x08)  <-  상위 32비트 (63:32)
 *     DEPCMDPAR1 (오프셋 0x04)  <-  하위 32비트 (31:0)
 *
 * 즉 '하위 주소가 낮은 오프셋' 이라는 리틀엔디안 배치이고, PAR 번호와는
 * 거꾸로다 (PAR0 의 주소가 PAR1 보다 크다). 추측하면 틀리는 자리다.
 * 출처: Fuchsia dwc3-commands.cc 의 CmdEpStartTransfer 주석 (BSD-2).
 */
#define DWC3_DEPSTRTXFER_PAR0(trb_phys)	((uint32_t)((uint64_t)(trb_phys) >> 32))
#define DWC3_DEPSTRTXFER_PAR1(trb_phys)	((uint32_t)((trb_phys) & 0xffffffffU))

/* ── 속도 값 ───────────────────────────────────────────────────────────
 * DCFG.DEVSPD 에 쓰는 값과 DSTS.CONNECTSPD 에서 읽히는 값. 인코딩이 다르다
 * (DEVSPD 에는 LOW=2 가 있고 CONNECTSPD 에는 없다).
 */
#define DWC3_DEVSPD_HIGH		0
#define DWC3_DEVSPD_FULL		1
#define DWC3_DEVSPD_LOW			2
#define DWC3_DEVSPD_SUPER		4
#define DWC3_DEVSPD_ENHANCED_SUPER	5

#define DWC3_CONNECTSPD_HIGH		0
#define DWC3_CONNECTSPD_FULL		1
#define DWC3_CONNECTSPD_SUPER		4
#define DWC3_CONNECTSPD_ENHANCED_SUPER	5

#endif /* ZEPHYR_DRIVERS_USB_UDC_DWC3_REGS_H */
