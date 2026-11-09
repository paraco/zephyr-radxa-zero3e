/* ml approve — src/approve.c 머리 주석 참고 (mlledger/1 v0.2 §4). SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct hubml_approve_result {
	bool ok;
	const char *code;	/* 실패: bad_arg · bad_json · unknown_key · sig · stale · expired · not_found · denied · ledger */
	char kind[16];
	uint32_t seq;
	char head[65];
	int deploy;		/* 1 = 배포 없음, 0 = 시작함, 음수 = 시작 못 함 (Edge 없음 등) */
};
int hubml_approve(const char *b64, const char *sig64, const char *key, struct hubml_approve_result *res);
