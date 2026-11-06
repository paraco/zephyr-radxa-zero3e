/*
 * Copyright (c) 2026 paraco
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Synopsys DesignWare USB 3.x (DWC3) — USB device controller (UDC) 드라이버
 *
 * 대상: Rockchip RK3566 의 usb_host0_xhci @0xfcc00000
 *       (Radxa ZERO 3E 의 "USB 2.0 Type-C OTG" 포트)
 *       RK356x 에서 device 모드가 가능한 유일한 컨트롤러이며,
 *       dwc2 는 이 SoC 에 존재하지 않는다.
 *
 * ── 범위 ────────────────────────────────────────────────────────────────
 *   O  USB 2.0 High Speed, device(peripheral) 모드
 *   X  SuperSpeed  — 보드가 USB3 combphy 를 연결하지 않음.
 *                    실제로 호스트에서 High Speed 로만 열거됨을 확인
 *   X  host / OTG 역할 전환
 *   X  hibernation / LPM
 *   X  ISO 엔드포인트 — 목표는 CDC-ACM (bulk IN/OUT + interrupt IN)
 *
 * ── 구현 단계 ───────────────────────────────────────────────────────────
 *   [2] 이 파일 — 뼈대. UDC API 16함수 스텁 + 인스턴스 생성.
 *                 하드웨어 레지스터는 아직 건드리지 않는다.
 *   [3] 코어 초기화 (GCTL/DCTL 소프트리셋, PHY)
 *   [4] 이벤트 버퍼 + ISR
 *   [5] DEPCMD 명령 인터페이스
 *   [6] TRB 링 + 캐시 관리
 *   [7] EP0 제어 전송
 *   [8] bulk/interrupt 엔드포인트
 *
 * ── dwc2 와 다른 점 (왜 포팅이 아니라 신작인가) ─────────────────────────
 *   EP 제어  : 레지스터 직접 쓰기 -> DEPCMD 명령 큐
 *   전송     : FIFO 직접 접근     -> TRB 링 (메모리 디스크립터, HWO 소유권 비트)
 *   인터럽트 : 상태 레지스터 폴링 -> 이벤트 버퍼 (GEVNTCOUNT 소비 모델)
 *
 * ── 라이선스 경계 ───────────────────────────────────────────────────────
 *   Linux 의 drivers/usb/dwc3 (GPL-2.0) 와 바인딩 문서 snps,dwc3.yaml
 *   (GPL-2.0 단독) 은 참조하지 않는다. 참조 가능한 것은 Synopsys DWC3
 *   databook, Linux *디바이스트리 소스*(GPL-2.0+ OR MIT -> MIT 로 사용),
 *   그리고 Zephyr 자체 코드뿐이다.
 */

#include "udc_common.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <zephyr/kernel/internal/mm.h>

#include "udc_dwc3_regs.h"
#include "udc_dwc3_diag.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_dwc3, CONFIG_UDC_DRIVER_LOG_LEVEL);

#define DT_DRV_COMPAT snps_dwc3

/* 비휘발 설정 — dev->config 로 접근 */
struct udc_dwc3_config {
	/* DT 가 준 *물리* 주소와 크기. MMU 가 켜져 있으므로 그대로
	 * 역참조하면 데이터 어보트다 — init 에서 device_map() 한다. */
	uintptr_t phys;
	size_t size;
	size_t num_of_eps;
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	void (*irq_connect)(void);
	void (*make_thread)(const struct device *dev);
	int speed_idx;
};

/*
 * 물리 엔드포인트 개수. 물리 번호 = (ep_num << 1) | is_in 이므로
 * USB 엔드포인트 N 개면 물리 2N 개다.
 */
#define DWC3_NUM_PHYS_EPS	(2 * DT_INST_PROP(0, num_bidir_endpoints))

/*
 * EP0 제어 전송 상태.
 *
 * DWC3 는 제어 전송의 각 단계를 TRB 로 기술하고, 호스트가 다음 단계를
 * 요구하면 XferNotReady 이벤트로 알려 준다. 반면 Zephyr 의 UDC 스택은
 * 단계별 버퍼를 비동기로 enqueue 한다. 그래서 드라이버가 '하드웨어가
 * 요구한 단계' 와 '스택이 준 버퍼' 를 짝지어야 한다.
 *
 * SETUP TRB 는 선제적으로 무장한다. DATA 는 버퍼가 오면 바로 시작한다.
 * STATUS 만 두 조건(버퍼 + XferNotReady(STATUS))이 모두 갖춰져야 한다 —
 * 어느 쪽이 먼저 올지 알 수 없어 ep0_nrdy_status 로 기억한다.
 */
enum dwc3_ep0_state {
	DWC3_EP0_SETUP,		/* SETUP TRB 무장 상태로 대기 */
	DWC3_EP0_DATA_WAIT,	/* SETUP 소비 — 스택의 데이터 버퍼 대기 */
	DWC3_EP0_TWO_STAGE,	/* wLength==0 — 데이터 단계가 없다 */
	DWC3_EP0_DATA_OUT,	/* 호스트→장치 데이터 수신 중 */
	DWC3_EP0_DATA_IN,	/* 장치→호스트 데이터 송신 중 */
	DWC3_EP0_STATUS,	/* STATUS TRB 진행 중 */
};

/* 드라이버 사설 데이터 — udc_get_private(dev) 로 접근 */
struct udc_dwc3_data {
	/* device_map() 으로 얻은 가상 주소. init 전에는 0 이다. */
	mm_reg_t base;
	/* GHWPARAMS3 에서 읽은 실제 물리 엔드포인트 수 (init 에서 채움) */
	uint8_t hw_num_eps;
	/* 이벤트 링의 읽기 위치 (바이트 단위, 링 내부 오프셋) */
	uint32_t evt_pos;

	/* ── [7b] 아래쪽 절반(bottom half) ──────────────────────────────
	 * udc_setup_received() 는 UDC 락에 의존해 '스레드 문맥에서만'
	 * 부를 수 있다 (udc_common.h 의 계약). 그래서 이벤트 처리를 ISR
	 * 에서 할 수 없고, ISR 은 인터럽트를 마스크하고 깃발만 올린다.
	 * 덤으로 ISR 에서 DEPCMD 를 동기 대기하던 문제도 없어진다.
	 */
	struct k_event drv_evt;

	/*
	 * USBPHY_U3_GRF 매핑. init 에서 한 번 매핑해 두고 STATUS(0x00C0)를
	 * 나중에도 읽는다 — PHY 가 실제로 보고 있는 linestate/vbusvalid/
	 * bvalid/iddig 를 확인하는 유일한 창이다.
	 */
	mm_reg_t phy_grf;

	struct k_thread thread_data;

	/* 받은 SETUP 8바이트 사본. 스레드가 스택에 올려 준다. */
	uint8_t setup[8];

	/* EP0 제어 전송 상태 기계 */
	enum dwc3_ep0_state ep0_state;
	/* 현재 데이터 단계로 하드웨어에 건 길이 (잔량 계산용) */
	uint32_t ep0_xfer_len;
	/* 호스트가 요청한 전체 데이터 길이 (SETUP 의 wLength) */
	uint16_t ep0_wlength;
	/* XferNotReady(STATUS) 를 이미 받았는가 — 버퍼보다 먼저 올 수 있다 */
	bool ep0_nrdy_status;

	/* ── [8] 일반 엔드포인트 ────────────────────────────────────────
	 * 물리 엔드포인트별 상태. EP0(0,1)도 배열에 들어 있지만 EP0 은
	 * 자기 상태 기계를 쓰므로 rsrc_id 만 의미가 있다.
	 */
	struct {
		/*
		 * 전송 리소스 인덱스. DEPENDXFER 에 반드시 필요하다.
		 * DEPSTRTXFER 의 CMD_CMPLT 이벤트로 받는다 — DEPCMD 를
		 * 읽어서 얻는 것이 아니다.
		 */
		uint8_t rsrc_id;
		bool rsrc_valid;
		/*
		 * 이 엔드포인트에 하드웨어가 걸어 준 TRB 길이.
		 * 완료 시 '남은 바이트' 를 빼서 실제 전송량을 구한다.
		 */
		uint32_t xfer_len;
		/*
		 * ⚠ XferNotReady 를 한 번이라도 받았는가.
		 *   DWC3 는 일반 엔드포인트에서 이 이벤트를 받기 전에
		 *   DEPSTRTXFER 를 내는 것을 기대하지 않는다. 받기 전에
		 *   걸면 전송이 시작되지 않을 수 있다.
		 */
		bool got_not_ready;
		/* DEPENDXFER 를 내고 완료 이벤트를 기다리는 중 */
		bool canceling;
	} ep[DWC3_NUM_PHYS_EPS];

	/*
	 * DEPSTARTCFG(base=2) 를 냈는가. SET_CONFIGURATION 이후 EP2 이상을
	 * 설정하기 '전에' 정확히 한 번 내야 한다 (databook 규정). 리셋과
	 * enable 에서 되돌린다.
	 */
	bool started_cfg2;
	/* 진단 카운터 — 동작에 영향이 없다. udc_dwc3_diag.h 참고 */
	struct udc_dwc3_stats stats;
};

static inline uint32_t dwc3_read32(const struct device *dev, uint32_t off)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);

	return sys_read32(priv->base + off);
}

static inline void dwc3_write32(const struct device *dev, uint32_t off, uint32_t val)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);

	sys_write32(val, priv->base + off);
}

/* 아래쪽에 정의된 것들의 전방 선언 (UDC API 가 먼저 나오기 때문) */
/*
 * ISR → 스레드 깃발.
 *   EVENTS — 이벤트 링에 처리할 것이 있다 (ISR 이 인터럽트를 마스크했다)
 *   XFER   — 새 전송이 엔드포인트 큐에 들어왔다 (ep_enqueue 가 올린다)
 */
#define DWC3_DRV_EVT_EVENTS	0
#define DWC3_DRV_EVT_XFER	1

/*
 * 물리 엔드포인트 번호. DWC3 는 USB 엔드포인트 하나를 방향별로 쪼개
 * 물리 번호를 매긴다: 물리 = (ep_num << 1) | is_in.
 * 따라서 제어 엔드포인트는 EP0 OUT = 0, EP0 IN = 1 이다.
 * ISR 과 EP0 설정 코드가 모두 쓰므로 여기서 정의한다.
 */
#define DWC3_EP0_OUT		0U
#define DWC3_EP0_IN		1U
#define DWC3_EP0_MPS	64U	/* High Speed 제어 엔드포인트는 64바이트 */

/*
 * Rockchip USB2 PHY GRF — [임시] 보드 고유 값.
 * 제대로 하려면 PHY 드라이버와 DT 참조로 빼야 한다. 자세한 근거는
 * udc_dwc3_init 의 주석.
 */
/*
 * Rockchip USBPHY_U3_GRF — OTG PHY 의 GRF. [임시] 보드 고유 값.
 *
 * 출처: Rockchip RK3568 TRM Part1 V1.3, 3.9절 "USBPHY_U3_GRF_Register
 * Description" (419쪽). 메모리맵에 USBPHY_U3_GRF = 0xFDCA0000 (32KB) 이고,
 * 보드의 라이브 DT 에서도 usb2-phy@fe8a0000 의 rockchip,usbgrf 가 이것을
 * 가리킨다.
 *
 * 모든 레지스터가 hiword-mask 다 — "Write enable for lower 16bits, each
 * bit is individual" (상위 16비트가 비트별 쓰기 허용). GPIO 와 같은 규약.
 *
 * ⚠ 제대로 하려면 PHY 드라이버로 빼야 한다. 지금은 bring-up 임시 조치다.
 */
#define DWC3_RK_USB2PHY_GRF	0xfdca0000UL

/* CON0 (0x0000): OTG 포트 UTMI 제어 */
#define DWC3_RK_PHY_CON0	0x0000
#define DWC3_RK_CON0_IDDIG	BIT(10)	/* GRF OTG plug iddig 지시자 (1=B-device) */
#define DWC3_RK_CON0_IDDIG_SEL	BIT(9)	/* 1=GRF 지시자를 컨트롤러에 */

/* CON2 (0x0008): bvalid/vbusvalid 와 PLL */
#define DWC3_RK_PHY_CON2	0x0008
#define DWC3_RK_CON2_BV_SEL	BIT(15)	/* 1=GRF 값을 bvalid/vbusvalid 로 */
#define DWC3_RK_CON2_BV_GRF	BIT(14)	/* 컨트롤러에 줄 bvalid/vbusvalid 값 */

/*
 * STATUS (0x00C0): PHY 가 실제로 보고 있는 상태. 전부 RO.
 * OTG 쪽 비트만 적는다 (HOST 쪽은 12~22).
 */
#define DWC3_RK_PHY_STATUS	0x00C0
#define DWC3_RK_ST_LS_FS_RCV	BIT(11)		/* usbotg_phy_ls_fs_rcv */
#define DWC3_RK_ST_AVALID	BIT(10)		/* usbotg_utmi_avalid */
#define DWC3_RK_ST_BVALID	BIT(9)		/* usbotg_utmi_bvalid */
#define DWC3_RK_ST_HOSTDISC	BIT(7)		/* usbotg_utmi_hostdisconnect */
#define DWC3_RK_ST_IDDIG	BIT(6)		/* usbotg_utmi_iddig */
#define DWC3_RK_ST_LINESTATE	GENMASK(5, 4)	/* usbotg_utmi_linestate */
#define DWC3_RK_ST_SESSEND	BIT(3)		/* usbotg_utmi_sessend */
#define DWC3_RK_ST_VBUSVALID	BIT(2)		/* usbotg_utmi_vbusvalid */

/* 상위 16비트 쓰기 허용 + 하위 값을 한 번에 만든다 */
#define DWC3_RK_HIWORD(bits)	(((uint32_t)(bits) << 16) | (uint32_t)(bits))

/* USB 엔드포인트 주소(0x81 등) -> 물리 번호 */
#define DWC3_PHYS_EP(addr)	\
	((USB_EP_GET_IDX(addr) << 1) | (USB_EP_DIR_IS_IN(addr) ? 1U : 0U))

static void dwc3_evt_buf_setup(const struct device *dev);
static void udc_dwc3_isr(const struct device *dev);
static int dwc3_ep0_configure(const struct device *dev);
/* [8] 일반 엔드포인트 */
static int dwc3_ep_configure(const struct device *dev,
			     struct udc_ep_config *const cfg);
static void dwc3_ep_xfer_next(const struct device *dev,
			      struct udc_ep_config *const ep_cfg);
static void dwc3_ep_xfer_complete(const struct device *dev, uint8_t phys);
static void dwc3_handle_cmd_cmplt(const struct device *dev, uint8_t phys,
				  uint32_t raw);
static int dwc3_ep_end_xfer(const struct device *dev, uint8_t phys,
			    bool want_event);
static void dwc3_handle_events(const struct device *dev);
static int dwc3_dep_cmd_ioc(const struct device *dev, uint8_t ep,
			    uint32_t cmdtyp, uint32_t par0, uint32_t par1,
			    uint32_t par2, uint32_t extra, uint32_t *status);
static int dwc3_dep_cmd(const struct device *dev, uint8_t ep,
			uint32_t cmdtyp, uint32_t par0, uint32_t par1,
			uint32_t par2, uint32_t *status);
static int dwc3_ep0_arm_setup(const struct device *dev);
static void dwc3_ep0_setup_received(const struct device *dev, uint32_t raw);
static void dwc3_ep0_xfer_complete(const struct device *dev, uint8_t ep);
static void dwc3_ep0_xfer_not_ready(const struct device *dev, uint8_t ep,
				    uint32_t stage);
static void dwc3_rk_log_phy_status(const struct device *dev, const char *when);
static void dwc3_handle_xfer_next(const struct device *dev,
				  struct udc_ep_config *const ep_cfg);
static void dwc3_ep0_kick(const struct device *dev);
static int dwc3_ep0_configure(const struct device *dev);

/* ── UDC API ──────────────────────────────────────────────────────────── */

/*
 * udc_ep_enqueue() 문맥에서 호출되며 블로킹하면 안 된다.
 * [6]~[8] 에서 TRB 를 준비해 DEPSTRTXFER 를 내는 자리가 된다.
 */
/*
 * 스택이 전송을 요청한다. 여기서 바로 하드웨어를 건드리지 않고 큐에 넣고
 * 스레드를 깨운다 — 호출 문맥이 보장되지 않고, 전송 시작 조건(상태 단계는
 * XferNotReady 필요)이 갖춰졌는지는 스레드가 판단한다.
 */
static int udc_dwc3_ep_enqueue(const struct device *dev,
			       struct udc_ep_config *const cfg,
			       struct net_buf *buf)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);

	udc_buf_put(cfg, buf);
	k_event_post(&priv->drv_evt, BIT(DWC3_DRV_EVT_XFER));

	return 0;
}

static int udc_dwc3_ep_dequeue(const struct device *dev,
			       struct udc_ep_config *const cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t ep = USB_EP_DIR_IS_IN(cfg->addr) ? DWC3_EP0_IN : DWC3_EP0_OUT;
	uint32_t st;

	if (USB_EP_GET_IDX(cfg->addr) != 0U) {
		/*
		 * 일반 엔드포인트는 rsrc_id 를 보관하므로 DEPENDXFER 로
		 * 제대로 끊을 수 있다 — EP0 처럼 stall 로 우회하지 않는다.
		 */
		uint8_t phys = DWC3_PHYS_EP(cfg->addr);

		if (phys >= DWC3_NUM_PHYS_EPS) {
			return -EINVAL;
		}

		(void)dwc3_ep_end_xfer(dev, phys, false);
		udc_ep_set_busy(cfg, false);
		udc_ep_cancel_queued(dev, cfg);
		return 0;
	}

	/*
	 * EP0: 진행 중인 전송을 끊는다.
	 *
	 * ⚠ rsrc_id 를 보관하지 않아 DEPENDXFER 를 정확히 낼 수 없다.
	 *   그래서 stall 로 끊는다. 활성 TRB 가 하드웨어 소유로 남을 수
	 *   있으므로, 상태 기계를 SETUP 으로 되돌리고 TRB 를 재무장해
	 *   그 다음 전송이 깨끗한 상태에서 시작하게 한다.
	 *   rsrc_id 보관과 DEPENDXFER 는 [8] 에서 넣는다.
	 *
	 * 호출 시점: 스택은 usbd_ep_disable 과 종료 시의 udc_purge_queues
	 *   에서만 이 함수를 부른다. 전송 한가운데서 불리는 경로는 지금
	 *   없으므로 이 임시 조치가 실제로 문제를 일으키지는 않는다.
	 */
	(void)dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPSSTALL, 0U, 0U, 0U, &st);
	cfg->stat.halted = true;
	udc_ep_set_busy(cfg, false);
	udc_ep_cancel_queued(dev, cfg);

	priv->ep0_state = DWC3_EP0_SETUP;
	priv->ep0_nrdy_status = false;
	(void)dwc3_ep0_arm_setup(dev);

	return 0;
}

/*
 * EP0 은 enable() 에서 이미 설정했으므로 여기서는 성공만 돌려준다.
 * 일반 엔드포인트의 DEPCFG·DALEPENA·TRB 링은 [8] 에서.
 */
static int udc_dwc3_ep_enable(const struct device *dev,
			      struct udc_ep_config *const cfg)
{
	/* EP0 은 udc_dwc3_enable() 에서 이미 설정했다 */
	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		return 0;
	}

	return dwc3_ep_configure(dev, cfg);
}

