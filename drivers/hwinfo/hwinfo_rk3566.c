/*
 * Radxa ZERO 3E (RK3566) — hwinfo 장치 ID.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ⚠ 이것은 '자리표' 다. 보드마다 같은 값이 나온다.
 *
 * 왜 자리표인가:
 *   Zephyr 의 USB 스택은 직렬번호 문자열 디스크립터를
 *   hwinfo_get_device_id() 로 만든다 (usbd_ch9.c 의 get_sn_from_hwid).
 *   그 구현이 없으면 호스트의 GET_DESCRIPTOR(String, index 3) 요청이
 *   "HWINFO not implemented or enabled" + protocol error 로 끝난다.
 *   열거와 동작에는 영향이 없지만 로그가 지저분하다.
 *
 *   RK3566 의 실제 칩 고유 ID 는 OTP(OTP_NS @0xFE38C000)에 있다. 읽으려면
 *   OTP 컨트롤러 시퀀스와 clk_otpc_ns / pclk_otpc_ns 게이팅이 필요하고,
 *   RK3568 TRM Part1 V1.3 에는 그 레지스터 설명이 없다 (CRU 쪽 게이팅
 *   비트만 보인다 — 6822~6829행). Part2 나 미공개 문서가 필요하다.
 *
 *   그래서 지금은 고정값을 준다. 한 대를 붙여 쓰는 bring-up 에서는
 *   문제가 없고, 여러 대를 동시에 붙이거나 제품으로 쓰려면 OTP 읽기를
 *   구현해 이 파일을 대체해야 한다.
 *
 * ⚠ Zephyr 의 z_impl_hwinfo_get_device_id() 는 __weak 이다
 *   (drivers/hwinfo/hwinfo_weak_impl.c). 여기서 강한 심볼로 정의하면
 *   링크 때 이쪽이 쓰인다.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

/*
 * 8바이트. usbd 는 이것을 16자 ASCII-7 hex 로 바꿔 직렬번호로 쓴다
 * (CONFIG_USBD_HWINFO_DEVID_LENGTH 참고).
 *
 * 값에 의미를 담아 두면 로그에서 자리표임을 알아보기 쉽다:
 *   "RK3566" + 0x00 0x01  ->  "524B33353636" + "0001"
 */
static const uint8_t rk3566_placeholder_id[8] = {
	'R', 'K', '3', '5', '6', '6', 0x00, 0x01,
};

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	size_t n = MIN(length, sizeof(rk3566_placeholder_id));

	if (buffer == NULL || length == 0U) {
		return -EINVAL;
	}

	memcpy(buffer, rk3566_placeholder_id, n);

	return (ssize_t)n;
}
