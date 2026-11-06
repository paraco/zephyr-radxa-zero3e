/*
 * USB device 구성 — CDC-ACM 두 개 (셸 + OTA 데이터).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ── 왜 직접 구성하는가 (2026-11-06) ──────────────────────────────────
 *
 * `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 이 하던 일을 앱으로 가져온다.
 * 그 자동 경로는 **CDC-ACM 인스턴스 하나만** 등록한다 —
 * `subsys/usb/device_next/app/cdc_acm_serial.c:65` 가
 * `usbd_register_class(..., "cdc_acm_0", ...)` 를 부르고 함수 이름도
 * `register_cdc_acm_0` 다. 그 Kconfig 의 help 도 "다른 USB 기능이 필요
 * 없는 앱의 기본 시리얼 백엔드 전용" 이라고 적고 있다.
 *
 * 실기에서 확인한 증상: `cdc_acm_uart1` 노드를 DT 에 추가하면 드라이버
 * 인스턴스는 생겨서 `device_is_ready()` 가 true 인데, USB 디스크립터에는
 * 인터페이스가 2개(CDC 하나)뿐이고 호스트에 포트가 하나만 보였다.
 *
 * 구조는 위 `cdc_acm_serial.c` (Apache-2.0, Copyright (c) 2024 Nordic
 * Semiconductor ASA) 를 본보기로 했다. 같은 라이선스라 문제가 없고, 베낀
 * 것이 아니라 필요한 차이를 두었다:
 *   - `usbd_register_class` 로 이름을 하나씩 대지 않고
 *     **`usbd_register_all_classes()`** 를 쓴다. 사용 가능한 인스턴스를
 *     전부 등록하므로 CDC 를 더 늘려도 이 파일을 고치지 않는다
 *     (include/zephyr/usb/usbd.h:892). ⚠ 그 주석대로 두 API 를 섞으면
 *     실패하므로 `usbd_register_class` 는 쓰지 않는다
 *   - VID·PID·문자열을 이 파일에서 정한다. `CDC_ACM_SERIAL_*` Kconfig 는
 *     `INITIALIZE_AT_BOOT` 하위라 그것을 끄면 사라진다
 *
 * ⚠ `usbd_device_set_code_triple(..., USB_BCC_MISCELLANEOUS, 0x02, 0x01)` 이
 *   필수다. 복합 장치(IAD, Interface Association Descriptor)임을 호스트에
 *   알리는 코드 트리플이고, 이것이 없으면 CDC 가 둘일 때 호스트가 두 번째를
 *   잡지 못한다.
 */

#include <stdint.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(moru_usbd, CONFIG_USBD_LOG_LEVEL);

/*
 * VID 0x2fe3 은 Zephyr 프로젝트의 테스트용 VID 다 (Nordic 이 Zephyr 에
 * 기부한 것으로, 인트리 샘플이 모두 이 값을 쓴다). 제품을 외부에 내려면
 * 자체 VID/PID 가 필요하다 — 지금은 bring-up 이다.
 *
 * PID 0x0005 는 기존 세 진단 앱(0x0004)과 호스트 캐시가 섞이지 않게
 * 분리한 값이다.
 */
#define MORU_USB_VID	0x2fe3
#define MORU_USB_PID	0x0005

USBD_DEVICE_DEFINE(moru_uds, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   MORU_USB_VID, MORU_USB_PID);

USBD_DESC_LANG_DEFINE(moru_lang);
USBD_DESC_MANUFACTURER_DEFINE(moru_mfr, "MoruLab");
USBD_DESC_PRODUCT_DEFINE(moru_product, "MoruLab Zephyr RTOS");
/* 직렬번호는 hwinfo(OTP 칩 ID)에서 온다 — drivers/hwinfo/hwinfo_rk3566.c */
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(moru_sn)));

USBD_DESC_CONFIG_DEFINE(moru_fs_cfg_desc, "MoruLab FS");
USBD_DESC_CONFIG_DEFINE(moru_hs_cfg_desc, "MoruLab HS");

/* 버스 전원. 보드가 USB 에서 전원을 받지 않으므로 self-powered 가 맞지만,
 * bring-up 중에는 호스트 쪽 판단을 단순하게 두려고 기본값(0)을 쓴다. */
USBD_CONFIGURATION_DEFINE(moru_fs_config, 0, 250, &moru_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(moru_hs_config, 0, 250, &moru_hs_cfg_desc);

static int moru_add_speed(const enum usbd_speed speed)
{
	struct usbd_config_node *cfg_nd =
		(speed == USBD_SPEED_HS) ? &moru_hs_config : &moru_fs_config;
	int err;

	err = usbd_add_configuration(&moru_uds, speed, cfg_nd);
	if (err) {
		LOG_ERR("설정 추가 실패 (speed %d, err %d)", (int)speed, err);
		return err;
	}

	/* 사용 가능한 클래스 인스턴스를 전부 — CDC-ACM 둘이 여기서 들어온다 */
	err = usbd_register_all_classes(&moru_uds, speed, 1, NULL);
	if (err) {
		LOG_ERR("클래스 등록 실패 (speed %d, err %d)", (int)speed, err);
		return err;
	}

	/* 복합 장치(IAD) 표시 — 없으면 호스트가 두 번째 CDC 를 잡지 못한다 */
	err = usbd_device_set_code_triple(&moru_uds, speed,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (err) {
		LOG_ERR("코드 트리플 설정 실패 (speed %d, err %d)", (int)speed, err);
	}

	return err;
}

static int moru_usbd_init(void)
{
	int err;

	err = usbd_add_descriptor(&moru_uds, &moru_lang);
	if (err) {
		LOG_ERR("언어 디스크립터 실패 (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&moru_uds, &moru_mfr);
	if (err) {
		LOG_ERR("제조사 디스크립터 실패 (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&moru_uds, &moru_product);
	if (err) {
		LOG_ERR("제품 디스크립터 실패 (%d)", err);
		return err;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		err = usbd_add_descriptor(&moru_uds, &moru_sn);
	))
	if (err) {
		LOG_ERR("직렬번호 디스크립터 실패 (%d)", err);
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&moru_uds) == USBD_SPEED_HS) {
		err = moru_add_speed(USBD_SPEED_HS);
		if (err) {
			return err;
		}
	}

	err = moru_add_speed(USBD_SPEED_FS);
	if (err) {
		return err;
	}

	err = usbd_init(&moru_uds);
	if (err) {
		LOG_ERR("usbd_init 실패 (%d)", err);
		return err;
	}

	err = usbd_enable(&moru_uds);
	if (err) {
		LOG_ERR("usbd_enable 실패 (%d)", err);
		return err;
	}

	LOG_INF("usbd: CDC-ACM 2개 등록 (셸 + OTA), VID %04x PID %04x",
		MORU_USB_VID, MORU_USB_PID);
	return 0;
}

/*
 * 자동 경로와 같은 시점에 돈다 (APPLICATION / APPLICATION_INIT_PRIORITY).
 * moru_mark.c 의 "[<U]" · "[U>]" 마커가 이 전후를 감싸므로, 여기서 죽으면
 * UART2 에서 "[<U]" 까지만 보인다.
 */
SYS_INIT(moru_usbd_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