static int udc_dwc3_ep_disable(const struct device *dev,
			       struct udc_ep_config *const cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t phys = DWC3_PHYS_EP(cfg->addr);

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		return 0;
	}

	if (phys >= DWC3_NUM_PHYS_EPS) {
		return -EINVAL;
	}

	/*
	 * 진행 중인 전송을 끊는다. 완료 이벤트를 받지 않는 쪽을 택했으므로
	 * dwc3_ep_end_xfer 안에서 DMA 가 멎을 시간을 기다린다 — 그 전에
	 * DALEPENA 를 지우면 하드웨어가 아직 쓰고 있는 메모리를 놓게 된다.
	 */
	(void)dwc3_ep_end_xfer(dev, phys, false);

	/*
	 * ⚠ DALEPENA 는 엔드포인트 명령이 진행 중인 동안 지우면 안 된다.
	 *   CMDACT 가 내려간 뒤에 지운다.
	 */
	if ((dwc3_read32(dev, DWC3_DEPCMD(phys)) & DWC3_DEPCMD_CMDACT) != 0U) {
		/*
		 * 명령이 아직 활성이라 DALEPENA 를 지울 수 없다. 여기서
		 * 0 을 돌려주면 스택은 엔드포인트가 꺼진 줄 알지만 하드웨어
		 * 에서는 활성인 채로 남는다 — 그 어긋남을 숨기지 않는다.
		 */
		LOG_ERR("ep%u DALEPENA 못 지움 — DEPCMD 가 아직 활성", phys);
		return -EIO;
	}

	dwc3_write32(dev, DWC3_DALEPENA,
		     dwc3_read32(dev, DWC3_DALEPENA) & ~BIT(phys));

	priv->ep[phys].got_not_ready = false;
	priv->ep[phys].rsrc_valid = false;
	udc_ep_set_busy(cfg, false);
	udc_ep_cancel_queued(dev, cfg);

	/*
	 * 일반 엔드포인트가 모두 꺼졌으면 DEPSTARTCFG(base=2)를 다시 낼 수
	 * 있게 해 둔다. 호스트가 버스 리셋 없이 SET_CONFIGURATION 을 다시
	 * 보내면(구성 전환) 스택이 엔드포인트를 껐다 켜는데, 그때 전송
	 * 리소스 할당을 다시 시작해야 한다.
	 */
	if ((dwc3_read32(dev, DWC3_DALEPENA) &
	     ~(BIT(DWC3_EP0_OUT) | BIT(DWC3_EP0_IN))) == 0U) {
		priv->started_cfg2 = false;
		LOG_DBG("일반 엔드포인트 모두 꺼짐 — DEPSTARTCFG 재발행 허용");
	}

	LOG_DBG("ep 0x%02x (phys %u) disabled", cfg->addr, phys);
	return 0;
}

static int udc_dwc3_ep_set_halt(const struct device *dev,
				struct udc_ep_config *const cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t ep = USB_EP_DIR_IS_IN(cfg->addr) ? DWC3_EP0_IN : DWC3_EP0_OUT;
	uint32_t st;
	int err;

	if (USB_EP_GET_IDX(cfg->addr) != 0U) {
		/*
		 * 일반 엔드포인트. 진행 중 전송을 먼저 끊은 뒤 stall 한다 —
		 * 활성 TRB 를 남겨 두면 stall 해제 후 엉뚱한 전송이 재개된다.
		 */
		uint8_t phys = DWC3_PHYS_EP(cfg->addr);

		if (phys >= DWC3_NUM_PHYS_EPS) {
			return -EINVAL;
		}

		(void)dwc3_ep_end_xfer(dev, phys, false);
		err = dwc3_dep_cmd(dev, phys, DWC3_DEPCMD_DEPSSTALL,
				   0U, 0U, 0U, &st);
		if (err == 0) {
			cfg->stat.halted = true;
			udc_ep_set_busy(cfg, false);
			LOG_DBG("ep%u stalled (status=0x%x)", phys, st);
		}
		return err;
	}

	err = dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPSSTALL, 0U, 0U, 0U, &st);
	if (err != 0) {
		return err;
	}

	cfg->stat.halted = true;
	LOG_DBG("ep%u stalled (status=0x%x)", ep, st);

	/*
	 * ⚠ stall 만 걸고 끝내면 안 된다.
	 *
	 * 스택은 지원하지 않는 요청이나 버퍼 부족에서 ctrl_xfer_stall 로
	 * 가는데, 거기서 udc_ep_set_halt 와 usbd_enqueue_setup 을 부르지만
	 * 드라이버의 상태 기계는 건드리지 않는다. 호스트는 열거 중
	 * 지원하지 않는 요청을 흔히 보낸다. 그때 상태가 DATA_WAIT /
	 * TWO_STAGE 로 남고 SETUP TRB 도 무장되지 않으면, 이후 SETUP 이
	 * 와도 디스패처가 데이터 완료로 오해해 영구히 멈춘다.
	 *
	 * 그래서 stall 뒤에 상태를 SETUP 으로 되돌리고 TRB 를 재무장한다.
	 * 제어 엔드포인트의 stall 은 SETUP 수신 시 하드웨어가 자동
	 * 해제하므로, 재무장이 stall 을 무력화하지는 않는다.
	 */
	priv->ep0_state = DWC3_EP0_SETUP;
	priv->ep0_nrdy_status = false;
	(void)dwc3_ep0_arm_setup(dev);

	return 0;
}

/*
 * 제어 엔드포인트의 stall 은 SETUP 수신으로 하드웨어가 자동 해제한다.
 * 그래도 스택이 명시적으로 부르면 DEPCSTALL 을 낸다.
 */
static int udc_dwc3_ep_clear_halt(const struct device *dev,
				  struct udc_ep_config *const cfg)
{
	uint8_t ep = (USB_EP_GET_IDX(cfg->addr) == 0U) ?
		     (USB_EP_DIR_IS_IN(cfg->addr) ? DWC3_EP0_IN : DWC3_EP0_OUT) :
		     DWC3_PHYS_EP(cfg->addr);
	uint32_t st;
	int err;

	if (ep >= DWC3_NUM_PHYS_EPS) {
		return -EINVAL;
	}

	err = dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPCSTALL, 0U, 0U, 0U, &st);
	if (err == 0) {
		cfg->stat.halted = false;
		LOG_DBG("ep%u stall cleared (status=0x%x)", ep, st);
		/* 막혀 있던 큐를 다시 흐르게 한다 */
		if (USB_EP_GET_IDX(cfg->addr) != 0U) {
			dwc3_ep_xfer_next(dev, cfg);
		}
	}

	return err;
}

/*
 * SET_ADDRESS. 주소는 DCFG.DEVADDR 에 그대로 쓴다.
 *
 * 호출 시점은 caps.addr_before_status 로 우리가 정한다. 그 플래그를 세워
 * 두었으므로 스택은 SET_ADDRESS 의 '상태 단계 전에' 이 함수를 부른다
 * (세우지 않으면 상태 단계 뒤에 부른다 — usbd_ch9.c).
 *
 * DWC3 가 상태 단계 전을 요구하는 근거: SET_ADDRESS 를 받으면 DEVADDR 를
 * 곧바로 쓰고 그 다음에 상태 단계를 시작하는 것이 이 컨트롤러의 절차다.
 * 진행 중인 제어 전송은 옛 주소로 끝나고 다음 전송부터 새 주소가 쓰인다.
 */
static int udc_dwc3_set_address(const struct device *dev, const uint8_t addr)
{
	uint32_t dcfg;

	dcfg = dwc3_read32(dev, DWC3_DCFG);
	dcfg &= ~DWC3_DCFG_DEVADDR;
	dcfg |= FIELD_PREP(DWC3_DCFG_DEVADDR, addr);
	dwc3_write32(dev, DWC3_DCFG, dcfg);

	LOG_DBG("device address = %u (DCFG=0x%08x)", addr, dcfg);
	return 0;
}

static int udc_dwc3_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

/*
 * 이 보드는 USB2 PHY 만 물려 있어 High Speed 가 상한이다
 * (DT 의 maximum-speed = "high-speed").
 * [3] 에서 DSTS 를 읽어 실제 협상 속도를 돌려주도록 바꾼다.
 */
static enum udc_bus_speed udc_dwc3_device_speed(const struct device *dev)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t spd;

	if (priv->base == 0U) {
		/* init 전 — DT 가 선언한 능력으로 답한다 */
		struct udc_data *data = dev->data;

		return data->caps.hs ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS;
	}

	spd = FIELD_GET(DWC3_DSTS_CONNECTSPD, dwc3_read32(dev, DWC3_DSTS));

	switch (spd) {
	case DWC3_CONNECTSPD_HIGH:
		return UDC_BUS_SPEED_HS;
	case DWC3_CONNECTSPD_FULL:
		return UDC_BUS_SPEED_FS;
	default:
		/*
		 * SuperSpeed 는 이 보드에서 나올 수 없다 (combphy 미연결).
		 * 나오면 DT·PHY 설정이 의도와 다르다는 신호다.
		 */
		LOG_WRN("unexpected CONNECTSPD=%u", spd);
		return UDC_BUS_SPEED_HS;
	}
}

static int udc_dwc3_enable(const struct device *dev)
{
	const struct udc_dwc3_config *config = dev->config;
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t dctl, dcfg;
	int err;

	dwc3_evt_buf_setup(dev);

	/*
	 * 받을 디바이스 이벤트. 지금 단계에서 의미 있는 것만 켠다 —
	 * 리셋 / 연결 완료 / 분리. 나머지는 [7] 이후.
	 */
	/*
	 * ⚠ 전원 관리 이벤트를 켠다 (2026-11-06, 1단계).
	 *
	 * 지금까지 USBRST·CONNECTDONE·DISCONN 만 켜 두어 **suspend 이벤트가
	 * 애초에 오지 않았다.** 호스트가 실제로 보내는지 확인하려면 먼저
	 * 받아야 한다.
	 *
	 *   U3L2L1SUSPEN  suspend 진입 (HS 에서는 L2)
	 *   WKUPEVTEN     remote wakeup
	 *   ULSTCNGEN     링크 상태 변화 — **resume 을 아는 유일한 경로**다.
	 *                 HS 에서 resume 은 별도 이벤트가 아니라 링크가 U0 로
	 *                 돌아오는 것으로 나타난다 (Linux dwc3 도
	 *                 linksts_change 로 처리한다)
	 *
	 * ⚠ ULSTCNGEN 은 빈번할 수 있다. 그래서 그 case 는 **로그를 찍지
	 *   않고 카운터만** 올린다 — 로그 폭주가 수신을 멈추게 한 전례가
	 *   있다(CDC hexdump).
	 *
	 * 1단계는 세기만 한다. UDC_EVT_SUSPEND/RESUME 을 스택에 올리는 것은
	 * 2단계다 — 호스트가 보내는 것이 확인된 뒤에.
	 */
	dwc3_write32(dev, DWC3_DEVTEN,
		     DWC3_DEVTEN_USBRSTEVTEN |
		     DWC3_DEVTEN_CONNECTDONEEVTEN |
		     DWC3_DEVTEN_DISSCONNEVTEN |
		     DWC3_DEVTEN_U3L2L1SUSPEN |
		     DWC3_DEVTEN_WKUPEVTEN |
		     DWC3_DEVTEN_ULSTCNGEN);

	/*
	 * 동작 속도와 디바이스 주소. 이 보드는 USB3 combphy 가 연결되지
	 * 않아 High Speed 로만 열거된다 (DT 의 maximum-speed, 그리고 실기
	 * 확인 — docs/hardware-verified.md).
	 * 주소는 0 으로 시작한다 — SET_ADDRESS 를 받기 전의 기본 상태다.
	 */
	dcfg = dwc3_read32(dev, DWC3_DCFG);
	dcfg &= ~(DWC3_DCFG_DEVSPD | DWC3_DCFG_DEVADDR);
	dcfg |= FIELD_PREP(DWC3_DCFG_DEVSPD, DWC3_DEVSPD_HIGH);
	dwc3_write32(dev, DWC3_DCFG, dcfg);

	/*
	 * EP0 을 쓸 수 있게 만들고 SETUP 을 받을 준비까지 한 뒤에 버스에
	 * 붙는다. 순서가 거꾸로면 호스트의 첫 SETUP 을 놓친다.
	 *
	 * IRQ 연결은 이 둘 '뒤' 다. 이유가 둘 있다:
	 *   - ISR 이 USB_RESET 에서 EP0 명령을 내므로, 설정 중에 인터럽트가
	 *     들어오면 같은 EP 에 명령이 겹칠 수 있다
	 *   - 실패 시 IRQ 가 연결된 채로 남지 않는다. udc_enable 이 실패하면
	 *     보통 disable 이 불리지 않아 되돌릴 기회가 없고, 재시도하면
	 *     irq_connect 가 두 번 불린다
	 * RUN_STOP 전에는 버스 이벤트가 없으므로 인터럽트를 놓치지 않는다.
	 */
	/*
	 * EP0 상태를 초기화한다. preinit 과 USB_RESET 에서만 하면,
	 * 전송 중 disable 후 다시 enable 했을 때 낡은 상태로 시작해
	 * 첫 SETUP 이 데이터 완료로 오해된다.
	 */
	priv->ep0_state = DWC3_EP0_SETUP;
	priv->ep0_nrdy_status = false;
	priv->ep0_xfer_len = 0U;
	priv->ep0_wlength = 0U;
	priv->started_cfg2 = false;
	memset(priv->ep, 0, sizeof(priv->ep));
	udc_ep_set_busy(udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT), false);
	udc_ep_set_busy(udc_get_ep_cfg(dev, USB_CONTROL_EP_IN), false);

	err = dwc3_ep0_configure(dev);
	if (err != 0) {
		goto err_devten;
	}

	err = dwc3_ep0_arm_setup(dev);
	if (err != 0) {
		goto err_devten;
	}

	/*
	 * ⚠ 공통 계층에 제어 엔드포인트가 켜졌다고 알린다.
	 *
	 *   이것이 없으면 udc_ep_enqueue() 가 cfg->stat.enabled 를 보고
	 *   -ENODEV 를 돌려준다. 하드웨어는 DALEPENA 로 켜져 있어도 스택은
	 *   엔드포인트가 없는 것으로 취급한다. 실기에서 정확히 이것으로
	 *   막혔다 (usbd_ch9 "Failed to enqueue SETUP buffer", -ENODEV).
	 *   참조: zephyr/drivers/usb/udc/udc_dwc2.c 의 udc_dwc2_enable.
	 *
	 *   udc_ep_enable_internal 은 api->ep_enable 을 되부르지만
	 *   udc_dwc3_ep_enable 이 EP0 을 통과시키므로 중복 설정은 없다.
	 */
	err = udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT,
				     USB_EP_TYPE_CONTROL, DWC3_EP0_MPS, 0);
	if (err != 0) {
		LOG_ERR("control EP OUT enable failed (%d)", err);
		goto err_devten;
	}

	err = udc_ep_enable_internal(dev, USB_CONTROL_EP_IN,
				     USB_EP_TYPE_CONTROL, DWC3_EP0_MPS, 0);
	if (err != 0) {
		LOG_ERR("control EP IN enable failed (%d)", err);
		goto err_devten;
	}

	config->irq_connect();

	dctl = dwc3_read32(dev, DWC3_DCTL);
	dwc3_write32(dev, DWC3_DCTL, dctl | DWC3_DCTL_RUN_STOP);

	LOG_INF("enabled (RUN_STOP set, DCFG=0x%08x)",
		dwc3_read32(dev, DWC3_DCFG));
	return 0;

err_devten:
	/* 켜 둔 것만 되돌린다. IRQ 는 아직 연결하지 않았다. */
	dwc3_write32(dev, DWC3_DEVTEN, 0U);
	dwc3_write32(dev, DWC3_DALEPENA, 0U);
	LOG_ERR("enable failed (%d) — DEVTEN/DALEPENA 되돌림", err);
	return err;
}

static int udc_dwc3_disable(const struct device *dev)
{
	uint32_t dctl;

	/* 역순 — 먼저 버스에서 떨어진다 */
	dctl = dwc3_read32(dev, DWC3_DCTL);
	dwc3_write32(dev, DWC3_DCTL, dctl & ~DWC3_DCTL_RUN_STOP);

	irq_disable(DT_INST_IRQN(0));
	dwc3_write32(dev, DWC3_DEVTEN, 0U);

	LOG_INF("disabled");
	return 0;
}

/*
 * udc_init() 에서 호출. [3] 에서 코어 소프트리셋 + PHY 시퀀스,
 * [4] 에서 이벤트 버퍼 할당과 IRQ 연결이 들어온다.
 */
/* ── 이벤트 버퍼 ──────────────────────────────────────────────────────
 * DWC3 는 dwc2 처럼 상태 레지스터를 폴링하지 않는다. 컨트롤러가 메모리의
 * 링 버퍼에 32비트 이벤트를 DMA 로 쓰고, GEVNTCOUNT 에 "대기 중인 바이트
 * 수"를 올린다. CPU 는 그만큼 소비한 뒤 같은 수를 GEVNTCOUNT 에 되써서
 * ack 한다.
 *
 * 이벤트 1개 = 4바이트. 256개면 리셋·연결 수준에는 충분하다.
 * 정렬은 캐시 라인(64B) 기준 — 컨트롤러가 DMA 로 쓰는 메모리라
 * 읽기 전에 반드시 캐시를 무효화해야 한다.
 */
#define DWC3_EVT_BUF_BYTES	1024U
#define DWC3_EVT_INTR_IDX	0U

static uint32_t dwc3_evt_buf[DWC3_EVT_BUF_BYTES / sizeof(uint32_t)]
	__aligned(64);

static const char *dwc3_devt_name(uint32_t type)
{
	switch (type) {
	case DWC3_DEVT_DISCONNECT:		return "DISCONNECT";
	case DWC3_DEVT_USB_RESET:		return "USB_RESET";
	case DWC3_DEVT_CONNECTION_DONE:		return "CONNECTION_DONE";
	case DWC3_DEVT_LINK_STATE_CHANGE:	return "LINK_STATE_CHANGE";
	case DWC3_DEVT_REMOTE_WAKEUP:		return "REMOTE_WAKEUP";
	case DWC3_DEVT_HIBERNATE_REQUEST:	return "HIBERNATE_REQ";
	case DWC3_DEVT_SUSPEND_ENTRY:		return "SUSPEND_ENTRY";
	case DWC3_DEVT_SOF:			return "SOF";
	case DWC3_DEVT_ERRATIC_ERROR:		return "ERRATIC_ERROR";
	case DWC3_DEVT_COMMAND_COMPLETE:	return "CMD_COMPLETE";
	case DWC3_DEVT_EVENT_BUF_OVERFLOW:	return "EVT_BUF_OVERFLOW";
	case DWC3_DEVT_STOPPED_DISCONNECT:	return "STOPPED_DISCONNECT";
	case DWC3_DEVT_L1_RESUME_DETECT:	return "L1_RESUME";
	default:				return "?";
	}
}

/*
 * 위쪽 절반(top half). 하는 일이 셋뿐이다:
 *   1) 처리할 이벤트가 있는지 본다
 *   2) 이벤트 인터럽트를 '마스크' 한다 — GEVNTCOUNT 가 0 이 아니면 레벨
 *      인터럽트가 계속 걸리므로, 스레드가 링을 비울 때까지 막아야 한다.
 *      이것이 EVNTINTRPTMASK 의 용도다.
 *   3) 스레드를 깨운다
 *
 * 이벤트를 두 번 복사하지 않는다. 링은 DMA 메모리에 그대로 두고 스레드가
 * 직접 읽는다. 그래서 유실도 없고 별도 큐도 필요 없다.
 */
static void udc_dwc3_isr(const struct device *dev)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t cnt;

	cnt = dwc3_read32(dev, DWC3_GEVNTCOUNT(DWC3_EVT_INTR_IDX)) &
	      DWC3_GEVNTCOUNT_EVNTCOUNT;
	if (cnt == 0U) {
		return;
	}

	dwc3_write32(dev, DWC3_GEVNTSIZ(DWC3_EVT_INTR_IDX),
		     DWC3_EVT_BUF_BYTES | DWC3_GEVNTSIZ_EVNTINTRPTMASK);

	k_event_post(&priv->drv_evt, BIT(DWC3_DRV_EVT_EVENTS));
}


/*
 * 이벤트 링을 컨트롤러에 알려 준다.
 * GEVNTADR 는 64비트 레지스터다. sys_write64 한 번은 버스에 따라 쪼개질 수
 * 있어, 하위/상위를 각각 32비트로 쓴다.
 */
