/*
 * RK3566 hwinfo — OTP 칩 ID 의 유효성을 외부에 알린다.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 왜 필요한가: hwinfo_rk3566.c 는 OTP 읽기가 실패하면 자리표 ID
 * ("RK3566\0\x01" + 0) 를 캐시에 넣고 **성공을 반환한다** — USB 직렬번호
 * 디스크립터가 비는 것보다 낫기 때문이다. 그래서 hwinfo_get_device_id() 만
 * 보면 실제 ID 와 자리표를 구분할 수 없다. `moru info` 가 그것을 구분해
 * 찍으려면 이 함수가 필요하다.
 */

#ifndef MORU_HWINFO_RK3566_H_
#define MORU_HWINFO_RK3566_H_

#include <stdbool.h>

#ifdef CONFIG_HWINFO_RK3566_OTP

/**
 * OTP 에서 실제로 읽은 ID 인가. false 면 자리표다.
 *
 * ⚠ **첫 hwinfo_get_device_id() 호출 뒤에만 의미가 있다.** 드라이버는 OTP
 *   를 지연 읽기하므로, 그 전에 부르면 아직 읽지 않았다는 뜻의 false 가
 *   돌아온다 — "자리표" 와 구분되지 않는다. 반드시 get 을 먼저 부른다.
 */
bool rk3566_otp_id_is_valid(void);

#else

static inline bool rk3566_otp_id_is_valid(void)
{
	return false;
}

#endif /* CONFIG_HWINFO_RK3566_OTP */

#endif /* MORU_HWINFO_RK3566_H_ */
