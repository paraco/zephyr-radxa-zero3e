/*
 * 부팅 슬롯 — sdcard/boot.cmd 가 `go` 직전에 0x51000018 ('SLOT') · 0x5100001c (1 try ·
 * 2 main · 3 예비) 에 쓴 표식을 부팅 때 한 번 읽는다 (Studio F6).
 * 표식이 없으면(옛 boot.scr · QEMU) "-".
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#ifdef CONFIG_BOARD_RADXA_ZERO_3E
/** "try" · "main" · "main-alt" · "-" */
const char *rk3566_boot_slot(void);
#else
static inline const char *rk3566_boot_slot(void) { return "-"; }
#endif
#ifdef __cplusplus
}
#endif