static void dwc3_evt_buf_setup(const struct device *dev)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uintptr_t phys = k_mem_phys_addr(dwc3_evt_buf);

	priv->evt_pos = 0U;
	memset(dwc3_evt_buf, 0, sizeof(dwc3_evt_buf));
	sys_cache_data_flush_range(dwc3_evt_buf, sizeof(dwc3_evt_buf));

	dwc3_write32(dev, DWC3_GEVNTADR_LO(DWC3_EVT_INTR_IDX),
		     (uint32_t)(phys & 0xffffffffU));
	dwc3_write32(dev, DWC3_GEVNTADR_HI(DWC3_EVT_INTR_IDX),
		     (uint32_t)((uint64_t)phys >> 32));

	/*
	 * EVNTINTRPTMASK 는 '마스크' 라 1 이면 인터럽트가 오지 않는다.
	 * 크기는 바이트 수.
	 */
	dwc3_write32(dev, DWC3_GEVNTSIZ(DWC3_EVT_INTR_IDX), DWC3_EVT_BUF_BYTES);

	/* 남아 있던 카운트를 비운다 */
	dwc3_write32(dev, DWC3_GEVNTCOUNT(DWC3_EVT_INTR_IDX),
		     dwc3_read32(dev, DWC3_GEVNTCOUNT(DWC3_EVT_INTR_IDX)) &
		     DWC3_GEVNTCOUNT_EVNTCOUNT);

	LOG_INF("event ring: virt=%p phys=0x%lx size=%u",
		dwc3_evt_buf, (unsigned long)phys, DWC3_EVT_BUF_BYTES);
}

/* ── TRB 링 ───────────────────────────────────────────────────────────
 * DWC3 는 전송을 메모리 디스크립터(TRB)로 기술한다. dwc2 처럼 FIFO 에
 * 직접 읽고 쓰지 않는다 — 이것이 두 IP 의 가장 큰 구조 차이다.
 *
 * 지금은 EP0 만 다룬다. 물리 엔드포인트 번호는 EP0 OUT=0, EP0 IN=1 이고
 * 각각 TRB 2개면 SETUP + 여유로 충분하다. bulk 용 링과 LINK TRB 를 통한
 * 순환은 [8] 에서.
 *
 * 정렬: 하드웨어가 TRB 주소 하위 4비트 0 을 요구(구조체에 __aligned(16)),
 * 링 전체는 캐시 유지 단위에 맞춰 64바이트 정렬.
 */
#define DWC3_EP0_TRB_COUNT	2U

/*
 * ⚠ 방향(OUT/IN)별 링이 캐시 라인을 '공유하지 않아야' 한다.
 *
 * TRB 16바이트 × 2개 = 32바이트라, 그냥 2차원 배열로 두면 OUT 행과 IN 행이
 * 64바이트 캐시 라인 하나를 같이 쓴다. 이 SoC 의 DMA 는 캐시 코히런트하지
 * 않고, Zephyr 의 arm64 `arch_dcache_range(K_CACHE_INVD)` 는 부분 라인에도
 * `dc ivac` 를 그대로 적용한다. 그러면:
 *   - OUT TRB 를 invalidate 할 때 같은 라인의 IN 쪽 dirty 데이터가 버려진다
 *   - IN TRB 를 flush 할 때 OUT TRB 의 낡은 내용(HWO=1)이 하드웨어가 갱신한
 *     값을 덮어쓴다
 * 지금은 OUT[0] 만 쓰므로 터지지 않지만, IN 경로가 들어오는 [7b] 에서
 * 간헐적으로 깨진다. 구조체에 캐시 라인 정렬을 주면 sizeof 가 라인 크기의
 * 배수로 올림되므로, 배열 원소마다 자기 라인을 단독 점유한다.
 */
struct dwc3_trb_ring {
	struct dwc3_trb trb[DWC3_EP0_TRB_COUNT];
} __aligned(CONFIG_DCACHE_LINE_SIZE);

BUILD_ASSERT(sizeof(struct dwc3_trb_ring) % CONFIG_DCACHE_LINE_SIZE == 0,
	     "TRB ring must occupy whole cache lines");

/*
 * 물리 엔드포인트마다 링 하나. 물리 번호 = (ep_num << 1) | is_in 이므로
 * USB 엔드포인트 N 개면 물리 2N 개다.
 *
 * 한 엔드포인트에 한 번에 전송 하나만 건다 (TRB 체인·스캐터개더 없음).
 * Zephyr 의 net_buf 는 연속 메모리를 주므로 TRB 하나로 충분하고,
 * udc_ep_is_busy() 가 큐를 직렬화한다. 성능이 필요해지면 LINK TRB 로
 * 순환 링을 만들고 DEPUPDXFER 로 '진행 중 전송' 전략을 쓰면 된다.
 */
static struct dwc3_trb_ring dwc3_ep_trb[DWC3_NUM_PHYS_EPS];

/* 물리 엔드포인트의 TRB. EP0 OUT=0, EP0 IN=1 이 그대로 들어맞는다. */
#define DWC3_EP_TRB(phys)	(&dwc3_ep_trb[phys].trb[0])
/* 기존 EP0 코드가 쓰는 이름 — 같은 것을 가리킨다 */
#define DWC3_EP0_TRB(dir, i)	(&dwc3_ep_trb[dir].trb[i])

/*
 * TRB 를 채운다.
 *
 * HWO(Hardware Owner) 비트가 핵심이다. 이 비트를 세우는 순간 하드웨어가
 * 디스크립터를 가져갈 수 있으므로, 나머지 필드가 메모리에 먼저 보여야 한다.
 * 그래서 ptr/status 를 쓰고 flush 한 뒤, control(HWO 포함)을 쓰고 다시
 * flush 한다. 순서가 뒤집히면 하드웨어가 반쪽짜리 TRB 를 읽는다.
 */
static void dwc3_trb_fill(struct dwc3_trb *trb, uintptr_t buf_phys,
			  uint32_t len, uint32_t ctl)
{
	trb->ptr_low  = (uint32_t)(buf_phys & 0xffffffffU);
	trb->ptr_high = (uint32_t)((uint64_t)buf_phys >> 32);
	trb->status   = FIELD_PREP(DWC3_TRB_BUFSIZ, len);

	/* HWO 를 세우기 전에 위 세 필드가 메모리에 보여야 한다 */
	sys_cache_data_flush_range(trb, sizeof(*trb));

	trb->control = ctl | DWC3_TRB_HWO;
	sys_cache_data_flush_range(trb, sizeof(*trb));
}

/*
 * 하드웨어가 TRB 를 돌려줬는지 확인한다.
 * 하드웨어가 DMA 로 갱신한 내용을 보려면 읽기 전에 무효화해야 한다.
 * 반환: HWO 가 내려갔으면 0 과 함께 *status 에 TRB status 원시값,
 *       아직 하드웨어 소유면 -EBUSY.
 */
static int dwc3_trb_reclaim(struct dwc3_trb *trb, uint32_t *status)
{
	sys_cache_data_invd_range(trb, sizeof(*trb));

	if ((trb->control & DWC3_TRB_HWO) != 0U) {
		return -EBUSY;
	}

	if (status != NULL) {
		*status = trb->status;
	}
	return 0;
}

/* ── DEPCMD 명령 프리미티브 ───────────────────────────────────────────
 * DWC3 의 엔드포인트 제어는 레지스터 직접 쓰기가 아니라 명령 큐다.
 * 이것이 dwc2 와 가장 크게 갈리는 지점 중 하나다.
 *
 *   PAR2/PAR1/PAR0 에 인자를 넣는다  (주의: 주소 순서가 2,1,0 이다)
 *   DEPCMD 에 CMDTYP | CMDACT 를 쓴다
 *   하드웨어가 CMDACT 를 스스로 클리어할 때까지 기다린다
 *   CMDSTATUS(15:12) 에 결과가 남는다
 *
 * CMDSTATUS 의 성공/실패 코드값은 databook 영역이라 아직 모른다. Fuchsia
 * 드라이버도 CMDSTATUS 를 검사하지 않고 CMDACT 만 기다린다. 그래서 이
 * 함수도 성공 판정을 하지 않고 '원시 상태값'을 그대로 돌려준다 —
 * 실기 로그에서 값을 확인한 뒤 판정 로직을 넣는다.
 *
 * ⚠ PHY 클럭 게이팅 우회가 필수다. 엔드포인트 명령을 내는 동안
 *   GUSB2PHYCFG 의 SUSPENDUSB20 / ENBLSLPM 이 켜져 있으면 USB 2.0 PHY
 *   클럭이 게이팅돼 명령이 완료되지 않을 수 있다. 명령 전에 두 비트를
 *   지우고 끝난 뒤 원래 값으로 되돌린다.
 *   근거: Synopsys DWC_usb3 Programming Guide 3.2.2
 *   (Fuchsia dwc3-commands.cc 의 CmdEpEndTransfer 가 같은 처리를 한다)
 *
 * CMDIOC 는 기본으로 켜지 않는다 — CMDACT 를 동기로 기다리므로 완료
 * 이벤트가 중복이고 이벤트 링만 어지럽힌다.
 *
 * ⚠ 예외가 하나 있다. 일반 엔드포인트의 DEPSTRTXFER 는 CMDIOC 를 켠다.
 *   DEPENDXFER 에 필요한 전송 리소스 인덱스(rsrc_id)가 그 명령의
 *   CMD_CMPLT 이벤트로만 오기 때문이다 (dwc3_handle_cmd_cmplt).
 *   CMDIOC 등 추가 비트가 필요한 호출자는 dwc3_dep_cmd_ioc 를 쓴다.
 *
 * 동시성: 이 함수는 '항상 스레드 문맥에서, UDC 락을 잡은 상태로' 불린다.
 *   - udc_dwc3_enable() — 스택이 락을 잡고 부른다
 *   - dwc3_thread_handler() — 스스로 udc_lock_internal() 을 잡는다
 *   - ep_set_halt / ep_clear_halt / ep_dequeue — 스택이 락을 잡고 부른다
 * ISR 은 이 함수를 부르지 않는다 ([7b] 에서 이벤트 처리를 스레드로
 * 옮겼다 — udc_setup_received() 가 스레드 문맥 전용인 것이 그 계기였고,
 * 덤으로 ISR 에서 DEPCMD 를 동기 대기하던 문제도 없어졌다).
 * 따라서 같은 EP 에 명령이 겹칠 수 없고, dwc3_ep0_trb /
 * dwc3_ep0_setup_buf 접근도 락으로 직렬화된다.
 */
#define DWC3_DEPCMD_TIMEOUT_US	5000

/*
 * extra 는 DEPCMD 에 추가로 OR 할 비트다 — CMDIOC(완료 이벤트 요청),
 * HIPRI_FORCERM(강제 제거), COMMANDPARAM(리소스 ID 등).
 */
static int dwc3_dep_cmd_ioc(const struct device *dev, uint8_t ep,
			    uint32_t cmdtyp, uint32_t par0, uint32_t par1,
			    uint32_t par2, uint32_t extra, uint32_t *status)
{
	uint32_t cmd;
	uint32_t phycfg_saved;
	bool phycfg_restore;
	int ret = -ETIMEDOUT;

	/* PHY 클럭이 게이팅된 상태로 명령을 내면 완료되지 않는다 (위 주석) */
	phycfg_saved = dwc3_read32(dev, DWC3_GUSB2PHYCFG(0));
	phycfg_restore = (phycfg_saved & (DWC3_GUSB2PHYCFG_SUSPENDUSB20 |
					  DWC3_GUSB2PHYCFG_ENBLSLPM)) != 0U;
	if (phycfg_restore) {
		dwc3_write32(dev, DWC3_GUSB2PHYCFG(0),
			     phycfg_saved & ~(DWC3_GUSB2PHYCFG_SUSPENDUSB20 |
					      DWC3_GUSB2PHYCFG_ENBLSLPM));
	}

	dwc3_write32(dev, DWC3_DEPCMDPAR2(ep), par2);
	dwc3_write32(dev, DWC3_DEPCMDPAR1(ep), par1);
	dwc3_write32(dev, DWC3_DEPCMDPAR0(ep), par0);

	cmd = FIELD_PREP(DWC3_DEPCMD_CMDTYP, cmdtyp) | DWC3_DEPCMD_CMDACT |
	      extra;
	dwc3_write32(dev, DWC3_DEPCMD(ep), cmd);

	for (int us = 0; us < DWC3_DEPCMD_TIMEOUT_US; us += 10) {
		uint32_t val = dwc3_read32(dev, DWC3_DEPCMD(ep));

		if ((val & DWC3_DEPCMD_CMDACT) == 0U) {
			uint32_t st = FIELD_GET(DWC3_DEPCMD_CMDSTATUS, val);

			if (status != NULL) {
				*status = st;
			}
			LOG_DBG("ep%u cmd %u done in <%d us, status=0x%x",
				ep, cmdtyp, us + 10, st);
			ret = 0;
			break;
		}
		/*
		 * udc 락을 잡은 채로 불리므로 k_sleep 이 아니라 busy wait 다.
		 * 여기서 자면 데드락 후보가 된다.
		 */
		k_busy_wait(10);
	}

	if (ret != 0) {
		LOG_ERR("ep%u cmd %u timed out (DEPCMD=0x%08x)", ep, cmdtyp,
			dwc3_read32(dev, DWC3_DEPCMD(ep)));
	}

	if (phycfg_restore) {
		dwc3_write32(dev, DWC3_GUSB2PHYCFG(0), phycfg_saved);
	}

	if (ret == -ETIMEDOUT) {
		struct udc_dwc3_data *p = udc_get_private(dev);

		p->stats.cmd_timeout++;
	}

	return ret;
}

/* CMDIOC 등 추가 비트가 필요 없는 흔한 경우 */
static int dwc3_dep_cmd(const struct device *dev, uint8_t ep,
			uint32_t cmdtyp, uint32_t par0, uint32_t par1,
			uint32_t par2, uint32_t *status)
{
	return dwc3_dep_cmd_ioc(dev, ep, cmdtyp, par0, par1, par2, 0U, status);
}

/*
 * 전송을 끊는다.
 *
 * DEPENDXFER 는 전송 리소스 인덱스를 COMMANDPARAM 으로 받는다 — 그 값이
 * 없으면 끊을 수 없다. rsrc_id 는 DEPSTRTXFER 의 CMD_CMPLT 이벤트에서만
 * 온다 (dwc3_handle_cmd_cmplt).
 *
 * HIPRI_FORCERM 은 '강제 제거' 다. CMDIOC 를 켜면 완료가 이벤트로 오고,
 * 끄면 오지 않는다 — 끄는 경우에는 하드웨어가 진행 중인 DMA 버스트를
 * 끝낼 시간을 줘야 하므로 1 ms 기다린 뒤 DALEPENA 를 지우거나 메모리를
 * 놓아야 한다.
 */
static int dwc3_ep_end_xfer(const struct device *dev, uint8_t phys,
			    bool want_event)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t extra, st;
	int err;

	if (!priv->ep[phys].rsrc_valid) {
		/*
		 * ⚠ 경합 창이 있다.
		 *   DEPSTRTXFER 는 동기로 끝나지만 그 CMD_CMPLT 이벤트(= 유일한
		 *   rsrc_id 출처)는 링에 쌓였다가 다음 스레드 실행 때 처리된다.
		 *   스레드가 락을 놓은 사이에 스택이 ep_disable / ep_dequeue /
		 *   ep_set_halt 를 부르면 여기로 들어온다.
		 *
		 *   그냥 생략하면 하드웨어 전송이 살아 있는데 호출자가 net_buf
		 *   를 풀에 반환해 버리고, 이후 DMA 가 반환된 버퍼에 쓴다.
		 *   그래서 밀린 이벤트를 먼저 소화해 rsrc_id 를 받아 온다.
		 *   (우리는 락을 쥐고 있고, 이 시점에 ep 는 아직 busy 라
		 *    dwc3_ep_xfer_next 가 새 전송을 시작하지는 않는다.)
		 */
		dwc3_handle_events(dev);
	}

	if (!priv->ep[phys].rsrc_valid) {
		/*
		 * 이벤트를 소화해도 없다 = 애초에 활성 전송이 없었거나,
		 * 전송이 그 사이에 완료된 것이다. 전자라면 끊을 것이 없고
		 * 후자라면 이미 끝났다. 둘 다 정상이다.
		 */
		LOG_DBG("ep%u DEPENDXFER 생략 — 활성 전송 없음", phys);
		return 0;
	}

	extra = DWC3_DEPCMD_HIPRI_FORCERM |
		FIELD_PREP(DWC3_DEPCMD_COMMANDPARAM, priv->ep[phys].rsrc_id);
	if (want_event) {
		extra |= DWC3_DEPCMD_CMDIOC;
	}

	err = dwc3_dep_cmd_ioc(dev, phys, DWC3_DEPCMD_DEPENDXFER,
			       0U, 0U, 0U, extra, &st);
	if (err != 0) {
		LOG_ERR("ep%u DEPENDXFER failed (%d)", phys, err);
		return err;
	}

	priv->ep[phys].canceling = want_event;
	priv->ep[phys].rsrc_valid = false;

	if (!want_event) {
		/* 완료 이벤트가 없으므로 DMA 가 멎을 시간을 직접 준다 */
		k_busy_wait(1000);
	}

	LOG_DBG("ep%u DEPENDXFER rsrc=%u status=0x%x", phys,
		priv->ep[phys].rsrc_id, st);
	return 0;
}

/* ── EP0 ──────────────────────────────────────────────────────────────
 * 제어 엔드포인트를 쓸 수 있는 상태로 만든다. 순서가 databook 규정이다:
 *
 *   1) DEPSTARTCFG(base=0)  — 전송 리소스 할당을 초기화. EP0/EP1 을
 *      설정하기 전에는 베이스가 0 이고, EP2 이상을 설정할 때는 2 다.
 *   2) DEPCFG  × 2          — EP0 OUT(물리 0) 과 EP0 IN(물리 1)
 *   3) DEPXFERCFG × 2       — 각 EP 에 전송 리소스 1개
 *   4) DALEPENA              — 두 EP 를 활성화
 *
 * 이 함수는 enable 시점과 USB 리셋을 받을 때마다 다시 불린다. 리셋은
 * 컨트롤러의 엔드포인트 상태를 날리기 때문이다.
 */

