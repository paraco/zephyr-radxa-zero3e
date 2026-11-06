/*
 * Radxa ZERO 3E (RK3566) — OTP 에서 칩 고유 ID 를 읽는다.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr 의 USB 스택은 직렬번호 문자열 디스크립터를
 * hwinfo_get_device_id() 로 만든다 (usbd_ch9.c 의 get_sn_from_hwid).
 * Rockchip 용 hwinfo 드라이버가 인트리에 없어 이것을 만들었다.
 *
 * ── 왜 이렇게 손이 많이 가는가 ───────────────────────────────────────
 * Zephyr 에는 이 SoC 의 클럭·리셋 드라이버가 없다. 그래서 OTP 컨트롤러를
 * 쓰려면 CRU 레지스터를 직접 써서 클럭 게이트를 풀고 소프트리셋을 때려야
 * 한다. 아래 주소·비트는 모두 출처를 달아 두었다.
 *
 * ── 출처 ─────────────────────────────────────────────────────────────
 * CRU 와 OTP 의 주소, 클럭 게이트·소프트리셋 비트:
 *   Rockchip RK3568 TRM Part1 V1.3
 *     메모리맵 (CRU_NS 0xFDD20000, OTP_NS 0xFE38C000)
 *     CRU_GATE_CON26 (0x0368) · CON34 (0x0388)
 *     CRU_SOFTRST_CON22 (0x0458) · CON28 (0x0470)
 *   게이트는 "When high, disable clock", 리셋은 "When high, reset
 *   relative logic" 이고 모두 hiword-mask 다 (상위 16비트가 비트별 쓰기
 *   허용).
 *
 * ⚠ OTP 컨트롤러 자체의 레지스터와 읽기 절차는 공개 TRM 양쪽(Part1/Part2)
 *   에 없다. Part2 의 33개 장을 모두 확인했고 OTP 장이 없다. 그래서
 *   레지스터 오프셋·비트 위치·절차는 Linux 의
 *   drivers/nvmem/rockchip-otp.c (GPL-2.0, Copyright (c) 2018 Rockchip,
 *   Finley Xiao) 에서 '사실로서' 확인했다. 코드는 베끼지 않고 이 파일에
 *   맞게 직접 작성했다. 하드웨어 레지스터 번호는 저작 표현이 아니라
 *   사실이라는 판단이다.
 *
 * 칩 ID 의 OTP 내 위치는 Linux 의 디바이스트리에서 확인했다 —
 *   arch/arm64/boot/dts/rockchip/rk356x-base.dtsi 의
 *   otp: efuse@fe38c000 { nvmem-layout { otp_id: id@a { reg = <0x0a 0x10>; } } }
 *   즉 바이트 오프셋 0x0A 에서 16바이트. rk3568 계열의 OTP 는 16비트
 *   워드 단위로 읽으므로 워드 주소 5 에서 8워드를 읽는다.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(hwinfo_rk3566, CONFIG_HWINFO_LOG_LEVEL);

/* ── CRU (TRM Part1: 메모리맵, GATE_CON26/34, SOFTRST_CON22/28) ──────── */
#define CRU_NS_BASE		0xfdd20000UL
#define CRU_NS_SIZE		0x1000

#define CRU_GATE_CON26		0x0368
#define GATE26_CLK_OTPC_NS_USR	BIT(11)
#define GATE26_CLK_OTPC_NS_SBPI	BIT(10)
#define GATE26_PCLK_OTPC_NS	BIT(9)

#define CRU_GATE_CON34		0x0388
#define GATE34_PCLK_OTPPHY	BIT(13)

#define CRU_SOFTRST_CON22	0x0458
#define SRST22_OTPC_NS_USR	BIT(14)
#define SRST22_OTPC_NS_SBPI	BIT(13)
#define SRST22_P_OTPC_NS	BIT(12)

#define CRU_SOFTRST_CON28	0x0470
#define SRST28_OTPPHY		BIT(15)
#define SRST28_P_OTPPHY		BIT(14)

/*
 * hiword-mask: 상위 16비트에 쓰기 허용 비트를, 하위 16비트에 값을 둔다.
 * 게이트는 0 이 '켜짐', 리셋은 1 이 '리셋 걸림' 이다.
 */
#define HIWORD_SET(bits)	(((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define HIWORD_CLR(bits)	((uint32_t)(bits) << 16)

/* ── OTP 컨트롤러 (출처: 위 주석의 Linux rockchip-otp.c 사실 확인) ──── */
#define OTP_NS_BASE		0xfe38c000UL
#define OTP_NS_SIZE		0x4000

#define OTPC_SBPI_CTRL		0x0020
#define OTPC_SBPI_CMD_VALID_PRE	0x0024
#define OTPC_USER_CTRL		0x0100
#define OTPC_USER_ADDR		0x0104
#define OTPC_USER_ENABLE	0x0108
#define OTPC_USER_QP		0x0120
#define OTPC_USER_Q		0x0124
#define OTPC_INT_STATUS		0x0304
#define OTPC_SBPI_CMD0		0x1000
#define OTPC_SBPI_CMD1		0x1004

