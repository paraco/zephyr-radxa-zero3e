/*
 * `moru` 셸 명령 — 시스템 관리.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 설계 원칙: **내장 명령을 다시 만들지 않는다.**
 * 셸을 켜면 공짜로 붙는 것들(kernel thread list·stacks·unwind·uptime·
 * cycles, kernel heap, device list, hwinfo devid, log status, app version)은
 * 그대로 쓰고, `moru info` 는 그중 자주 보는 값을 **한 화면에 모으는 것**만
 * 한다. 중복 구현은 두 곳이 어긋날 위험만 만든다.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/devicetree.h>
#include <zephyr/app_version.h>
#include <zephyr/version.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/sys/util.h>

#include "moru.h"
#include "hwinfo_rk3566.h"

#ifndef MORU_BUILD_TIME
#define MORU_BUILD_TIME "unknown"
#endif

#define read_midr_el1()	read_sysreg(midr_el1)

static void print_uptime(const struct shell *sh)
{
	uint64_t ms = (uint64_t)k_uptime_get();
	uint32_t s = (uint32_t)(ms / 1000U);

	shell_print(sh, "  가동      %ud %02u:%02u:%02u (%llu ms)",
		    s / 86400U, (s / 3600U) % 24U, (s / 60U) % 60U, s % 60U, ms);
}

static void print_chip_id(const struct shell *sh)
{
	uint8_t id[16];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));
	char hex[sizeof(id) * 2 + 1];

	if (n <= 0) {
		shell_print(sh, "  칩 ID     읽기 실패 (%d)", (int)n);
		return;
	}

	for (ssize_t i = 0; i < n; i++) {
		(void)snprintk(&hex[i * 2], 3, "%02x", id[i]);
	}

	shell_print(sh, "  칩 ID     %s  (%s)", hex,
		    rk3566_otp_id_is_valid() ? "OTP" : "자리표 — OTP 읽기 실패");
}

static int cmd_moru_info(const struct shell *sh, size_t argc, char **argv)
{
	uint64_t midr = read_midr_el1();
	uint32_t impl = (uint32_t)((midr >> 24) & 0xffU);
	uint32_t part = (uint32_t)((midr >> 4) & 0xfffU);
	uint32_t var  = (uint32_t)((midr >> 20) & 0xfU);
	uint32_t rev  = (uint32_t)(midr & 0xfU);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "MoruLab Zephyr RTOS");
	shell_print(sh, "  버전      %s (build %s)", APP_VERSION_STRING,
		    STRINGIFY(APP_BUILD_VERSION));
	shell_print(sh, "  빌드      %s", MORU_BUILD_TIME);
	shell_print(sh, "  Zephyr    %s", KERNEL_VERSION_STRING);
	shell_print(sh, "  보드      %s  (%s)", CONFIG_BOARD_TARGET, MORU_MODEL);
	shell_print(sh, "  SoC       Rockchip RK3566 (%s 정의 재사용)", CONFIG_SOC);
	shell_print(sh, "  CPU       %s r%up%u, MIDR 0x%08llx, EL%u",
		    moru_cpu_part_name(impl, part), var, rev, midr,
		    (unsigned int)GET_EL(read_currentel()));
	shell_print(sh, "  메모리    0x%08lx + %lu KiB (zephyr,sram)",
		    (unsigned long)CONFIG_SRAM_BASE_ADDRESS,
		    (unsigned long)CONFIG_SRAM_SIZE);
	print_chip_id(sh);
	shell_print(sh, "  콘솔      %s (셸 같은 포트)",
		    DT_NODE_FULL_NAME(DT_CHOSEN(zephyr_console)));
	shell_print(sh, "  마커 UART %s %u 8N1 (초기 부팅 전용)",
		    DT_NODE_FULL_NAME(DT_NODELABEL(uart2)),
		    (unsigned int)DT_PROP(DT_NODELABEL(uart2), current_speed));
	if (moru_preboot_us() != 0U) {
		shell_print(sh, "  부트로더  %llu.%03llu s (전원 투입 → Zephyr 진입)",
			    moru_preboot_us() / 1000000ULL,
			    (moru_preboot_us() % 1000000ULL) / 1000ULL);
	}
	print_uptime(sh);

	shell_print(sh, "");
	shell_print(sh, "내장 명령도 쓴다 — kernel thread list|stacks|unwind,");
	shell_print(sh, "kernel heap|uptime|cycles, device list, hwinfo devid,");
	shell_print(sh, "log status, app version");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(moru_subcmds,
	SHELL_CMD_ARG(info, NULL, "시스템 정보 한 화면", cmd_moru_info, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(moru, &moru_subcmds, "MoruLab 시스템 관리", NULL);