static int dwc3_ep0_configure(const struct device *dev)
{
	static const uint8_t eps[2] = { DWC3_EP0_OUT, DWC3_EP0_IN };
	uint32_t st;
	int err;

	err = dwc3_dep_cmd(dev, DWC3_EP0_OUT, DWC3_DEPCMD_DEPSTARTCFG,
			   0U, 0U, 0U, &st);
	if (err != 0) {
		LOG_ERR("EP0 DEPSTARTCFG failed (%d)", err);
		return err;
	}
	/*
	 * ⚠ 아래 status 들은 LOG_INF 다. CMDSTATUS 의 성공 코드값을 아직
	 *   모르므로(위 dwc3_dep_cmd 주석) 코드가 성공/실패를 판정할 수
	 *   없다. 즉 하드웨어가 명령을 거절해도 이 함수는 0 을 돌려준다.
	 *   첫 실기 구동에서 사람이 값을 보고 판단해야 하므로, 기본 로그
	 *   레벨에서 보이게 둔다. 값을 확인한 뒤 판정 로직을 넣고 레벨을
	 *   DBG 로 내린다.
	 */
	LOG_INF("EP0 DEPSTARTCFG status=0x%x", st);

	for (int i = 0; i < 2; i++) {
		uint8_t ep = eps[i];
		uint32_t par0, par1;

		/*
		 * FIFO_NUM 은 OUT 엔드포인트와 EP0-IN 에서 0 이다 (그 외는
		 * ep_num >> 1). EP0 은 둘 다 0 이므로 명시하지 않아도 되지만,
		 * 규칙을 잊지 않도록 0 을 적어 둔다.
		 *
		 * BURST_SIZE 는 '실제값 - 1' 이라 0 = 버스트 1 이다.
		 * High Speed 에는 버스트 개념이 없으므로 0 이 맞다.
		 */
		par0 = FIELD_PREP(DWC3_DEPCFG_PAR0_ACTION,
				  DWC3_DEPCFG_ACTION_INITIALIZE) |
		       FIELD_PREP(DWC3_DEPCFG_PAR0_FIFO_NUM, 0U) |
		       FIELD_PREP(DWC3_DEPCFG_PAR0_MAX_PACKET, DWC3_EP0_MPS) |
		       FIELD_PREP(DWC3_DEPCFG_PAR0_EP_TYPE,
				  DWC3_DEPCFG_EP_TYPE_CONTROL);

		/*
		 * XFER_CMPL_EN / XFER_NRDY_EN 을 켠다 — 완료와 '다음 단계
		 * 준비됨' 을 둘 다 받아야 제어 전송을 진행할 수 있다.
		 * XFER_IP_EN 은 TRB 를 여러 개 걸 때만 의미가 있어 끈다.
		 * INTR_NUM 0 = 우리가 쓰는 유일한 이벤트 버퍼.
		 */
		par1 = FIELD_PREP(DWC3_DEPCFG_PAR1_EP_NUMBER, ep) |
		       DWC3_DEPCFG_PAR1_XFER_CMPL_EN |
		       DWC3_DEPCFG_PAR1_XFER_NRDY_EN |
		       FIELD_PREP(DWC3_DEPCFG_PAR1_INTR_NUM, 0U);

		err = dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPCFG,
				   par0, par1, 0U, &st);
		if (err != 0) {
			LOG_ERR("ep%u DEPCFG failed (%d)", ep, err);
			return err;
		}
		LOG_INF("ep%u DEPCFG par0=0x%08x par1=0x%08x status=0x%x",
			ep, par0, par1, st);
	}

	for (int i = 0; i < 2; i++) {
		uint8_t ep = eps[i];

		err = dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPXFERCFG,
				   FIELD_PREP(DWC3_DEPXFERCFG_NUM_XFER_RES, 1U),
				   0U, 0U, &st);
		if (err != 0) {
			LOG_ERR("ep%u DEPXFERCFG failed (%d)", ep, err);
			return err;
		}
		LOG_INF("ep%u DEPXFERCFG status=0x%x", ep, st);
	}

	/* 두 물리 엔드포인트만 활성화한다. 나머지는 [8] 에서. */
	dwc3_write32(dev, DWC3_DALEPENA,
		     BIT(DWC3_EP0_OUT) | BIT(DWC3_EP0_IN));

	/*
	 * "configured" 는 '명령이 타임아웃 없이 끝났다' 는 뜻일 뿐이다.
	 * 실제 성공 여부는 위 status 값들을 봐야 한다.
	 */
	LOG_INF("EP0 commands issued (mps=%u, DALEPENA=0x%08x) "
		"— 성공 판정은 위 status 확인 필요",
		DWC3_EP0_MPS, dwc3_read32(dev, DWC3_DALEPENA));
	return 0;
}

/*
 * SETUP 패킷을 받을 준비를 한다.
 *
 * 호스트가 8바이트 SETUP 을 보내면 컨트롤러가 이 버퍼로 DMA 하고
 * XferComplete 이벤트를 올린다. 제어 전송의 모든 시작점이다.
 *
 * 버퍼를 64바이트로 잡은 이유: 컨트롤러가 DMA 로 쓰는 메모리를 읽기 전에
 * 캐시를 무효화해야 하는데, 무효화 단위가 캐시 라인(이 SoC 는 64바이트)
 * 이다. 8바이트만 잡으면 같은 라인에 있는 다른 변수가 함께 날아간다.
 */
static uint8_t dwc3_ep0_setup_buf[64] __aligned(64);

/*
 * EP0 의 단일 TRB 전송을 시작한다. 제어 전송의 모든 단계가 이 한 함수를
 * 쓴다 — 단계마다 TRBCTL 과 방향(물리 EP)만 다르다.
 *
 * len 0 (ZLP·STATUS 단계) 도 정상이다. 그때도 TRB 는 필요하다.
 */
/*
 * EP0 의 진행 중 전송을 끊는다 — 리셋 경로 전용.
 *
 * ⚠ dwc3_ep_end_xfer() 를 쓰면 안 된다. 그 함수는 rsrc_valid 가 없을 때
 *   dwc3_handle_events() 를 되부르는데, 리셋 핸들러는 그 함수 '안' 이라
 *   재귀가 된다. 여기서는 받아 둔 rsrc_id 가 있을 때만 명령을 낸다.
 *
 * rsrc_id 가 없으면 애초에 활성 전송이 없었거나 이미 끝난 것이므로 끊을
 * 것이 없다.
 */
static void dwc3_ep0_end_xfer(const struct device *dev, uint8_t phys)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t extra, st;

	if (phys >= DWC3_NUM_PHYS_EPS || !priv->ep[phys].rsrc_valid) {
		return;
	}

	extra = DWC3_DEPCMD_HIPRI_FORCERM |
		FIELD_PREP(DWC3_DEPCMD_COMMANDPARAM, priv->ep[phys].rsrc_id);

	(void)dwc3_dep_cmd_ioc(dev, phys, DWC3_DEPCMD_DEPENDXFER,
			       0U, 0U, 0U, extra, &st);
	priv->ep[phys].rsrc_valid = false;
	LOG_DBG("ep%u DEPENDXFER (리셋)", phys);
}

static int dwc3_ep0_start(const struct device *dev, uint8_t ep,
			  uintptr_t buf_phys, uint32_t len, uint32_t trbctl)
{
	struct dwc3_trb *trb = DWC3_EP0_TRB(ep, 0);
	uintptr_t trb_phys = k_mem_phys_addr(trb);
	uint32_t st;
	int err;

	/*
	 *   IOC — 완료되면 인터럽트를 받는다
	 *   LST — 이 TRB 가 링의 마지막. 하드웨어가 다음을 가져오지 않는다.
	 */
	dwc3_trb_fill(trb, buf_phys, len,
		      trbctl | DWC3_TRB_IOC | DWC3_TRB_LST);

	/*
	 * ⚠ CMDIOC 를 켠다 — DEPENDXFER 에 필요한 전송 리소스 인덱스
	 *   (rsrc_id)가 이 명령의 CMD_CMPLT 이벤트로만 오기 때문이다.
	 *
	 *   한때 EP0 은 "CMDACT 를 동기로 기다리니 완료 이벤트가 중복" 이라며
	 *   켜지 않았다. 그 결과 리셋 때 EP0 의 진행 중 전송을 DEPENDXFER 로
	 *   끊을 수 없었고, 호스트가 부분적으로 소비한 TRB 가 컨트롤러 시야에
	 *   HWO=1 로 남았다 — 실기의 "TRB still HW-owned" 와 무장 불가 창이
	 *   그것이다.
	 *
	 *   Linux 의 dwc3_gadget_reset_interrupt() 도 재무장 '전에'
	 *   dwc3_ep0_reset_state() -> dwc3_ep0_end_control_data() 로 EP0
	 *   양방향에 DEPENDXFER 를 낸다. 그 단계를 우리가 빼먹고 있었다.
	 */
	err = dwc3_dep_cmd_ioc(dev, ep, DWC3_DEPCMD_DEPSTRTXFER,
			       DWC3_DEPSTRTXFER_PAR0(trb_phys),
			       DWC3_DEPSTRTXFER_PAR1(trb_phys), 0U,
			       DWC3_DEPCMD_CMDIOC, &st);
	if (err != 0) {
		LOG_ERR("ep%u DEPSTRTXFER failed (%d)", ep, err);
		return err;
	}

	/*
	 * EP0 은 rsrc_id 를 보관하지 않는다 — 제어 전송은 DEPENDXFER 로
	 * 끊는 경로가 없고(stall 과 SETUP 재무장으로 되돌린다) CMDIOC 도
	 * 켜지 않아 CMD_CMPLT 이벤트가 오지 않는다. 일반 엔드포인트는
	 * 다르다 (dwc3_ep_xfer_next 참고).
	 */
	/*
	 * 컨트롤러에 넘긴 '물리' 주소를 함께 남긴다. DMA 가 쓰는 주소와
	 * 우리가 읽는 주소가 같은지는 EP0 SETUP 수신 문제를 추적할 때
	 * 핵심 단서다 (docs/hardware-verified.md 참고).
	 */
	LOG_DBG("ep%u xfer: trbctl=%u len=%u trb_virt=%p trb_phys=0x%lx buf_phys=0x%lx st=0x%x",
		ep, trbctl >> 4, len, (void *)trb,
		(unsigned long)trb_phys, (unsigned long)buf_phys, st);
	return 0;
}

static int dwc3_ep0_arm_setup(const struct device *dev)
{
	uintptr_t buf_phys = k_mem_phys_addr(dwc3_ep0_setup_buf);

	memset(dwc3_ep0_setup_buf, 0, sizeof(dwc3_ep0_setup_buf));
	sys_cache_data_flush_range(dwc3_ep0_setup_buf,
				   sizeof(dwc3_ep0_setup_buf));

	return dwc3_ep0_start(dev, DWC3_EP0_OUT, buf_phys, 8U,
			      DWC3_TRBCTL_SETUP);
}

/* 제어 엔드포인트를 stall 시키고 다음 SETUP 을 기다린다. */
static void dwc3_ep0_stall_and_restart(const struct device *dev, uint8_t ep)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t st;

	(void)dwc3_dep_cmd(dev, ep, DWC3_DEPCMD_DEPSSTALL, 0U, 0U, 0U, &st);

	priv->ep0_state = DWC3_EP0_SETUP;
	priv->ep0_nrdy_status = false;
	(void)dwc3_ep0_arm_setup(dev);
}

/*
 * SETUP 8바이트가 도착했다. 해독해 UDC 스택에 올린다.
 *
 * 스택은 이 호출에 응답해 데이터 단계와 상태 단계의 버퍼를 ep_enqueue 로
 * 내려 준다. 즉 여기서 전송을 시작하지 않는다 — 버퍼가 올 때 시작한다.
 *
 * ⚠ 스레드 문맥에서만 불려야 한다 (udc_setup_received 가 UDC 락에 의존).
 */

static void dwc3_ep0_setup_received(const struct device *dev, uint32_t raw)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	struct dwc3_trb *trb = DWC3_EP0_TRB(DWC3_EP0_OUT, 0);
	uint32_t trbsts = 0U;
	const uint8_t *b = dwc3_ep0_setup_buf;
	int rc;

	priv->stats.setup++;

	/*
	 * ⚠ 200 us 폴링을 시도했으나 무용했다 — write-back 경합이 아니다.
	 *   실기에서 이 경로는 36번 중 2번만 타고, 그때는 리셋 처리가
	 *   TRB 를 재무장해 덮어쓴 '낡은 이벤트' 다. 흘려보내면 호스트가
	 *   재시도한다.
	 */
	rc = dwc3_trb_reclaim(trb, &trbsts);
	if (rc != 0) {
		/*
		 * ⚠ [임시 계측] 두 가능성을 가린다:
		 *   (a) 이 TRB 의 완료인데 write-back 이 이벤트와 경합 —
		 *       raw 의 상태 비트가 '전송했다' 고 말하면 이쪽
		 *   (b) 슬롯을 잘못 읽는다 — EP0 링은 TRB 가 2개다
		 *       (DWC3_EP0_TRB_COUNT). trb[1] 의 HWO 가 0 이고
		 *       BUFSIZ < 8 이면 그쪽에 SETUP 이 들어온 것
		 */
		struct dwc3_trb *t1 = DWC3_EP0_TRB(DWC3_EP0_OUT, 1);

		sys_cache_data_invd_range(t1, sizeof(*t1));
		sys_cache_data_invd_range(dwc3_ep0_setup_buf,
					  sizeof(dwc3_ep0_setup_buf));
		LOG_WRN("EP0 XferComplete but TRB still HW-owned");
		priv->stats.trb_hw_owned++;
		LOG_WRN("  depevt=0x%08x  trb0 ctl=0x%08x st=0x%08x  trb1 ctl=0x%08x st=0x%08x",
			raw, trb->control, trb->status, t1->control, t1->status);
		LOG_WRN("  buf=%02x %02x %02x %02x %02x %02x %02x %02x",
			b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
		/*
		 * ⚠ 이웃 링까지 본다 — write-back 이 인접한 다른 슬롯에
		 *   떨어졌을 수 있다 (링 stride 오산 등).
		 */
		{
			struct dwc3_trb *n = DWC3_EP0_TRB(DWC3_EP0_IN, 0);

			sys_cache_data_invd_range(dwc3_ep_trb,
						  sizeof(dwc3_ep_trb));
			LOG_WRN("  ep1ring[0] ctl=0x%08x st=0x%08x ptr=%08x:%08x",
				n->control, n->status, n->ptr_high, n->ptr_low);
			LOG_WRN("  trb0 ptr=%08x:%08x (무장 때 준 주소와 같은가)",
				trb->ptr_high, trb->ptr_low);
		}
		return;
	}

	/* 컨트롤러가 DMA 로 쓴 버퍼다. 읽기 '전에' 무효화해야 한다. */
	sys_cache_data_invd_range(dwc3_ep0_setup_buf,
				  sizeof(dwc3_ep0_setup_buf));

	memcpy(priv->setup, b, sizeof(priv->setup));
	priv->ep0_wlength = sys_get_le16(&b[6]);
	priv->ep0_nrdy_status = false;

	LOG_DBG("SETUP %02x %02x wValue=0x%04x wIndex=0x%04x wLength=%u",
		b[0], b[1], sys_get_le16(&b[2]), sys_get_le16(&b[4]),
		priv->ep0_wlength);

	/*
	 * wLength 0 이면 데이터 단계가 없는 2단계 전송이다. 이 구분이
	 * 상태 단계의 TRBCTL 을 가른다 — 2단계는 STATUS_2, 3단계는
	 * STATUS_3 이고 시작하는 방향도 다르다.
	 */
	/*
	 * ⚠ 여기서 SETUP 상태로 되돌리면 안 된다. 디스패처는
	 *   'EP0 OUT 의 XferComplete + state==SETUP' 을 SETUP 패킷 수신으로
	 *   해석하므로, 3단계 control-write 의 데이터 OUT 완료가 SETUP 으로
	 *   오해된다. 스택의 버퍼를 기다리는 별도 상태를 쓴다.
	 */
	priv->ep0_state = (priv->ep0_wlength == 0U) ?
			  DWC3_EP0_TWO_STAGE : DWC3_EP0_DATA_WAIT;

	udc_setup_received(dev, priv->setup);
}

/*
 * 데이터 또는 상태 단계가 끝났다. 실제 전송량을 계산해 버퍼에 반영하고
 * 스택에 완료를 알린다.
 *
 * ⚠ 버퍼를 큐에서 바로 빼지 않는다 (peek). IN 방향에서 ZLP 를 한 번 더
 *   보내야 하는 경우, 같은 버퍼로 전송을 다시 시작해야 하기 때문이다.
 *   진짜로 끝났을 때만 udc_buf_get 으로 빼낸다.
 */
static void dwc3_ep0_xfer_complete(const struct device *dev, uint8_t ep)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	struct dwc3_trb *trb = DWC3_EP0_TRB(ep, 0);
	uint8_t ep_addr = (ep == DWC3_EP0_IN) ? USB_CONTROL_EP_IN :
					        USB_CONTROL_EP_OUT;
	struct udc_ep_config *ep_cfg = udc_get_ep_cfg(dev, ep_addr);
	struct udc_buf_info *bi;
	struct net_buf *buf;
	uint32_t trbsts = 0U;
	uint32_t remaining, done;

	if (dwc3_trb_reclaim(trb, &trbsts) != 0) {
		LOG_WRN("ep%u XferComplete but TRB still HW-owned", ep);
		priv->stats.trb_hw_owned++;
		return;
	}

	/*
	 * 새 SETUP 이 이미 와 있다는 하드웨어 신호. 진행 중이던 전송은
	 * 의미가 없어졌으므로 버리고 SETUP 부터 다시 받는다.
	 * (제어 엔드포인트 stall 은 SETUP 수신으로 자동 해제된다)
	 */
	if (FIELD_GET(DWC3_TRB_TRBSTS, trbsts) ==
	    DWC3_TRB_TRBSTS_SETUP_PENDING) {
		LOG_DBG("ep%u SETUP pending — 진행 중 전송 포기", ep);
		udc_ep_set_busy(ep_cfg, false);
		priv->ep0_state = DWC3_EP0_SETUP;
		priv->ep0_nrdy_status = false;
		(void)dwc3_ep0_arm_setup(dev);
		return;
	}

	buf = udc_buf_peek(ep_cfg);
	if (buf == NULL) {
		LOG_WRN("ep%u XferComplete with no queued buffer", ep);
		priv->stats.no_queued_buf++;
		udc_ep_set_busy(ep_cfg, false);
		return;
	}

	bi = udc_get_buf_info(buf);
	remaining = FIELD_GET(DWC3_TRB_BUFSIZ, trbsts);
	done = (remaining > priv->ep0_xfer_len) ? 0U :
	       (priv->ep0_xfer_len - remaining);

	if (bi->status) {
		/* 상태 단계 완료 = 제어 전송 하나가 끝났다 */
		LOG_DBG("ep%u STATUS done", ep);
		buf = udc_buf_get(ep_cfg);
		udc_ep_set_busy(ep_cfg, false);
		priv->ep0_state = DWC3_EP0_SETUP;
		priv->ep0_nrdy_status = false;
		udc_submit_ep_event(dev, buf, 0);
		(void)dwc3_ep0_arm_setup(dev);
		return;
	}

	if (ep == DWC3_EP0_IN && udc_ep_buf_has_zlp(buf)) {
		/*
		 * 길이 0 패킷을 한 번 더 보내야 한다.
		 *
		 * 조건은 'wLength 보다 적게 보냈고 보낸 양이 MPS 의 배수' 다 —
		 * 그러면 호스트가 전송이 끝났는지 알 수 없으므로 짧은(길이 0)
		 * 패킷으로 끝을 알린다. 이 조건은 UDC 스택이 이미 계산해
		 * 플래그로 알려 준다 (usbd_endpoint.c 가 udc_ep_buf_set_zlp).
		 * 드라이버는 플래그만 보면 되고, 중복 계산하면 두 판단이
		 * 어긋날 수 있다.
		 *
		 * 버퍼는 큐에 그대로 두고 busy 도 유지한다 — 같은 버퍼의
		 * 전송이 아직 끝나지 않은 것으로 다룬다.
		 */
		int err;

		udc_ep_buf_clear_zlp(buf);
		priv->ep0_xfer_len = 0U;

		err = dwc3_ep0_start(dev, DWC3_EP0_IN, 0U, 0U,
				     DWC3_TRBCTL_CONTROL_DATA);
		if (err == 0) {
			LOG_DBG("ep%u ZLP queued (sent=%u wLength=%u)",
				ep, done, priv->ep0_wlength);
			return;
		}

		/* ZLP 를 못 보냈으면 그대로 완료로 처리한다 — 호스트가
		 * 타임아웃으로 복구하는 편이 전송을 매달아 두는 것보다 낫다. */
		LOG_WRN("ep%u ZLP start failed (%d) — 완료로 처리", ep, err);
	}

	if (ep == DWC3_EP0_OUT) {
		/*
		 * 컨트롤러가 DMA 로 쓴 영역이다. 읽기 '전에' 무효화한다.
		 * 하드웨어에 허용한 길이(MPS 배수로 올린 값) 전체를 무효화해야
		 * 한다 — 그만큼 썼을 수 있다. 버퍼 풀이 캐시 라인 정렬·단위라
		 * (USB_BUF_ALIGN == USB_BUF_GRANULARITY == 캐시 라인) 인접
		 * 변수를 오염시키지 않는다.
		 */
		sys_cache_data_invd_range(buf->data, priv->ep0_xfer_len);

		if (done > priv->ep0_wlength) {
			LOG_WRN("EP0 OUT overflow: done=%u wLength=%u",
				done, priv->ep0_wlength);
			buf = udc_buf_get(ep_cfg);
			udc_ep_set_busy(ep_cfg, false);
			udc_submit_ep_event(dev, buf, -EIO);
			dwc3_ep0_stall_and_restart(dev, DWC3_EP0_OUT);
			return;
		}

		net_buf_add(buf, done);
	}

	LOG_DBG("ep%u DATA done=%u (wLength=%u)", ep, done, priv->ep0_wlength);

	buf = udc_buf_get(ep_cfg);
	udc_ep_set_busy(ep_cfg, false);
	udc_submit_ep_event(dev, buf, 0);

	/*
	 * 상태 단계는 '스택이 버퍼를 줄 때' 와 'XferNotReady(STATUS) 가 올
	 * 때' 중 늦은 쪽에 시작된다. 방향이 데이터 단계의 반대이므로
	 * 양방향을 함께 살핀다.
	 */
	priv->ep0_state = DWC3_EP0_STATUS;
	dwc3_ep0_kick(dev);
}