/* 이 레지스터들도 상위 16비트가 쓰기 허용이다 */
#define OTPC_USER_ADDR_MASK	GENMASK(31, 16)
#define OTPC_USE_USER		BIT(0)
#define OTPC_USE_USER_MASK	BIT(16)
#define OTPC_USER_FSM_ENABLE	BIT(0)
#define OTPC_USER_FSM_EN_MASK	BIT(16)

#define OTPC_INT_SBPI_DONE	BIT(1)
#define OTPC_INT_USER_DONE	BIT(2)

#define SBPI_DAP_ADDR		0x02U
#define SBPI_DAP_ADDR_SHIFT	8
#define SBPI_DAP_ADDR_MASK	GENMASK(31, 24)
#define SBPI_CMD_VALID_MASK	GENMASK(31, 16)
#define SBPI_DAP_CMD_WRF	0xC0U
#define SBPI_DAP_REG_ECC	0x3AU
#define SBPI_ECC_ENABLE		0x00U
#define SBPI_ENABLE		BIT(0)
#define SBPI_ENABLE_MASK	BIT(16)

/* USER_QP 의 ECC 오류 표시 */
#define OTPC_QP_ECC_FAIL	0xC0U
#define OTPC_QP_ECC_UNCORR	0x20U

#define OTPC_POLL_TIMEOUT_US	10000
#define OTPC_POLL_STEP_US	10

/* 칩 ID: 바이트 오프셋 0x0A, 16바이트 → 16비트 워드 5번지에서 8워드 */
#define OTP_ID_WORD_ADDR	5U
#define OTP_ID_WORDS		8U
#define OTP_ID_BYTES		(OTP_ID_WORDS * 2U)

static uint8_t rk_otp_id[OTP_ID_BYTES];
static bool rk_otp_id_valid;
static bool rk_otp_tried;

static int otp_wait_flag(mm_reg_t otp, uint32_t flag)
{
	for (int us = 0; us < OTPC_POLL_TIMEOUT_US; us += OTPC_POLL_STEP_US) {
		if ((sys_read32(otp + OTPC_INT_STATUS) & flag) != 0U) {
			/* 상태 비트는 같은 값을 다시 써서 지운다 */
			sys_write32(flag, otp + OTPC_INT_STATUS);
			return 0;
		}
		k_busy_wait(OTPC_POLL_STEP_US);
	}

	return -ETIMEDOUT;
}

/*
 * SBPI(직렬 버스)로 OTP 매크로의 ECC 를 켠다. rk3568 계열은 ECC 를 켠
 * 상태로 읽어야 USER_QP 의 오류 표시가 의미를 가진다.
 */
static int otp_ecc_enable(mm_reg_t otp)
{
	sys_write32(SBPI_DAP_ADDR_MASK |
		    (SBPI_DAP_ADDR << SBPI_DAP_ADDR_SHIFT),
		    otp + OTPC_SBPI_CTRL);
	sys_write32(SBPI_CMD_VALID_MASK | 0x1U, otp + OTPC_SBPI_CMD_VALID_PRE);
	sys_write32(SBPI_DAP_CMD_WRF | SBPI_DAP_REG_ECC, otp + OTPC_SBPI_CMD0);
	sys_write32(SBPI_ECC_ENABLE, otp + OTPC_SBPI_CMD1);
	sys_write32(SBPI_ENABLE_MASK | SBPI_ENABLE, otp + OTPC_SBPI_CTRL);

	return otp_wait_flag(otp, OTPC_INT_SBPI_DONE);
}

static void otp_clocks_on(mm_reg_t cru)
{
	/* 게이트는 0 이 '켜짐' 이다 */
	sys_write32(HIWORD_CLR(GATE26_CLK_OTPC_NS_USR |
			       GATE26_CLK_OTPC_NS_SBPI |
			       GATE26_PCLK_OTPC_NS),
		    cru + CRU_GATE_CON26);
	sys_write32(HIWORD_CLR(GATE34_PCLK_OTPPHY), cru + CRU_GATE_CON34);
}

static void otp_reset_pulse(mm_reg_t cru)
{
	/* 리셋은 1 이 '걸림' 이다 */
	sys_write32(HIWORD_SET(SRST22_OTPC_NS_USR | SRST22_OTPC_NS_SBPI |
			       SRST22_P_OTPC_NS),
		    cru + CRU_SOFTRST_CON22);
	sys_write32(HIWORD_SET(SRST28_OTPPHY | SRST28_P_OTPPHY),
		    cru + CRU_SOFTRST_CON28);

	k_busy_wait(5);

	sys_write32(HIWORD_CLR(SRST22_OTPC_NS_USR | SRST22_OTPC_NS_SBPI |
			       SRST22_P_OTPC_NS),
		    cru + CRU_SOFTRST_CON22);
	sys_write32(HIWORD_CLR(SRST28_OTPPHY | SRST28_P_OTPPHY),
		    cru + CRU_SOFTRST_CON28);

	k_busy_wait(5);
}