/*
 * EP0 양방향의 큐를 모두 살펴 시작할 수 있는 전송을 시작한다.
 *
 * 한쪽만 보면 안 된다 — 상태 단계는 데이터 단계의 '반대 방향' 에서 일어나고
 * (데이터 IN -> 상태는 EP0 OUT, 데이터 OUT -> 상태는 EP0 IN), 스택이 어느
 * 방향에 버퍼를 걸지는 요청에 따라 다르다.
 */
static void dwc3_ep0_kick(const struct device *dev)
{
	dwc3_handle_xfer_next(dev, udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT));
	dwc3_handle_xfer_next(dev, udc_get_ep_cfg(dev, USB_CONTROL_EP_IN));
}

/*
 * 호스트가 다음 단계를 요구한다. 우리에게 중요한 것은 STATUS 요구다 —
 * 상태 단계 TRB 는 이 이벤트를 받은 뒤에 시작해야 한다.
 *
 * DATA 요구는 Zephyr 모델에서 정상적으로 버퍼보다 먼저 올 수 있다 (스택이
 * 비동기로 enqueue 한다). 그래서 stall 하지 않고 무시한다 — 버퍼가 오면
 * ep_enqueue 가 전송을 시작하고, 하드웨어는 그때까지 NAK 한다.
 */
static void dwc3_ep0_xfer_not_ready(const struct device *dev, uint8_t ep,
				    uint32_t stage)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);

	if (stage != DWC3_DEPEVT_XNR_STAGE_STATUS) {
		LOG_DBG("ep%u XferNotReady(DATA) — 버퍼 대기", ep);
		return;
	}

	priv->ep0_nrdy_status = true;

	/* 상태 단계 버퍼가 이미 큐에 있으면 지금 시작한다 */
	dwc3_ep0_kick(dev);
}

/*
 * 큐 맨 앞의 버퍼로 전송을 시작한다. ep_enqueue 와 단계 완료 양쪽에서
 * 불린다 — 조건이 갖춰진 순간이 어느 쪽인지 알 수 없기 때문이다.
 */
static void dwc3_handle_xfer_next(const struct device *dev,
				  struct udc_ep_config *const ep_cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t ep = USB_EP_DIR_IS_IN(ep_cfg->addr) ? DWC3_EP0_IN :
						      DWC3_EP0_OUT;
	struct udc_buf_info *bi;
	struct net_buf *buf;
	uint32_t trbctl, len;
	int err;

	if (udc_ep_is_busy(ep_cfg)) {
		return;
	}

	buf = udc_buf_peek(ep_cfg);
	if (buf == NULL) {
		return;
	}

	bi = udc_get_buf_info(buf);

	/*
	 * ⚠ SETUP 버퍼는 전송 요청이 아니다.
	 *
	 * 스택은 제어 전송이 끝날 때마다 bi->setup 이 선 버퍼를 EP0 OUT
	 * 큐에 넣어 둔다 (usbd_enqueue_setup). 이것은 다음 SETUP 8바이트를
	 * 담을 '그릇' 이고, udc_setup_received() 가 큐에서 이 버퍼를 만나면
	 * 일부러 break 해서 큐에 남겨 둔다 (udc_common.c).
	 *
	 * 이걸 전송으로 시작하면 다음이 한꺼번에 깨진다:
	 *   - SETUP TRB 와 같은 슬롯(DWC3_EP0_TRB(OUT,0))을 덮어쓴다
	 *   - 이미 전송이 진행 중인 EP 에 DEPSTRTXFER 를 또 낸다
	 *   - ep0_state 가 DATA_OUT 이 되어, 뒤이어 오는 SETUP 의
	 *     XferComplete 가 SETUP 이 아니라 데이터 완료로 처리된다
	 * 즉 첫 제어 전송부터 열거가 실패한다. 건너뛴다.
	 */
	if (bi->setup) {
		LOG_DBG("SETUP 그릇 버퍼 — 전송하지 않는다");
		return;
	}

	if (bi->status) {
		/*
		 * 상태 단계. 두 조건이 모두 필요하다:
		 *   - 호스트가 요구했다 (XferNotReady(STATUS))
		 *   - 상태 단계를 받아들일 상태다
		 * 두 번째가 없으면, 낡은 ep0_nrdy_status 때문에 데이터 단계가
		 * 진행 중인데도 상태 TRB 가 걸릴 수 있다 — 상태 단계는 데이터
		 * 단계의 '반대 방향' 이라 udc_ep_is_busy 로는 막히지 않는다.
		 */
		if (!priv->ep0_nrdy_status) {
			LOG_DBG("STATUS 버퍼 대기 중 — NotReady 아직 안 옴");
			return;
		}
		if (priv->ep0_state != DWC3_EP0_STATUS &&
		    priv->ep0_state != DWC3_EP0_TWO_STAGE) {
			LOG_DBG("STATUS 버퍼 보류 — state=%d",
				(int)priv->ep0_state);
			return;
		}

		/*
		 * 2단계(wLength==0)는 STATUS_2, 데이터 단계가 있었으면
		 * STATUS_3 이다.
		 */
		trbctl = (priv->ep0_wlength == 0U) ? DWC3_TRBCTL_STATUS_2 :
						     DWC3_TRBCTL_STATUS_3;
		len = 0U;
		priv->ep0_xfer_len = 0U;
		err = dwc3_ep0_start(dev, ep, 0U, len, trbctl);
	} else if (priv->ep0_state != DWC3_EP0_DATA_WAIT) {
		/*
		 * 데이터 단계는 SETUP 을 소비한 직후에만 시작한다. 그 밖의
		 * 상태에서 데이터 버퍼가 보이면 (예: 호스트가 데이터 단계를
		 * 조기 종료했는데 스택이 아직 버퍼를 내려 주는 중) 걸지 않는다.
		 */
		LOG_DBG("DATA 버퍼 보류 — state=%d", (int)priv->ep0_state);
		return;
	} else if (ep == DWC3_EP0_IN) {
		/* 데이터 IN — 보낼 내용을 하드웨어가 읽어 가므로 flush */
		len = buf->len;
		sys_cache_data_flush_range(buf->data, len);
		priv->ep0_xfer_len = len;
		priv->ep0_state = DWC3_EP0_DATA_IN;
		err = dwc3_ep0_start(dev, ep, k_mem_phys_addr(buf->data), len,
				     DWC3_TRBCTL_CONTROL_DATA);
	} else {
		/*
		 * 데이터 OUT.
		 *
		 * ⚠ DWC3 는 OUT TRB 의 길이가 MPS 의 배수가 아니면, 짧은
		 *   패킷이 오거나 TRB_BUFSIZ 가 0 이 될 때까지 unchained
		 *   OUT TRB 를 회수하지 않는다. 즉 길이를 그대로 걸면 전송이
		 *   영구히 끝나지 않을 수 있다. MPS 배수로 올려서 건다.
		 *   버퍼가 그만큼 크지 않으면 넘치므로 가진 공간으로 자른다.
		 */
		len = ROUND_DOWN(buf->size, DWC3_EP0_MPS);
		if (len == 0U) {
			LOG_ERR("EP0 OUT buffer too small (%u < MPS %u)",
				buf->size, DWC3_EP0_MPS);
			udc_submit_ep_event(dev, udc_buf_get(ep_cfg), -ENOMEM);
			return;
		}

		/* 하드웨어가 쓸 영역이므로 미리 비워 캐시와 어긋나지 않게 한다 */
		sys_cache_data_flush_range(buf->data, len);
		priv->ep0_xfer_len = len;
		priv->ep0_state = DWC3_EP0_DATA_OUT;
		err = dwc3_ep0_start(dev, ep, k_mem_phys_addr(buf->data), len,
				     DWC3_TRBCTL_CONTROL_DATA);
	}

	if (err != 0) {
		/*
		 * 전송을 시작하지 못했다. 에러만 제출하고 돌아가면 SETUP TRB
		 * 가 무장되지 않은 상태로 남아 다음 SETUP 을 영구히 못 받는다
		 * (스택은 상태 버퍼 에러에서 SETUP 을 다시 등록하지 않는다).
		 * stall 로 호스트에 알리고 SETUP 부터 다시 시작한다.
		 */
		LOG_ERR("ep%u xfer start failed (%d) — stall 후 재시작", ep, err);
		udc_submit_ep_event(dev, udc_buf_get(ep_cfg), err);
		dwc3_ep0_stall_and_restart(dev, ep);
		return;
	}

	udc_ep_set_busy(ep_cfg, true);
}

/* ── 일반 엔드포인트 (bulk / interrupt) ──────────────────────────────
 * EP0 은 제어 전송의 단계 기계가 필요해 따로 다루고, 여기서는 그보다
 * 단순하다 — 큐에서 버퍼를 꺼내 TRB 하나로 걸고 완료되면 올린다.
 *
 * 한 엔드포인트에 전송 하나만 건다. TRB 체인·스캐터개더·'진행 중 전송'
 * (DEPUPDXFER) 전략은 넣지 않았다. 성능이 필요해지면 그때 올린다.
 */

/* DT 의 엔드포인트 속성 -> DWC3 의 EP_TYPE 필드 값 (USB bmAttributes 인코딩) */
static uint32_t dwc3_ep_type(const struct udc_ep_config *const cfg)
{
	switch (cfg->attributes & USB_EP_TRANSFER_TYPE_MASK) {
	case USB_EP_TYPE_ISO:
		return DWC3_DEPCFG_EP_TYPE_ISOC;
	case USB_EP_TYPE_BULK:
		return DWC3_DEPCFG_EP_TYPE_BULK;
	case USB_EP_TYPE_INTERRUPT:
		return DWC3_DEPCFG_EP_TYPE_INTERRUPT;
	default:
		return DWC3_DEPCFG_EP_TYPE_CONTROL;
	}
}

/*
 * 엔드포인트를 쓸 수 있게 만든다.
 *
 *   DEPSTARTCFG(base=2)  — SET_CONFIGURATION 이후 EP2 이상을 설정하기
 *                          '전에' 정확히 한 번. 베이스 2 는 databook 규정이다
 *                          (EP0/EP1 설정 때는 0 이었다).
 *   DEPCFG               — 타입·MPS·FIFO 번호·interval
 *   DEPXFERCFG           — 전송 리소스 1개
 *   DALEPENA             — 활성화
 *
 * TX FIFO(GTXFIFOSIZ)는 건드리지 않는다 — 하드웨어 기본 배치를 쓴다.
 */
static int dwc3_ep_configure(const struct device *dev,
			     struct udc_ep_config *const cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t phys = DWC3_PHYS_EP(cfg->addr);
	uint32_t par0, par1, st;
	uint32_t fifo_num;
	int err;

	if (phys >= DWC3_NUM_PHYS_EPS) {
		LOG_ERR("ep 0x%02x -> phys %u 범위 밖 (max %u)",
			cfg->addr, phys, DWC3_NUM_PHYS_EPS - 1U);
		return -EINVAL;
	}

	if (!priv->started_cfg2) {
		/*
		 * ⚠ 베이스 2 는 COMMANDPARAM 으로 전달한다. 파라미터
		 *   레지스터(PAR0/1/2)가 아니다 — 거기에 2 를 넣어도
		 *   하드웨어는 base=0 으로 읽고, 그러면 EP0 의 전송 리소스를
		 *   다시 초기화해 EP0 이 깨지거나 EP2 이상의 리소스가
		 *   EP0/EP1 과 겹친다.
		 */
		err = dwc3_dep_cmd_ioc(dev, DWC3_EP0_OUT,
				       DWC3_DEPCMD_DEPSTARTCFG,
				       0U, 0U, 0U,
				       FIELD_PREP(DWC3_DEPCMD_COMMANDPARAM, 2U),
				       &st);
		if (err != 0) {
			LOG_ERR("DEPSTARTCFG(2) failed (%d)", err);
			return err;
		}
		priv->started_cfg2 = true;
		LOG_INF("DEPSTARTCFG(base=2) status=0x%x", st);
	}

	/*
	 * FIFO_NUM 은 OUT 엔드포인트에서 0 이고, IN 엔드포인트는 phys >> 1
	 * 이다 (EP0 IN 만 예외로 0 — 여기는 일반 엔드포인트만 다룬다).
	 */
	fifo_num = USB_EP_DIR_IS_IN(cfg->addr) ? (phys >> 1) : 0U;

	par0 = FIELD_PREP(DWC3_DEPCFG_PAR0_ACTION,
			  DWC3_DEPCFG_ACTION_INITIALIZE) |
	       FIELD_PREP(DWC3_DEPCFG_PAR0_FIFO_NUM, fifo_num) |
	       FIELD_PREP(DWC3_DEPCFG_PAR0_MAX_PACKET, cfg->mps) |
	       FIELD_PREP(DWC3_DEPCFG_PAR0_EP_TYPE, dwc3_ep_type(cfg));

	/*
	 * XFER_CMPL_EN — 완료를 받는다
	 * XFER_NRDY_EN — 호스트가 전송을 요구할 때를 받는다. 일반
	 *                엔드포인트는 이 이벤트를 한 번 받은 뒤에야 첫
	 *                DEPSTRTXFER 를 내야 하므로 반드시 켠다.
	 * XFER_IP_EN   — TRB 를 여러 개 걸 때만 의미가 있어 끈다.
	 */
	/*
	 * INTERVAL 은 bInterval - 1 이다. bulk 에는 의미가 없으므로 0 을
	 * 둔다 — 엉뚱한 값이 들어가 폴링 주기가 왜곡되지 않게 한다.
	 */
	par1 = FIELD_PREP(DWC3_DEPCFG_PAR1_EP_NUMBER, phys) |
	       FIELD_PREP(DWC3_DEPCFG_PAR1_INTERVAL,
			  (dwc3_ep_type(cfg) == DWC3_DEPCFG_EP_TYPE_INTERRUPT &&
			   cfg->interval > 0U) ? (cfg->interval - 1U) : 0U) |
	       DWC3_DEPCFG_PAR1_XFER_CMPL_EN |
	       DWC3_DEPCFG_PAR1_XFER_NRDY_EN |
	       FIELD_PREP(DWC3_DEPCFG_PAR1_INTR_NUM, 0U);

	err = dwc3_dep_cmd(dev, phys, DWC3_DEPCMD_DEPCFG, par0, par1, 0U, &st);
	if (err != 0) {
		LOG_ERR("ep%u DEPCFG failed (%d)", phys, err);
		return err;
	}
	LOG_INF("ep%u DEPCFG par0=0x%08x par1=0x%08x status=0x%x",
		phys, par0, par1, st);

	err = dwc3_dep_cmd(dev, phys, DWC3_DEPCMD_DEPXFERCFG,
			   FIELD_PREP(DWC3_DEPXFERCFG_NUM_XFER_RES, 1U),
			   0U, 0U, &st);
	if (err != 0) {
		LOG_ERR("ep%u DEPXFERCFG failed (%d)", phys, err);
		return err;
	}

	priv->ep[phys].rsrc_valid = false;
	priv->ep[phys].got_not_ready = false;
	priv->ep[phys].canceling = false;
	priv->ep[phys].xfer_len = 0U;

	dwc3_write32(dev, DWC3_DALEPENA,
		     dwc3_read32(dev, DWC3_DALEPENA) | BIT(phys));

	LOG_INF("ep 0x%02x (phys %u) enabled: type=%u mps=%u DALEPENA=0x%08x",
		cfg->addr, phys, (unsigned int)dwc3_ep_type(cfg), cfg->mps,
		dwc3_read32(dev, DWC3_DALEPENA));
	return 0;
}

/*
 * 큐 맨 앞의 버퍼로 전송을 시작한다.
 *
 * ⚠ got_not_ready 가 서기 전에는 시작하지 않는다. DWC3 는 일반
 *   엔드포인트에서 XferNotReady 를 받기 전에 DEPSTRTXFER 를 내는 것을
 *   기대하지 않는다.
 */
static void dwc3_ep_xfer_next(const struct device *dev,
			      struct udc_ep_config *const ep_cfg)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint8_t phys = DWC3_PHYS_EP(ep_cfg->addr);
	struct dwc3_trb *trb = DWC3_EP_TRB(phys);
	uintptr_t trb_phys = k_mem_phys_addr(trb);
	struct net_buf *buf;
	uint32_t len, ctl, st;
	int err;

	if (priv->ep[phys].canceling || ep_cfg->stat.halted) {
		return;
	}
	if (!priv->ep[phys].got_not_ready) {
		LOG_DBG("ep%u XferNotReady 대기 중", phys);
		return;
	}
	if (udc_ep_is_busy(ep_cfg)) {
		return;
	}

	buf = udc_buf_peek(ep_cfg);
	if (buf == NULL) {
		return;
	}

	ctl = DWC3_TRBCTL_NORMAL | DWC3_TRB_IOC | DWC3_TRB_LST;

	if (USB_EP_DIR_IS_IN(ep_cfg->addr)) {
		len = buf->len;
		/* 하드웨어가 읽어 갈 내용이므로 먼저 메모리에 내린다 */
		if (len > 0U) {
			sys_cache_data_flush_range(buf->data, len);
		}
	} else {
		/*
		 * 받을 공간을 건다.
		 *
		 * ⚠ 'buf->size - buf->len' 은 남은 공간이 아니다 —
		 *   net_buf 는 앞쪽에 headroom 을 둘 수 있어서, 그 식은
		 *   headroom 만큼 버퍼 끝을 넘어선다. net_buf_tailroom() 이
		 *   size - headroom - len 을 준다.
		 *
		 * EP0 과 달리 길이를 MPS 배수로 올리지 않는다 — 일반
		 * 엔드포인트는 짧은 패킷이 오면 TRB 가 회수되고, 그때 TRB
		 * status 의 '남은 바이트' 와 SPR 비트로 실제 수신량을 안다.
		 * 다만 하드웨어가 TRB 가 덮는 크기보다 큰 패킷을 받으면
		 * 넘쳐 쓰므로, MPS 배수로 '내림' 해 오버런을 막는다.
		 */
		len = net_buf_tailroom(buf);
		if (len >= ep_cfg->mps) {
			len = ROUND_DOWN(len, ep_cfg->mps);
		}

		if (len == 0U) {
			/*
			 * 받을 공간이 없다. 주소 0, 길이 0 의 OUT TRB 는
			 * 동작이 정의돼 있지 않으므로 시작하지 않는다.
			 */
			LOG_WRN("ep%u OUT 버퍼에 여유 공간 없음 (mps=%u)",
				phys, ep_cfg->mps);
			udc_submit_ep_event(dev, udc_buf_get(ep_cfg), -ENOMEM);
			return;
		}

		sys_cache_data_flush_range(net_buf_tail(buf), len);
	}

	priv->ep[phys].xfer_len = len;
	dwc3_trb_fill(trb, (len > 0U) ?
		      k_mem_phys_addr(USB_EP_DIR_IS_IN(ep_cfg->addr) ?
				      buf->data : net_buf_tail(buf)) : 0U,
		      len, ctl);

	/*
	 * ⚠ CMDIOC 를 켠다. DEPENDXFER 에 필요한 전송 리소스 인덱스는
	 *   이 명령의 CMD_CMPLT 이벤트로만 오기 때문이다.
	 */
	err = dwc3_dep_cmd_ioc(dev, phys, DWC3_DEPCMD_DEPSTRTXFER,
			       DWC3_DEPSTRTXFER_PAR0(trb_phys),
			       DWC3_DEPSTRTXFER_PAR1(trb_phys), 0U,
			       DWC3_DEPCMD_CMDIOC, &st);
	if (err != 0) {
		/*
		 * 이 버퍼는 포기하고 다음 것으로 넘어간다 — 여기서 그냥
		 * 돌아가면 다음 enqueue 가 올 때까지 큐가 멈춘다.
		 * busy 를 올리지 않았으므로 재진입은 안전하다.
		 */
		LOG_ERR("ep%u DEPSTRTXFER failed (%d)", phys, err);
		udc_submit_ep_event(dev, udc_buf_get(ep_cfg), err);
		dwc3_ep_xfer_next(dev, ep_cfg);
		return;
	}

	udc_ep_set_busy(ep_cfg, true);
	LOG_DBG("ep%u xfer started: len=%u status=0x%x", phys, len, st);
}

/* 전송 완료 */
static void dwc3_ep_xfer_complete(const struct device *dev, uint8_t phys)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	struct dwc3_trb *trb = DWC3_EP_TRB(phys);
	uint8_t ep_addr = ((phys & 1U) ? USB_EP_DIR_IN : USB_EP_DIR_OUT) |
			  (phys >> 1);
	struct udc_ep_config *ep_cfg = udc_get_ep_cfg(dev, ep_addr);
	struct net_buf *buf;
	uint32_t trbsts = 0U;
	uint32_t remaining, done;

	if (ep_cfg == NULL) {
		LOG_WRN("phys %u -> 알 수 없는 엔드포인트", phys);
		return;
	}

	/*
	 * ⚠ busy 가 아니면 이 이벤트는 낡은 것이다.
	 *   dequeue/disable 이 busy 를 지운 직후 새 버퍼가 큐에 들어왔는데
	 *   이전 전송의 XferComplete 가 링에 남아 있을 수 있다. 그때 TRB 의
	 *   HWO 는 이미 0 이라 reclaim 이 성공하므로, 가드가 없으면 시작도
	 *   안 한 버퍼를 이전 전송의 길이로 완료 처리한다 (OUT 은
	 *   net_buf_add 까지 한다).
	 */
	if (!udc_ep_is_busy(ep_cfg)) {
		LOG_DBG("ep%u 낡은 XferComplete 무시 (busy 아님)", phys);
		return;
	}

	if (dwc3_trb_reclaim(trb, &trbsts) != 0) {
		LOG_WRN("ep%u XferComplete but TRB still HW-owned", phys);
		return;
	}

	/* 전송이 끝났으므로 리소스 인덱스는 더 이상 유효하지 않다 */
	priv->ep[phys].rsrc_valid = false;

	buf = udc_buf_peek(ep_cfg);
	if (buf == NULL) {
		LOG_WRN("ep%u XferComplete with no queued buffer", phys);
		udc_ep_set_busy(ep_cfg, false);
		return;
	}

	remaining = FIELD_GET(DWC3_TRB_BUFSIZ, trbsts);
	done = (remaining > priv->ep[phys].xfer_len) ? 0U :
	       (priv->ep[phys].xfer_len - remaining);

	if (USB_EP_DIR_IS_IN(ep_addr)) {
		/*
		 * IN 방향의 길이 0 패킷.
		 *
		 * ⚠ databook 에 Normal-ZLP 전송 타입이 있지만 실제로 동작하지
		 *   않는다고 알려져 있다. 그래서 길이 0 TRB 를 따로 건다.
		 * 조건 판정은 UDC 스택이 했다 (bi->zlp).
		 */
		if (udc_ep_buf_has_zlp(buf)) {
			uintptr_t trb_phys = k_mem_phys_addr(trb);
			uint32_t st;

			udc_ep_buf_clear_zlp(buf);
			priv->ep[phys].xfer_len = 0U;
			dwc3_trb_fill(trb, 0U, 0U, DWC3_TRBCTL_NORMAL |
					      DWC3_TRB_IOC | DWC3_TRB_LST);

			if (dwc3_dep_cmd_ioc(dev, phys,
					     DWC3_DEPCMD_DEPSTRTXFER,
					     DWC3_DEPSTRTXFER_PAR0(trb_phys),
					     DWC3_DEPSTRTXFER_PAR1(trb_phys),
					     0U, DWC3_DEPCMD_CMDIOC,
					     &st) == 0) {
				LOG_DBG("ep%u ZLP queued (sent=%u)", phys, done);
				return;	/* busy 유지 — 아직 끝나지 않았다 */
			}
			LOG_WRN("ep%u ZLP start failed — 완료로 처리", phys);
		}
	} else {
		/*
		 * 컨트롤러가 DMA 로 쓴 영역이다. 읽기 '전에' 무효화한다.
		 * SPR(Short Packet Received)이 서 있으면 호스트가 MPS 보다
		 * 짧은 패킷으로 전송을 끝낸 것이다 — 정상이다.
		 */
		sys_cache_data_invd_range(net_buf_tail(buf),
					  priv->ep[phys].xfer_len);
		net_buf_add(buf, done);

		if ((trbsts & DWC3_TRB_SPR) != 0U) {
			LOG_DBG("ep%u short packet (done=%u)", phys, done);
		}
	}

	LOG_DBG("ep%u done=%u remaining=%u", phys, done, remaining);

	buf = udc_buf_get(ep_cfg);
	udc_ep_set_busy(ep_cfg, false);
	udc_submit_ep_event(dev, buf, 0);

	dwc3_ep_xfer_next(dev, ep_cfg);
}

/*
 * DEPEVT_CMD_CMPLT — 엔드포인트 명령이 끝났다.
 *
 * ⚠ 이것이 전송 리소스 인덱스(rsrc_id)를 얻는 '유일한' 경로다.
 *   CMD_TYPE 이 DEPSTRTXFER 인 이벤트의 RSRC_ID 필드가 그 값이고,
 *   DEPENDXFER 로 전송을 끊을 때 반드시 필요하다.
 *   그래서 DEPSTRTXFER 를 낼 때 CMDIOC 를 켠다.
 */
static void dwc3_handle_cmd_cmplt(const struct device *dev, uint8_t phys,
				  uint32_t raw)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t cmd_type = FIELD_GET(DWC3_DEPEVT_CC_CMD_TYPE, raw);
	uint32_t rsrc_id = FIELD_GET(DWC3_DEPEVT_CC_RSRC_ID, raw);

	if (phys >= DWC3_NUM_PHYS_EPS) {
		return;
	}

	switch (cmd_type) {
	case DWC3_DEPCMD_DEPSTRTXFER:
		priv->ep[phys].rsrc_id = (uint8_t)rsrc_id;
		priv->ep[phys].rsrc_valid = true;
		LOG_DBG("ep%u xfer started, rsrc_id=%u", phys, rsrc_id);
		break;
	case DWC3_DEPCMD_DEPENDXFER:
		LOG_DBG("ep%u xfer ended", phys);
		priv->ep[phys].canceling = false;
		priv->ep[phys].rsrc_valid = false;
		break;
	default:
		LOG_DBG("ep%u cmd %u complete", phys, cmd_type);
		break;
	}
}

/* ── 아래쪽 절반 (스레드) ─────────────────────────────────────────────
 * ISR 이 깨우면 UDC 락을 잡고 이벤트 링을 비운다. 여기서는 스레드 문맥이라
 * udc_setup_received() 를 부를 수 있고, DEPCMD 를 동기로 기다려도 된다.
 */
static void dwc3_handle_events(const struct device *dev)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t cnt;

	cnt = dwc3_read32(dev, DWC3_GEVNTCOUNT(DWC3_EVT_INTR_IDX)) &
	      DWC3_GEVNTCOUNT_EVNTCOUNT;
	if (cnt == 0U) {
		return;
	}

	/*
	 * 컨트롤러가 DMA 로 쓴 내용을 CPU 가 보려면 읽기 '전에' 무효화해야
	 * 한다. 순서가 틀리면 캐시에 남은 옛 값을 읽고 "가끔 동작" 한다.
	 */
	sys_cache_data_invd_range(dwc3_evt_buf, sizeof(dwc3_evt_buf));

	/*
	 * ⚠ 리셋을 처리한 뒤에는 이 배치를 끊는다 — 'done' 까지만 ack 하고
	 *   남은 이벤트는 다음 호출에서 받는다.
	 *
	 *   이유: Zephyr 의 usbd 스레드는 어떤 이벤트를 꺼내든 맨 먼저
	 *   event_handler_ep_request() 로 ep_events 리스트를 비운다
	 *   (usbd_core.c). 그래서 우리가 한 배치에서 RESET 과 SETUP 을
	 *   연달아 올리면, usbd 가 RESET 메시지를 꺼낼 때 '먼저' SETUP 을
	 *   처리하고 '그 다음' 리셋 처리를 한다 — 순서가 뒤집힌다.
	 *
	 *   실기 첫 전송에서 정확히 그랬다:
	 *     00.625  (우리가) UDC_EVT_RESET 제출
	 *     00.650  (우리가) SETUP 제출
	 *     00.669  device address = 1   <- SETUP 이 먼저 처리됨
	 *     00.702  device address = 0   <- 리셋이 나중에
	 *   호스트는 주소 1 로 말을 거는데 우리는 0 이 되어 무응답이 되고,
	 *   1초 타임아웃마다 리셋이 반복됐다 (36번).
	 *
	 *   배치를 끊으면 usbd 스레드가 리셋을 끝낸 뒤에 SETUP 이 올라간다.
	 */
	bool stop_batch = false;
	uint32_t done = 0;

	for (uint32_t i = 0; i < cnt; i += sizeof(uint32_t)) {
		uint32_t raw = dwc3_evt_buf[priv->evt_pos / sizeof(uint32_t)];

		priv->evt_pos = (priv->evt_pos + sizeof(uint32_t)) %
				DWC3_EVT_BUF_BYTES;
		done = i + sizeof(uint32_t);

		if ((raw & DWC3_EVT_NON_EP) != 0U) {
			uint32_t type = FIELD_GET(DWC3_DEVT_TYPE, raw);

			LOG_DBG("DEVT %s (type=%u info=0x%04x)",
				dwc3_devt_name(type), type,
				(unsigned int)FIELD_GET(DWC3_DEVT_INFO, raw));

			switch (type) {
			case DWC3_DEVT_USB_RESET:
				priv->stats.reset++;
				/*
				 * 리셋은 컨트롤러의 엔드포인트 상태를 날린다.
				 * EP0 을 다시 설정하고 SETUP 을 재무장한다.
				 * 스택에도 알려 큐를 정리하게 한다.
				 */
				/*
				 * ⚠ 여기서 UDC_EVT_RESET 을 올리면 안 된다.
				 *   스택의 RESET 처리(usbd_core.c)가 그 자리에서
				 *   udc_device_speed() 를 읽어 status.speed 를
				 *   확정하는데, DSTS.CONNECTSPD 는
				 *   CONNECTION_DONE 뒤에야 유효하다. 속도를 잘못
				 *   잡으면 mps0 와 반환할 디스크립터 세트가 틀어진다.
				 *   인트리 dwc2 도 RESET 을 열거 완료 시점에 올린다.
				 *   그래서 아래 CONNECTION_DONE 에서 올린다.
				 */
				LOG_INF("USB reset");
				/*
				 * 리셋은 엔드포인트 설정을 전부 날린다.
				 * DEPSTARTCFG(base=2) 도 다시 내야 하고
				 * 리소스 인덱스도 모두 무효가 된다.
				 */
				priv->started_cfg2 = false;
				/*
				 * ⚠ memset(priv->ep) 보다 '먼저' 끊는다 — 그 memset 이 rsrc_id 를
				 *   지우면 DEPENDXFER 를 낼 근거가 없어진다.
				 */
				dwc3_ep0_end_xfer(dev, DWC3_EP0_OUT);
				dwc3_ep0_end_xfer(dev, DWC3_EP0_IN);
				memset(priv->ep, 0, sizeof(priv->ep));
				priv->ep0_state = DWC3_EP0_SETUP;
				priv->ep0_nrdy_status = false;
				/*
				 * ⚠ 반드시 '즉시' 재무장한다.
				 *
				 *   dwc3_ep0_configure() 의 DEPSTARTCFG 가 전송
				 *   리소스를 초기화하므로, 이 시점에 기존에 무장된
				 *   SETUP TRB 는 이미 무효다. 재무장을 미루면 그
				 *   사이에 SETUP 을 받을 그릇이 없다.
				 *
				 *   한때 "링을 비운 뒤" 로 미뤘다가 더 큰 구멍을
				 *   만들었다: 한 이벤트 배치에 리셋이 여러 개 오면
				 *   configure 는 매번 실행되는데 재무장은 배치 끝에
				 *   한 번뿐이어서, 그 배치 내내 TRB 가 없었다.
				 *   실기 로그에서 리셋 36번 / configure 37번 /
				 *   무장 22번으로 드러났고, 231 ms 동안 무장이 없는
				 *   창에서 호스트의 GET_DESCRIPTOR 가 버려졌다
				 *   (전체 로그에 SETUP 80 06 이 한 번도 없었다).
				 *
				 *   낡은 XferComplete 를 덮어쓰는 문제는 남지만
				 *   실기에서 36번 중 2번이고, 그때는 조기 반환으로
				 *   무해하게 흘린다 (호스트가 재시도한다).
				 */
				/*
				 * ⚠ 받아 둔 SETUP 이 있으면 '지우기 전에' 처리한다.
				 *
				 *   실기 측정 (리셋 36번):
				 *     32번  HWO=1, buf=00 00 00 00   받은 것 없음
				 *      4번  HWO=0, buf=00 05 01 00   SET_ADDRESS(1) 가
				 *                                   들어와 있다
				 *            (st=0 → BUFSIZ 8→0, 8바이트 전부 전송)
				 *
				 *   아래 dwc3_ep0_configure()+dwc3_ep0_arm_setup() 은
				 *   버퍼를 memset 하고 HWO 를 다시 세우므로, 이 4번을
				 *   매번 파괴했다. 그래서 setup_received 가 0 이었고
				 *   호스트는 응답을 못 받아 리셋을 되풀이했다.
				 *
				 *   왜 리셋 이벤트가 먼저 오는가: SETUP 은 리셋보다
				 *   먼저 도착했지만, 컨트롤러가 그 완료 이벤트를 리셋
				 *   이벤트 뒤에 올린다. 링 순서는 바꿀 수 없으므로
				 *   여기서 거둬들인다.
				 *
				 *   참고: SET_ADDRESS 가 첫 요청인 것은 정상이다.
				 *   xHCI 호스트(macOS)는 Address Device 명령으로
				 *   SET_ADDRESS 를 먼저 보내고 그 뒤에 GET_DESCRIPTOR
				 *   를 한다.
				 */
				{
					struct dwc3_trb *t =
						DWC3_EP0_TRB(DWC3_EP0_OUT, 0);

					sys_cache_data_invd_range(t, sizeof(*t));
					if ((t->control & DWC3_TRB_HWO) == 0U &&
					    priv->ep0_state == DWC3_EP0_SETUP) {
						LOG_INF("리셋 전에 받아 둔 SETUP 을 먼저 처리한다");
						dwc3_ep0_setup_received(dev, raw);
					}
				}

				(void)dwc3_ep0_configure(dev);
				(void)dwc3_ep0_arm_setup(dev);
				/*
				 * ⚠ RESET 이벤트를 '여기서' 올린다 —
				 *   CONNECTION_DONE 이 아니다.
				 *
				 *   스택의 RESET 처리는 udc_set_address(0) 을
				 *   부른다. 그런데 usbd 스레드는 비동기라, 우리가
				 *   CONNECTION_DONE 에서 올리면 그 처리가 '우리가
				 *   이미 SETUP 을 처리하고 주소를 적용한 뒤' 에
				 *   도착해 주소를 0 으로 되돌린다.
				 *
				 *   실기 첫 전송에서 정확히 그랬다:
				 *     00.649  SETUP 00 05 (SET_ADDRESS 1)
				 *     00.669  device address = 1
				 *     00.701  device address = 0   <- 스택의 RESET
				 *     00.747  ep1 STATUS done
				 *   호스트는 주소 1 로 말을 거는데 우리는 0 이어서
				 *   이후 모든 요청이 무응답이 되고, 1초 타임아웃마다
				 *   리셋이 반복됐다 (36번).
				 *
				 * ⚠ 속도(DSTS.CONNECTSPD)는 CONNECTION_DONE 뒤에야
				 *   확실하다. 스택이 이 이벤트를 처리하며
				 *   udc_device_speed() 를 읽으므로, 리셋 직후 값이
				 *   유효하지 않으면 mps0 가 틀어질 수 있다. 실기에서
				 *   확인할 사항으로 남긴다 — 순서가 틀리면 열거가
				 *   아예 안 되므로 순서를 먼저 바로잡는다.
				 *
				 * ✅ 2026-11-06: 그 확인이 됐다. 실제로 틀어졌다 —
				 *   벌크 EP 의 mps 가 FS 값 64 로 설정됐다(HS 는
				 *   512). 호스트는 HS 로 열거하는데(ioreg Device
				 *   Speed 2) 스택은 FS 디스크립터를 골랐고, 그래서
				 *   전송이 8배 느렸다.
				 *
				 *   → UDC_EVT_RESET 제출을 CONNECTION_DONE 으로
				 *     옮겼다. 그 시점에는 CONNECTSPD 가 확정이므로
				 *     스택이 udc_device_speed() 에서 HS 를 읽는다.
				 *     Linux dwc3 도 conndone 에서 속도를 확정한다
				 *     (dwc3_gadget_conndone_interrupt).
				 *
				 *   여기서는 EP0 재설정·재무장만 한다 — 그것은
				 *   리셋 직후에 해야 하고 속도와 무관하다.
				 */
				stop_batch = true;
				break;
			case DWC3_DEVT_CONNECTION_DONE: {
				uint32_t dsts = dwc3_read32(dev, DWC3_DSTS);

				priv->stats.connect_done++;

				LOG_INF("connected: CONNECTSPD=%u DSTS=0x%08x",
					(unsigned int)FIELD_GET(
						DWC3_DSTS_CONNECTSPD, dsts),
					dsts);
				dwc3_rk_log_phy_status(dev, "conn");
				/*
				 * 이제 DSTS.CONNECTSPD 가 유효하다. 스택이 이
				 * 이벤트를 처리하면서 udc_device_speed() 를 읽어
				 * 속도를 확정한다 (위 USB_RESET 주석 참고).
				 */
				/*
				 * ⚠ RESET 이벤트를 **여기서** 올린다 (2026-11-06).
				 *
				 * 스택은 이 이벤트를 처리하며 udc_device_speed() 를
				 * 읽어 status.speed 를 확정하고, 그 값으로 FS/HS
				 * 디스크립터를 고른다. USB_RESET 시점에 올리면
				 * CONNECTSPD 가 아직 유효하지 않아 FS 로 잡히고
				 * 벌크 mps 가 64 가 된다 — 실기에서 그렇게 됐다.
				 */
				udc_submit_event(dev, UDC_EVT_RESET, 0);
				break;
			}
			case DWC3_DEVT_SUSPEND_ENTRY:
				priv->stats.suspend++;
				/*
				 * 1단계: 세고 남긴다. 스택에 UDC_EVT_SUSPEND 를
				 * 올리지 않는다 — 올리면 usbd 가 클래스에
				 * 알리고 CDC 가 상태를 바꾼다. 호스트가 실제로
				 * 보내는지 확인한 뒤에 한다.
				 */
				LOG_INF("suspend entry (%u번째) DSTS=0x%08x",
					priv->stats.suspend,
					dwc3_read32(dev, DWC3_DSTS));
				break;
			case DWC3_DEVT_REMOTE_WAKEUP:
				priv->stats.wakeup++;
				LOG_INF("remote wakeup (%u번째)",
					priv->stats.wakeup);
				break;
			case DWC3_DEVT_LINK_STATE_CHANGE:
				/*
				 * ⚠ 로그를 찍지 않는다 — 빈번할 수 있고, 로그
				 *   폭주가 수신을 멈추게 한 전례가 있다.
				 *   마지막 링크 상태만 남겨 moru usb 가 보여 준다.
				 *   HS 에서 resume 은 여기서 U0(0) 로 돌아오는
				 *   것으로 나타난다.
				 */
				priv->stats.link_state++;
				priv->stats.last_linkst = (uint8_t)FIELD_GET(
					DWC3_DSTS_USBLNKST,
					dwc3_read32(dev, DWC3_DSTS));
				break;
			case DWC3_DEVT_DISCONNECT:
				priv->stats.disconnect++;
				/*
				 * VBUS_READY / VBUS_REMOVED 는 올리지 않는다 —
				 * 이 컨트롤러는 VBUS 상태 변화를 감지하지 않고
				 * (caps.can_detect_vbus 를 세우지 않았다),
				 * DISCONNECT 는 버스 분리이지 전원 변화가 아니다.
				 * 의미가 다른 이벤트를 올리면 스택의 상태가
				 * 실제와 어긋난다. 로그만 남긴다.
				 */
				LOG_INF("disconnected");
				break;
			default:
				break;
			}

			if (stop_batch) {
				break;
			}
			continue;
		}

		/* 엔드포인트 이벤트 */
		{
			uint32_t ep   = FIELD_GET(DWC3_DEPEVT_PHYS_EP, raw);
			uint32_t type = FIELD_GET(DWC3_DEPEVT_TYPE, raw);

			if (ep != DWC3_EP0_OUT && ep != DWC3_EP0_IN) {
				uint8_t ep_addr;
				struct udc_ep_config *c;

				/*
				 * ⚠ 이벤트의 ep 필드는 5비트(0~31)인데 우리
				 *   배열은 DWC3_NUM_PHYS_EPS 개뿐이다. 범위를
				 *   확인하지 않으면 배열 밖에 쓴다.
				 */
				if (ep >= DWC3_NUM_PHYS_EPS) {
					LOG_WRN("DEPEVT ep=%u 범위 밖 (max %u)",
						(unsigned int)ep,
						DWC3_NUM_PHYS_EPS - 1U);
					continue;
				}

				switch (type) {
				case DWC3_DEPEVT_XFER_COMPLETE:
					priv->stats.xfer_complete++;
					dwc3_ep_xfer_complete(dev, ep);
					break;
				case DWC3_DEPEVT_XFER_NOT_READY:
					priv->stats.xfer_not_ready++;
					/*
					 * 일반 엔드포인트는 이 이벤트를 한 번
					 * 받은 뒤에야 첫 DEPSTRTXFER 를 낼 수
					 * 있다. 깃발을 세우고 큐를 살핀다.
					 */
					priv->ep[ep].got_not_ready = true;
					ep_addr = ((ep & 1U) ? USB_EP_DIR_IN :
							       USB_EP_DIR_OUT) |
						  (ep >> 1);
					c = udc_get_ep_cfg(dev, ep_addr);
					/* 등록되지 않은 주소면 NULL 이 온다 */
					if (c != NULL && c->stat.enabled) {
						dwc3_ep_xfer_next(dev, c);
					}
					break;
				case DWC3_DEPEVT_CMD_CMPLT:
					dwc3_handle_cmd_cmplt(dev, ep, raw);
					break;
				default:
					LOG_DBG("DEPEVT ep=%u type=%u status=0x%x",
						(unsigned int)ep,
						(unsigned int)type,
						(unsigned int)FIELD_GET(
							DWC3_DEPEVT_STATUS, raw));
					break;
				}
				continue;
			}

			switch (type) {
			case DWC3_DEPEVT_CMD_CMPLT:
				/*
				 * EP0 은 DEPSTRTXFER 에 CMDIOC 를 켜지 않으므로
				 * 보통 여기 오지 않는다. 와도 해롭지 않게
				 * 처리해 두어, 나중에 EP0 도 CMDIOC 를 켜게
				 * 되면 그대로 동작하게 한다.
				 */
				dwc3_handle_cmd_cmplt(dev, ep, raw);
				break;
			case DWC3_DEPEVT_XFER_COMPLETE:
				priv->stats.xfer_complete++;
				if (ep == DWC3_EP0_OUT &&
				    priv->ep0_state == DWC3_EP0_SETUP) {
					dwc3_ep0_setup_received(dev, raw);
				} else {
					dwc3_ep0_xfer_complete(dev, ep);
				}
				break;
			case DWC3_DEPEVT_XFER_NOT_READY:
				priv->stats.xfer_not_ready++;
				/*
				 * ⚠ [임시 계측] 원시 워드와 상태 필드 전체를 찍는다.
				 *
				 *   상태 필드는 비트 15:12 이고 Linux 는 네 값을
				 *   구분한다: 1=CONTROL_DATA, 2=CONTROL_STATUS,
				 *   9=DATA|INVALTRB, 0xa=STATUS|INVALTRB.
				 *   INVALTRB(비트 15)는 "호스트가 요청한 단계에
				 *   TRB 가 없다" 는 뜻이다. 우리는 XNR_STAGE 를
				 *   GENMASK(13,12) 로만 읽어 그 비트를 버리고 있다.
				 *
				 *   실기에서 XferNotReady(STATUS) 가 한 번도 오지
				 *   않고 DATA 만 54번 왔다 — 2단계 전송
				 *   (SET_ADDRESS, wLength=0)에서는 설명되지 않는다.
				 */
				LOG_DBG("XNR raw=0x%08x ep=%u status[15:12]=0x%x stage=%u invaltrb=%u",
					raw, (unsigned int)ep,
					(unsigned int)((raw >> 12) & 0xFU),
					(unsigned int)FIELD_GET(
						DWC3_DEPEVT_XNR_STAGE, raw),
					(raw & DWC3_DEPEVT_XNR_REASON) ? 1U : 0U);
				dwc3_ep0_xfer_not_ready(dev, ep,
					FIELD_GET(DWC3_DEPEVT_XNR_STAGE, raw));
				break;
			default:
				LOG_DBG("DEPEVT ep=%u type=%u status=0x%x",
					(unsigned int)ep, (unsigned int)type,
					(unsigned int)FIELD_GET(
						DWC3_DEPEVT_STATUS, raw));
				break;
			}
		}
	}

	/* ack 는 0 이 아니라 '처리한 바이트 수' 다. */
	/*
	 * ack 는 '처리한 바이트 수' 다. 배치를 중간에 끊었으면 그만큼만
	 * 올려야 남은 이벤트가 다음 호출에서 다시 읽힌다.
	 */
	dwc3_write32(dev, DWC3_GEVNTCOUNT(DWC3_EVT_INTR_IDX), done);

	/* 남은 이벤트가 있으면 스레드를 다시 깨운다 */
	if (done < cnt) {
		LOG_DBG("배치 중단: %u/%u 바이트 처리, 나머지는 다음에",
			done, cnt);
		k_event_post(&priv->drv_evt, BIT(DWC3_DRV_EVT_EVENTS));
	}

}