static int otp_read_id(void)
{
	mm_reg_t cru, otp;
	int err = 0;

	device_map(&cru, CRU_NS_BASE, CRU_NS_SIZE, K_MEM_CACHE_NONE);
	device_map(&otp, OTP_NS_BASE, OTP_NS_SIZE, K_MEM_CACHE_NONE);

	otp_clocks_on(cru);
	otp_reset_pulse(cru);

	err = otp_ecc_enable(otp);
	if (err != 0) {
		LOG_WRN("OTP ECC 켜기 시간 초과");
		return err;
	}

	/* USER 모드로 전환 — FSM 이 안정될 시간을 준다 */
	sys_write32(OTPC_USE_USER_MASK | OTPC_USE_USER, otp + OTPC_USER_CTRL);
	k_busy_wait(5);

	for (uint32_t i = 0; i < OTP_ID_WORDS; i++) {
		uint32_t qp, q;

		sys_write32(OTPC_USER_ADDR_MASK | (OTP_ID_WORD_ADDR + i),
			    otp + OTPC_USER_ADDR);
		sys_write32(OTPC_USER_FSM_EN_MASK | OTPC_USER_FSM_ENABLE,
			    otp + OTPC_USER_ENABLE);

		err = otp_wait_flag(otp, OTPC_INT_USER_DONE);
		if (err != 0) {
			LOG_WRN("OTP 워드 %u 읽기 시간 초과", i);
			goto out;
		}

		qp = sys_read32(otp + OTPC_USER_QP);
		if (((qp & OTPC_QP_ECC_FAIL) == OTPC_QP_ECC_FAIL) ||
		    ((qp & OTPC_QP_ECC_UNCORR) != 0U)) {
			LOG_WRN("OTP 워드 %u ECC 오류 (QP=0x%08x)", i, qp);
			err = -EIO;
			goto out;
		}

		q = sys_read32(otp + OTPC_USER_Q);
		rk_otp_id[i * 2U] = (uint8_t)(q & 0xFFU);
		rk_otp_id[i * 2U + 1U] = (uint8_t)((q >> 8) & 0xFFU);
	}

out:
	/* USER 모드 해제 */
	sys_write32(OTPC_USE_USER_MASK, otp + OTPC_USER_CTRL);

	return err;
}

/*
 * ⚠ Zephyr 의 z_impl_hwinfo_get_device_id() 는 __weak 다
 *   (drivers/hwinfo/hwinfo_weak_impl.c). 여기서 강한 심볼로 정의하면
 *   링크 때 이쪽이 쓰인다.
 *
 * OTP 는 한 번만 읽고 캐시한다 — 값이 변하지 않고, 읽기마다 컨트롤러를
 * 리셋하는 것은 불필요하다.
 */
ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	size_t n;

	if (buffer == NULL || length == 0U) {
		return -EINVAL;
	}

	if (!rk_otp_tried) {
		rk_otp_tried = true;

		if (otp_read_id() == 0) {
			rk_otp_id_valid = true;
			LOG_INF("OTP 칩 ID: %02x%02x%02x%02x%02x%02x%02x%02x"
				"%02x%02x%02x%02x%02x%02x%02x%02x",
				rk_otp_id[0], rk_otp_id[1], rk_otp_id[2],
				rk_otp_id[3], rk_otp_id[4], rk_otp_id[5],
				rk_otp_id[6], rk_otp_id[7], rk_otp_id[8],
				rk_otp_id[9], rk_otp_id[10], rk_otp_id[11],
				rk_otp_id[12], rk_otp_id[13], rk_otp_id[14],
				rk_otp_id[15]);
		} else {
			/*
			 * ⚠ 읽기에 실패하면 자리표를 준다.
			 *
			 *   그대로 오류를 돌려주면 USB 직렬번호 디스크립터가
			 *   STALL 되고 호스트 로그가 지저분해진다. 기능에는
			 *   영향이 없지만, bring-up 에서는 USB 를 깨끗하게
			 *   두는 편이 낫다. 실패는 위 LOG_WRN 과 아래 경고로
			 *   드러나므로 조용히 숨기지는 않는다.
			 */
			static const uint8_t fallback[OTP_ID_BYTES] = {
				'R', 'K', '3', '5', '6', '6', 0x00, 0x01,
				0, 0, 0, 0, 0, 0, 0, 0,
			};

			memcpy(rk_otp_id, fallback, sizeof(rk_otp_id));
			LOG_WRN("OTP 읽기 실패 — 자리표 ID 를 쓴다 "
				"(보드마다 같은 값이다)");
		}
	}

	n = MIN(length, sizeof(rk_otp_id));
	memcpy(buffer, rk_otp_id, n);

	return (ssize_t)n;
}

/*
 * 외부 공개 접근자 — 선언은 hwinfo_rk3566.h.
 * `moru info` 가 실제 OTP ID 와 자리표를 구분해 찍는 데 쓴다.
 */
bool rk3566_otp_id_is_valid(void)
{
	return rk_otp_id_valid;
}