static void dwc3_thread_handler(void *const arg)
{
	const struct device *dev = (const struct device *)arg;
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t evt;

	evt = k_event_wait(&priv->drv_evt, UINT32_MAX, false, K_FOREVER);

	udc_lock_internal(dev, K_FOREVER);

	if ((evt & BIT(DWC3_DRV_EVT_EVENTS)) != 0U) {
		k_event_clear(&priv->drv_evt, BIT(DWC3_DRV_EVT_EVENTS));
		dwc3_handle_events(dev);

		/* 링을 비웠으니 이벤트 인터럽트를 다시 허용한다 */
		dwc3_write32(dev, DWC3_GEVNTSIZ(DWC3_EVT_INTR_IDX),
			     DWC3_EVT_BUF_BYTES);
	}

	if ((evt & BIT(DWC3_DRV_EVT_XFER)) != 0U) {
		k_event_clear(&priv->drv_evt, BIT(DWC3_DRV_EVT_XFER));
		dwc3_ep0_kick(dev);

		/*
		 * 일반 엔드포인트도 함께 살핀다. 어느 엔드포인트에 버퍼가
		 * 들어왔는지 깃발로 구분하지 않으므로 전부 훑는다 —
		 * 엔드포인트 수가 적어 비용이 무의미하다.
		 */
		for (uint8_t phys = 2; phys < DWC3_NUM_PHYS_EPS; phys++) {
			uint8_t addr = ((phys & 1U) ? USB_EP_DIR_IN :
						      USB_EP_DIR_OUT) |
				       (phys >> 1);
			struct udc_ep_config *c = udc_get_ep_cfg(dev, addr);

			if (c != NULL && c->stat.enabled) {
				dwc3_ep_xfer_next(dev, c);
			}
		}
	}

	udc_unlock_internal(dev);
}

/* DCTL.CSFTRST 가 스스로 풀릴 때까지 기다린다. 무한 대기는 하지 않는다. */
#define DWC3_RESET_TIMEOUT_US	10000

static int dwc3_core_soft_reset(const struct device *dev)
{
	uint32_t dctl;

	dctl = dwc3_read32(dev, DWC3_DCTL);
	dwc3_write32(dev, DWC3_DCTL, dctl | DWC3_DCTL_CSFTRST);

	for (int us = 0; us < DWC3_RESET_TIMEOUT_US; us += 10) {
		if ((dwc3_read32(dev, DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0U) {
			LOG_DBG("core soft reset done in <%d us", us + 10);
			return 0;
		}
		k_busy_wait(10);
	}

	LOG_ERR("core soft reset timed out (DCTL=0x%08x)", dwc3_read32(dev, DWC3_DCTL));
	return -ETIMEDOUT;
}

/*
 * udc_init() 에서 호출.
 *
 * 이번 [3] 단계의 목표는 "레지스터 접근이 실제로 된다" 를 증명하는 것이다.
 *   1) 레지스터 블록을 매핑
 *   2) GSNPSID 로 DWC3 코어인지 확인 — 아니면 즉시 중단(베이스 주소 오류
 *      상태에서 이후 레지스터를 쓰면 엉뚱한 곳을 망가뜨린다)
 *   3) GHWPARAMS3 로 실제 엔드포인트 수를 읽어 DT 잠정값과 대조
 *   4) 코어 소프트리셋 (타임아웃 있음)
 *
 * PHY 설정(GUSB2PHYCFG) · 포트 방향(GCTL.PRTCAPDIR) · 속도(DCFG.DEVSPD) 는
 * 아직 건드리지 않는다. U-Boot 가 이미 PHY 를 올려 두었을 가능성이 크고,
 * 잘못 건드리면 열거 자체를 못 보게 된다. [4] 이후로 미룬다.
 */
/*
 * PHY 가 보고 있는 상태를 해독해 찍는다.
 * 출처: RK3568 TRM Part1 V1.3 3.9절 USBPHY_U3_GRF_STATUS (0x00C0).
 *
 * linestate 는 D+/D- 의 전기 상태다 — HS/FS 문제를 PHY 수준에서 본다.
 *   0=SE0, 1=J, 2=K, 3=SE1
 * 디바이스가 호스트에 붙어 있으면 vbusvalid/bvalid 가 서고 sessend 는
 * 내려가 있어야 한다. iddig=1 이 B-device(주변장치)다.
 */
static void dwc3_rk_log_phy_status(const struct device *dev, const char *when)
{
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t st;

	if (priv->phy_grf == 0U) {
		return;
	}

	st = sys_read32(priv->phy_grf + DWC3_RK_PHY_STATUS);
	LOG_INF("PHY STATUS[%s]=0x%08x linestate=%u vbusvalid=%u bvalid=%u avalid=%u sessend=%u iddig=%u hostdisc=%u lsfsrcv=%u",
		when, st,
		(unsigned int)FIELD_GET(DWC3_RK_ST_LINESTATE, st),
		(st & DWC3_RK_ST_VBUSVALID) ? 1U : 0U,
		(st & DWC3_RK_ST_BVALID) ? 1U : 0U,
		(st & DWC3_RK_ST_AVALID) ? 1U : 0U,
		(st & DWC3_RK_ST_SESSEND) ? 1U : 0U,
		(st & DWC3_RK_ST_IDDIG) ? 1U : 0U,
		(st & DWC3_RK_ST_HOSTDISC) ? 1U : 0U,
		(st & DWC3_RK_ST_LS_FS_RCV) ? 1U : 0U);
}

/*
 * 진단 스냅샷 — 셸이 한 번에 읽는다 (udc_dwc3_diag.h).
 *
 * ⚠ 레지스터를 **읽기만** 한다. 드라이버 동작에 영향이 없다. 읽는 사이
 *   링크 상태가 바뀌어 찢긴 값을 보는 것을 줄이려고 한 번에 모아 준다.
 */
int udc_dwc3_diag_get(const struct device *dev, struct udc_dwc3_diag *out)
{
	struct udc_dwc3_data *priv;

	if (out == NULL) {
		return -EINVAL;
	}
	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	priv = udc_get_private(dev);
	if (priv == NULL || priv->base == 0) {
		return -ENODEV;
	}

	out->gsnpsid      = dwc3_read32(dev, DWC3_GSNPSID);
	out->ghwparams3   = dwc3_read32(dev, DWC3_GHWPARAMS3);
	out->hw_num_eps   = priv->hw_num_eps;
	out->gctl         = dwc3_read32(dev, DWC3_GCTL);
	out->dctl         = dwc3_read32(dev, DWC3_DCTL);
	out->dcfg         = dwc3_read32(dev, DWC3_DCFG);
	out->dsts         = dwc3_read32(dev, DWC3_DSTS);
	out->dalepena     = dwc3_read32(dev, DWC3_DALEPENA);
	out->gusb2phycfg0 = dwc3_read32(dev, DWC3_GUSB2PHYCFG(0));
	out->ep0_state    = (uint8_t)priv->ep0_state;
	out->ep0_wlength  = priv->ep0_wlength;

	/* 벌크 EP mps — HS(512) 인지 FS(64) 인지 판정용 */
	{
		static const uint8_t addrs[4] = { 0x82U, 0x01U, 0x84U, 0x02U };
		uint16_t *dst[4] = { &out->bulk_in_mps, &out->bulk_out_mps,
				     &out->bulk_in1_mps, &out->bulk_out1_mps };

		for (int i = 0; i < 4; i++) {
			struct udc_ep_config *c = udc_get_ep_cfg(dev, addrs[i]);

			*dst[i] = (c != NULL) ? c->mps : 0U;
		}
	}

	out->stats        = priv->stats;

	return 0;
}

static int udc_dwc3_init(const struct device *dev)
{
	const struct udc_dwc3_config *config = dev->config;
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint32_t id, hwp3;
	uint16_t core_id;
	int err;

	if (priv->base == 0U) {
		device_map(&priv->base, config->phys, config->size, K_MEM_CACHE_NONE);
		LOG_DBG("mapped 0x%lx -> 0x%lx (%zu bytes)",
			(unsigned long)config->phys, (unsigned long)priv->base,
			config->size);
	}

	id = dwc3_read32(dev, DWC3_GSNPSID);
	core_id = FIELD_GET(DWC3_GSNPSID_CORE_ID, id);

	if (core_id != DWC3_CORE_ID_DWC3) {
		/*
		 * ⚠ 0 이 읽히면 '주소가 틀린 것' 이 아니라 '블록이 죽은 것'
		 *   일 수 있다 — 클럭이 게이팅된 레지스터 블록은 전부 0 이다.
		 *   실제로 boot.scr 의 'usb stop' 이 U-Boot 의 dwc3 장치를
		 *   제거하며 클럭을 내려 이 증상이 났다 (Zephyr 에 CRU
		 *   드라이버가 없어 되살릴 수 없다).
		 */
		LOG_ERR("not a DWC3 core: GSNPSID=0x%08x (core_id=0x%04x)", id, core_id);
		LOG_ERR("  GHWPARAMS0=0x%08x GCTL=0x%08x GUSB2PHYCFG=0x%08x DCFG=0x%08x",
			dwc3_read32(dev, DWC3_GHWPARAMS0),
			dwc3_read32(dev, DWC3_GCTL),
			dwc3_read32(dev, DWC3_GUSB2PHYCFG(0)),
			dwc3_read32(dev, DWC3_DCFG));
		LOG_ERR("  전부 0 이면 클럭 게이팅/리셋 의심, 일부만 0 이면 베이스 주소 의심");
		return -ENODEV;
	}

	LOG_INF("DWC3 core 0x%04x release 0x%04x",
		core_id, (unsigned int)FIELD_GET(DWC3_GSNPSID_RELEASE, id));

	hwp3 = dwc3_read32(dev, DWC3_GHWPARAMS3);
	priv->hw_num_eps = FIELD_GET(DWC3_GHWPARAMS3_NUM_EPS, hwp3);

	/*
	 * ⚠ 단위를 맞춰 비교한다 (2026-11-06 정정).
	 *
	 * 예전 코드는 hw_num_eps(HW 의 **물리/단방향** EP 수)와
	 * config->num_of_eps(DT 의 **양방향** 수)를 바로 비교해, 실제로는
	 * 맞는 구성에서도 매 부팅마다 경고를 냈다:
	 *   "endpoint count mismatch: HW=16, DT num-bidir-endpoints=8"
	 * 이 보드는 GHWPARAMS3.NUM_EPS = 16 이고 DT 는 8 양방향 = 16 물리
	 * 이므로 **일치**한다. 그 경고가 OTA 작업 중 "EP 부족" 을 의심하게
	 * 만들어 왕복을 쓰게 했다 (docs/hardware-verified.md 의 기각된 가설).
	 *
	 * 그리고 방향을 구분한다 — DT 가 HW 보다 **많이** 선언하면 없는 EP 를
	 * 쓰려 하므로 오류이고, **적게** 선언하면 보수적이라 무해하다.
	 */
	if (DWC3_NUM_PHYS_EPS > priv->hw_num_eps) {
		LOG_ERR("DT 가 HW 보다 많은 EP 를 선언했다 — "
			"DT %u 물리(num-bidir-endpoints=%u) > HW %u",
			(unsigned int)DWC3_NUM_PHYS_EPS,
			(unsigned int)config->num_of_eps,
			priv->hw_num_eps);
	} else if (DWC3_NUM_PHYS_EPS < priv->hw_num_eps) {
		LOG_INF("endpoints: DT %u 물리 사용 / HW %u 보유 (IN %u) — 여유 있음",
			(unsigned int)DWC3_NUM_PHYS_EPS, priv->hw_num_eps,
			(unsigned int)FIELD_GET(DWC3_GHWPARAMS3_NUM_IN_EPS, hwp3));
	} else {
		LOG_INF("endpoints: %u 물리 (DT %u 양방향, HW 와 일치, IN %u)",
			priv->hw_num_eps, (unsigned int)config->num_of_eps,
			(unsigned int)FIELD_GET(DWC3_GHWPARAMS3_NUM_IN_EPS, hwp3));
	}

	err = dwc3_core_soft_reset(dev);
	if (err != 0) {
		return err;
	}

	/*
	 * 새 설정 시작을 알린다. COMMANDPARAM 의 리소스 인덱스는 첫 설정이라 0.
	 * 실제 EP 설정(DEPCFG/DEPXFERCFG)은 TRB 가 준비되는 [7] 에서 한다 —
	 * 지금 넣으면 TRB 없이 반쪽이고 검증도 안 된다.
	 */
	{
		uint32_t st = 0;

		err = dwc3_dep_cmd(dev, 0, DWC3_DEPCMD_DEPSTARTCFG,
				   0U, 0U, 0U, &st);
		if (err != 0) {
			return err;
		}
		LOG_INF("DEPSTARTCFG ok, CMDSTATUS=0x%x", st);
	}

	/*
	 * TRB 링 자체 점검.
	 *
	 * 하드웨어는 아직 이 링을 모른다(DEPSTRTXFER 미발행). 그래서 지금
	 * fill/reclaim 을 돌려보는 것은 안전하고, 다음 세 가지를 실기 첫
	 * 부팅에서 바로 확인할 수 있다:
	 *   - 구조체 레이아웃이 16바이트인가
	 *   - 16바이트 정렬이 지켜졌는가 (하드웨어 요구)
	 *   - flush/invalidate 가 실제로 왕복하는가
	 *     (fill 이 HWO 를 세웠으므로 reclaim 은 -EBUSY 여야 정상)
	 */
	{
		struct dwc3_trb *t = DWC3_EP0_TRB(DWC3_EP0_OUT, 0);
		uintptr_t tphys = k_mem_phys_addr(DWC3_EP0_TRB(DWC3_EP0_OUT, 0));
		uint32_t trbsts = 0;
		int rc;

		BUILD_ASSERT(sizeof(struct dwc3_trb) == 16,
			     "DWC3 TRB must be exactly 16 bytes");

		dwc3_trb_fill(t, 0U, 0U, DWC3_TRBCTL_NORMAL);
		rc = dwc3_trb_reclaim(t, &trbsts);

		LOG_INF("TRB ring: phys=0x%lx align16=%s selftest=%s",
			(unsigned long)tphys,
			((tphys & 0xfUL) == 0UL) ? "ok" : "BAD",
			(rc == -EBUSY) ? "ok (HWO held)" : "UNEXPECTED");

		/* 점검 흔적을 지운다 — 하드웨어가 보기 전에 */
		t->control = 0U;
		sys_cache_data_flush_range(t, sizeof(*t));
	}

	/*
	 * 아래 네 묶음은 '순서가 곧 내용' 이다. 한 번 틀렸으므로 적어 둔다:
	 *
	 *   1) PHY PLL 깨움   — 리셋 전에 PHY 에 클럭이 있어야 한다
	 *   2) 코어+PHY 리셋  — GCTL 을 초기화한다
	 *   3) SUSPHY 해제     — 리셋이 되돌려 놓으므로 리셋 뒤에
	 *   4) PRTCAPDIR       — 리셋이 지우므로 반드시 맨 뒤에
	 *
	 * ⚠ 처음에는 PRTCAPDIR 를 리셋보다 앞에 두었다. 코어 소프트리셋이
	 *   GCTL 을 초기화하므로 방금 쓴 PRTCAPDIR 가 그대로 지워졌다.
	 *   실기 콘솔 없이 네 번 시험하는 동안 이 사실이 보이지 않았다.
	 */

	/*
	 * ⚠ PHY 를 peripheral 전용으로 못 박는다 — 임시 조치다.
	 *
	 *   이 PHY 는 dwc3 와 '별개의 장치' 이고 GRF(General Register File)
	 *   레지스터로 제어된다. Zephyr 에는 Rockchip PHY 드라이버가 없고
	 *   (인트리 rk3568.dtsi 에 gic 와 uart2 뿐) 우리 DT 도 PHY 를
	 *   참조하지 않는다. PLL 이 suspend 상태면 dwc3 코어를 device 로
	 *   돌려놔도 버스를 구동하지 못해 열거가 일어나지 않는다.
	 *
	 *   USB 디바이스로 동작하려면 컨트롤러가 두 가지를 알아야 한다:
	 *
	 *   1) VBUS 가 유효하다 (bvalid/vbusvalid)
	 *      없으면 호스트에 붙었다는 것을 모르고 D+ 풀업과 HS 협상을
	 *      끝내지 못한다. reset 상태(CON2 비트15=0)에서는 PHY 의 자체
	 *      VBUS 감지에 의존하는데, 이 보드의 USB-C 포트에서는 그 경로가
	 *      연결되지 않은 것으로 보인다 — 실기에서 CONNECTSPD 가 HS(0)와
	 *      FS(1) 사이를 23:13 으로 요동쳤다.
	 *      → CON2 비트15(sel)=1, 비트14(값)=1 로 강제한다.
	 *
	 *   2) 자신이 B-device(주변장치)다 (iddig)
	 *      reset 상태(CON0 비트9=0)에서는 PHY 의 물리 ID 핀을 본다.
	 *      USB-C 에는 ID 핀이 없어 그 값이 떠 있거나 A-device(호스트)로
	 *      읽힐 수 있다.
	 *      → CON0 비트9(sel)=1, 비트10(iddig)=1 로 강제한다.
	 *
	 *   출처: RK3568 TRM Part1 V1.3 3.9절 (419쪽). 비트 이름 그대로:
	 *     CON0: usbotg_utmi_iddig / usbotg_utmi_iddig_sel
	 *     CON2: bvalid_vbusvalid_grf / bvalid_vbusvalid_sel
	 *
	 * ⚠ 이전에 여기서 CON2 에 0x00100000 을 썼다 (FreeBSD rk_usb2phy.c 의
	 *   clk_ctl). TRM 을 보니 그것은 비트4 usbphy_commononn 을 0 으로 쓰는
	 *   것이고 reset 값이 이미 0 이라 사실상 무동작이었다. PHY PLL 은
	 *   애초에 suspend 가 아니었다.
	 */
	{
		mm_reg_t grf;

		device_map(&grf, DWC3_RK_USB2PHY_GRF, 0x100,
			   K_MEM_CACHE_NONE);
		priv->phy_grf = grf;

		/* B-device 로 못 박는다 */
		sys_write32(DWC3_RK_HIWORD(DWC3_RK_CON0_IDDIG |
					   DWC3_RK_CON0_IDDIG_SEL),
			    grf + DWC3_RK_PHY_CON0);

		/* VBUS 유효를 컨트롤러에 강제로 알린다 */
		sys_write32(DWC3_RK_HIWORD(DWC3_RK_CON2_BV_SEL |
					   DWC3_RK_CON2_BV_GRF),
			    grf + DWC3_RK_PHY_CON2);

		LOG_INF("PHY peripheral 고정: CON0=0x%08x CON2=0x%08x (읽기: 0x%08x / 0x%08x)",
			DWC3_RK_HIWORD(DWC3_RK_CON0_IDDIG |
				       DWC3_RK_CON0_IDDIG_SEL),
			DWC3_RK_HIWORD(DWC3_RK_CON2_BV_SEL |
				       DWC3_RK_CON2_BV_GRF),
			sys_read32(grf + DWC3_RK_PHY_CON0),
			sys_read32(grf + DWC3_RK_PHY_CON2));
		dwc3_rk_log_phy_status(dev, "init");
	}

	/*
	 * ⚠ 코어와 PHY 를 함께 리셋한다.
	 *
	 *   U-Boot 의 preboot 가 'usb start' 를 돌려 이 컨트롤러를 '호스트'
	 *   로 올려놓고 떠난다 (U-Boot 바이너리에서 확인:
	 *   preboot=usb start; pci enum). 즉 우리가 받을 때 코어는 xhci 가
	 *   돌던 상태이고, GCTL.PRTCAPDIR 도 HOST 로 설정돼 있다.
	 *
	 *   DCTL.CSFTRST 는 '디바이스' 소프트리셋이라 그 상태를 뒤엎지
	 *   못한다. 코어 전체와 PHY 를 리셋해야 한다.
	 */
	{
		uint32_t gctl, phycfg;

		gctl = dwc3_read32(dev, DWC3_GCTL);
		dwc3_write32(dev, DWC3_GCTL, gctl | DWC3_GCTL_CORESOFTRESET);

		phycfg = dwc3_read32(dev, DWC3_GUSB2PHYCFG(0));
		dwc3_write32(dev, DWC3_GUSB2PHYCFG(0),
			     phycfg | DWC3_GUSB2PHYCFG_PHYSOFTRST);

		k_busy_wait(1000);

		dwc3_write32(dev, DWC3_GUSB2PHYCFG(0),
			     phycfg & ~DWC3_GUSB2PHYCFG_PHYSOFTRST);

		/* PHY PLL 이 다시 잠길 시간을 넉넉히 준다 */
		k_busy_wait(50000);

		gctl = dwc3_read32(dev, DWC3_GCTL);
		dwc3_write32(dev, DWC3_GCTL,
			     gctl & ~DWC3_GCTL_CORESOFTRESET);

		LOG_INF("core+PHY reset 완료 (GCTL=0x%08x GUSB2PHYCFG=0x%08x)",
			dwc3_read32(dev, DWC3_GCTL),
			dwc3_read32(dev, DWC3_GUSB2PHYCFG(0)));
	}

	/*
	 * ⚠ GUSB2PHYCFG.SUSPENDUSB20 (BIT 6) 을 '무조건' 끈다.
	 *
	 *   이 비트가 1 이면 UTMI+ PHY 가 suspend 에 머물러 D+ 풀업을 걸지
	 *   않는다. 호스트에는 '빈 포트' 로 보인다 — 열거 시도조차 없는
	 *   증상이 정확히 이것이다.
	 *
	 *   dwc3_dep_cmd_ioc() 에도 같은 비트를 다루는 코드가 있지만, 그쪽은
	 *   명령을 내는 구간에만 껐다가 '원래 값으로 되돌린다'. U-Boot 가
	 *   호스트 모드로 켜 둔 값이면 되돌린 뒤 다시 suspend 가 된다.
	 *   그래서 init 에서 끄고 계속 꺼진 채로 두어야 한다.
	 *
	 *   ENBLSLPM 도 함께 끈다 — L1 진입이 같은 효과를 낸다.
	 */
	{
		uint32_t phycfg = dwc3_read32(dev, DWC3_GUSB2PHYCFG(0));
		uint32_t before = phycfg;

		phycfg &= ~(DWC3_GUSB2PHYCFG_SUSPENDUSB20 |
			    DWC3_GUSB2PHYCFG_ENBLSLPM);
		dwc3_write32(dev, DWC3_GUSB2PHYCFG(0), phycfg);

		LOG_INF("GUSB2PHYCFG 0x%08x -> 0x%08x (SUSPHY %u->0, ENBLSLPM %u->0)",
			before, dwc3_read32(dev, DWC3_GUSB2PHYCFG(0)),
			(before & DWC3_GUSB2PHYCFG_SUSPENDUSB20) ? 1U : 0U,
			(before & DWC3_GUSB2PHYCFG_ENBLSLPM) ? 1U : 0U);
	}

	/*
	 * ⚠ 포트 방향을 device 로 못 박는다 — 이것이 없으면 컨트롤러가
	 *   USB 디바이스로 동작하지 않는다.
	 *
	 *   GCTL.PRTCAPDIR 는 코어가 호스트로 설지 디바이스로 설지를
	 *   정한다 (HOST=1 / DEVICE=2 / OTG=3). U-Boot 는 이 컨트롤러를
	 *   호스트나 OTG 로 두고 떠나므로, 우리가 바꾸지 않으면 호스트가
	 *   아무리 열거를 시도해도 응답하지 않는다.
	 *
	 *   반드시 모든 리셋 뒤에 쓴다 — 리셋이 이 필드를 되돌린다.
	 */
	{
		uint32_t gctl = dwc3_read32(dev, DWC3_GCTL);
		uint32_t before = gctl;

		gctl &= ~DWC3_GCTL_PRTCAPDIR;
		gctl |= FIELD_PREP(DWC3_GCTL_PRTCAPDIR,
				   DWC3_GCTL_PRTCAP_DEVICE);
		dwc3_write32(dev, DWC3_GCTL, gctl);

		LOG_INF("GCTL 0x%08x -> 0x%08x (PRTCAPDIR %u -> %u)",
			before, dwc3_read32(dev, DWC3_GCTL),
			(unsigned int)FIELD_GET(DWC3_GCTL_PRTCAPDIR, before),
			(unsigned int)FIELD_GET(DWC3_GCTL_PRTCAPDIR,
						dwc3_read32(dev, DWC3_GCTL)));
	}

	return 0;
}

static int udc_dwc3_shutdown(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

static void udc_dwc3_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_dwc3_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

/*
 * 드라이버 pre-init. 하드웨어는 건드리지 않고 엔드포인트 설정만 등록한다.
 * UDC 공통 계층이 이 시점의 ep 능력 정보를 보고 USB 스택에 보고한다.
 */
static int udc_dwc3_driver_preinit(const struct device *dev)
{
	const struct udc_dwc3_config *config = dev->config;
	struct udc_data *data = dev->data;
	struct udc_dwc3_data *priv = udc_get_private(dev);
	uint16_t mps;
	int err;

	k_mutex_init(&data->mutex);

	data->caps.rwup = true;
	data->caps.mps0 = UDC_MPS0_64;
	/*
	 * DWC3 는 SET_ADDRESS 의 주소를 '상태 단계 전에' DCFG.DEVADDR 에
	 * 써야 한다. 이 플래그가 없으면 스택이 상태 단계가 끝난 뒤에
	 * udc_set_address() 를 부르고(usbd_ch9.c 의 post_status_stage),
	 * 그러면 새 주소로 오는 호스트의 다음 요청을 받지 못한다.
	 */
	data->caps.addr_before_status = true;

	/* DT_ENUM_IDX(maximum_speed): 0=low 1=full 2=high 3=super */
	if (config->speed_idx >= 2) {
		data->caps.hs = true;
		mps = 512;	/* HS bulk 최대 패킷 */
	} else {
		mps = 64;	/* FS bulk 최대 패킷 */
	}

	for (unsigned int i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_out[i].caps.out = 1;
		if (i == 0) {
			config->ep_cfg_out[i].caps.control = 1;
			config->ep_cfg_out[i].caps.mps = 64;
		} else {
			config->ep_cfg_out[i].caps.bulk = 1;
			config->ep_cfg_out[i].caps.interrupt = 1;
			/*
			 * ISO 는 범위 밖이다. 켜두면 USB 스택이 ISO 전송을
			 * 들이밀 수 있는데 이 드라이버는 처리하지 못한다.
			 */
			config->ep_cfg_out[i].caps.iso = 0;
			config->ep_cfg_out[i].caps.mps = mps;
		}

		config->ep_cfg_out[i].addr = USB_EP_DIR_OUT | i;
		err = udc_register_ep(dev, &config->ep_cfg_out[i]);
		if (err != 0) {
			LOG_ERR("Failed to register OUT endpoint %u", i);
			return err;
		}
	}

	for (unsigned int i = 0; i < config->num_of_eps; i++) {
		config->ep_cfg_in[i].caps.in = 1;
		if (i == 0) {
			config->ep_cfg_in[i].caps.control = 1;
			config->ep_cfg_in[i].caps.mps = 64;
		} else {
			config->ep_cfg_in[i].caps.bulk = 1;
			config->ep_cfg_in[i].caps.interrupt = 1;
			config->ep_cfg_in[i].caps.iso = 0;
			config->ep_cfg_in[i].caps.mps = mps;
		}

		config->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, &config->ep_cfg_in[i]);
		if (err != 0) {
			LOG_ERR("Failed to register IN endpoint %u", i);
			return err;
		}
	}

	/*
	 * num_of_eps 는 DT 의 num-bidir-endpoints 에서 온다. RK356x 의 실제
	 * 물리 엔드포인트 수는 [3] 에서 GHWPARAMS3 를 읽어 확인한 뒤 맞춰야
	 * 한다 — 지금 값은 잠정이다. 이 로그가 그때의 비교 기준이 된다.
	 */
	LOG_INF("dwc3 @0x%lx: eps=%u (DT provisional), speed_idx=%d, hs=%d",
		(unsigned long)config->phys, (unsigned int)config->num_of_eps,
		config->speed_idx, (int)data->caps.hs);

	/*
	 * 아래쪽 절반을 돌릴 스레드. ISR 은 깃발만 올리고 실제 이벤트 처리는
	 * 이 스레드가 한다 — udc_setup_received() 가 스레드 문맥 전용이다.
	 */
	k_event_init(&priv->drv_evt);
	priv->ep0_state = DWC3_EP0_SETUP;
	config->make_thread(dev);

	return 0;
}

/*
 * UDC API 구조체.
 * 기본 인자 검증은 udc_common.c 가 해 주므로 드라이버에서 중복 검사하지
 * 않는다. 아직 구현하지 않은 것은 0 이 아니라 -ENOTSUP 을 돌려준다 —
 * 0 을 돌려주면 상위 계층이 성공으로 오해한다.
 */
static const struct udc_api udc_dwc3_api = {
	.lock = udc_dwc3_lock,
	.unlock = udc_dwc3_unlock,
	.device_speed = udc_dwc3_device_speed,
	.init = udc_dwc3_init,
	.enable = udc_dwc3_enable,
	.disable = udc_dwc3_disable,
	.shutdown = udc_dwc3_shutdown,
	.set_address = udc_dwc3_set_address,
	.host_wakeup = udc_dwc3_host_wakeup,
	.ep_enable = udc_dwc3_ep_enable,
	.ep_disable = udc_dwc3_ep_disable,
	.ep_set_halt = udc_dwc3_ep_set_halt,
	.ep_clear_halt = udc_dwc3_ep_clear_halt,
	.ep_enqueue = udc_dwc3_ep_enqueue,
	.ep_dequeue = udc_dwc3_ep_dequeue,
};

/*
 * UDC 드라이버는 플랫폼이 한 인스턴스만 쓰더라도 항상 다중 인스턴스로
 * 구현한다 (Zephyr 관례).
 *
 * 주의: 인트리 udc_skeleton.c 의 같은 매크로는 ep_cfg_in / ep_cfg_out
 *       배열이 서로 뒤바뀌어 대입돼 있다. 스텁 단계에서는 드러나지 않고
 *       EP0 구현 단계에서 간헐적 오동작으로 나타나므로 여기서는 바로잡는다.
 */
#define UDC_DWC3_DEVICE_DEFINE(n)						\
	static struct udc_ep_config						\
		udc_dwc3_ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];	\
	static struct udc_ep_config						\
		udc_dwc3_ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)];	\
										\
	K_THREAD_STACK_DEFINE(udc_dwc3_stack_##n,				\
			      CONFIG_UDC_DWC3_STACK_SIZE);			\
										\
	static void udc_dwc3_thread_##n(void *dev, void *arg1, void *arg2)	\
	{									\
		ARG_UNUSED(arg1);						\
		ARG_UNUSED(arg2);						\
		while (true) {							\
			dwc3_thread_handler(dev);				\
		}								\
	}									\
										\
	static void udc_dwc3_make_thread_##n(const struct device *dev)		\
	{									\
		struct udc_dwc3_data *priv = udc_get_private(dev);		\
										\
		k_thread_create(&priv->thread_data,				\
				udc_dwc3_stack_##n,				\
				K_THREAD_STACK_SIZEOF(udc_dwc3_stack_##n),	\
				udc_dwc3_thread_##n,				\
				(void *)dev, NULL, NULL,			\
				K_PRIO_COOP(CONFIG_UDC_DWC3_THREAD_PRIORITY),	\
				0, K_NO_WAIT);					\
		k_thread_name_set(&priv->thread_data, dev->name);		\
	}									\
										\
	static void udc_dwc3_irq_connect_##n(void)				\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    udc_dwc3_isr, DEVICE_DT_INST_GET(n), 0);		\
		irq_enable(DT_INST_IRQN(n));					\
	}									\
										\
	static const struct udc_dwc3_config udc_dwc3_config_##n = {		\
		.phys = DT_INST_REG_ADDR(n),					\
		.size = DT_INST_REG_SIZE(n),					\
		.num_of_eps = DT_INST_PROP(n, num_bidir_endpoints),		\
		.ep_cfg_in = udc_dwc3_ep_cfg_in_##n,				\
		.ep_cfg_out = udc_dwc3_ep_cfg_out_##n,				\
		.irq_connect = udc_dwc3_irq_connect_##n,			\
		.make_thread = udc_dwc3_make_thread_##n,			\
		.speed_idx = DT_ENUM_IDX_OR(DT_DRV_INST(n), maximum_speed, 2),	\
	};									\
										\
	static struct udc_dwc3_data udc_dwc3_priv_##n = {			\
	};									\
										\
	static struct udc_data udc_dwc3_data_##n = {				\
		.mutex = Z_MUTEX_INITIALIZER(udc_dwc3_data_##n.mutex),		\
		.priv = &udc_dwc3_priv_##n,					\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, udc_dwc3_driver_preinit, NULL,			\
			      &udc_dwc3_data_##n, &udc_dwc3_config_##n,		\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      &udc_dwc3_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_DWC3_DEVICE_DEFINE)
