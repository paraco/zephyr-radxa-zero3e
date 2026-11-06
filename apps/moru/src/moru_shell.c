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
#include <zephyr/device.h>
#include <zephyr/app_version.h>
#include <zephyr/version.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/drivers/pm_cpu_ops/psci.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>

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
#if DT_NODE_EXISTS(DT_NODELABEL(cdc_acm_uart1))
	shell_print(sh, "  OTA 포트  %s (%s)",
		    DT_NODE_FULL_NAME(DT_NODELABEL(cdc_acm_uart1)),
		    device_is_ready(DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1)))
			    ? "준비됨" : "준비 안 됨");
#endif
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
	shell_print(sh, "재부팅은 moru reboot (먼저 moru reboot probe)");
	shell_print(sh, "OTA 실험은 moru ota mark → moru reboot → moru ota check");

	return 0;
}

/*
 * ── 재부팅 (PSCI) ────────────────────────────────────────────────────
 *
 * CONFIG_PM_CPU_OPS 를 켜지 않고 arm_smccc_smc() 를 여기서 직접 부른다.
 * 그 설정을 켜면 psci_init 이 PRE_KERNEL_1 에서 PSCI_VERSION SMC 를 쏘는데
 * 그건 **콘솔보다 먼저**다 — BL31 이 응답하지 않으면 한 글자도 못 보고
 * 무음으로 죽어, 과거 FRAME_POINTER·INIT_STACKS 때와 증상이 똑같아져
 * 원인 구분이 불가능해진다. 여기서 부르면 위험한 SMC 는 사람이 명령을
 * 칠 때만 나가고, 그 시점엔 콘솔이 살아 있고 안내도 이미 나간 뒤다.
 *
 * arch/Kconfig:50 이 ARM64 에 HAS_ARM_SMCCC 를 무조건 select 하므로
 * 추가 Kconfig 가 0개다.
 *
 * 함수 ID 는 PSCI 사양(ARM DEN 0022)의 공개 값이다. Zephyr 의
 * drivers/pm_cpu_ops/pm_cpu_ops_psci.h:20 이 PSCI_0_2_FN_BASE 0x84000000 을
 * 쓰지만 그것은 드라이버 내부 헤더라 앱에서 include 할 수 없어 여기 적는다.
 * 버전 해독 매크로는 공개 헤더 zephyr/drivers/pm_cpu_ops/psci.h 를 쓴다.
 */
#define MORU_PSCI_FN_BASE	0x84000000UL
#define MORU_PSCI_VERSION	(MORU_PSCI_FN_BASE + 0UL)
#define MORU_PSCI_SYSTEM_RESET	(MORU_PSCI_FN_BASE + 9UL)

/* PSCI_VERSION 은 성공하면 버전을, 미지원이면 NOT_SUPPORTED(-1)를 준다 */
#define MORU_PSCI_RET_NOT_SUPPORTED	((unsigned long)-1)

static int cmd_moru_reboot_probe(const struct shell *sh, size_t argc, char **argv)
{
	struct arm_smccc_res res = { 0 };

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "probe: issuing PSCI_VERSION (SMC 0x%08lx) ...",
		    MORU_PSCI_VERSION);
	shell_print(sh, "       여기서 멈추면 BL31 이 NS-EL1 의 SMC 에 응답하지");
	shell_print(sh, "       않는다는 뜻이다 — 그 자체가 결론이다.");

	arm_smccc_smc(MORU_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);

	if (res.a0 == MORU_PSCI_RET_NOT_SUPPORTED) {
		shell_error(sh, "probe: PSCI 미지원 (NOT_SUPPORTED)");
		return -ENOTSUP;
	}

	shell_print(sh, "probe: PSCI %u.%u (raw 0x%08lx) — 응답했다",
		    (unsigned int)PSCI_VERSION_MAJOR((uint32_t)res.a0),
		    (unsigned int)PSCI_VERSION_MINOR((uint32_t)res.a0),
		    res.a0);
	shell_print(sh, "       이제 'moru reboot' 을 쓸 수 있다.");
	return 0;
}

static int cmd_moru_reboot(const struct shell *sh, size_t argc, char **argv)
{
	struct arm_smccc_res res = { 0 };

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "재부팅한다 (PSCI_SYSTEM_RESET).");
	shell_print(sh, "⚠ USB 가 끊기므로 이 셸도 끊긴다. 재부팅 확인은");
	shell_print(sh, "  UART2 의 마커 [2][<S][S>][<U][U>][M] 로 한다.");

	/*
	 * 순서가 중요하다 — CDC 가 끊긴 뒤 비우는 것은 의미가 없다.
	 *   ① 위 안내를 셸 큐에 넣었다
	 *   ② 로그를 동기 모드로 전환해 밀어낸다
	 *   ③ CDC 가 실제로 내보낼 시간을 준다 — usbd_cdc_acm.c 의
	 *      tx_fifo_work 가 K_MSEC(1) 로 예약되고 USB 프레임 몇 개가
	 *      더 나가야 한다. 50 ms 면 넉넉하다
	 *   ④ SMC
	 */
	log_panic();
	k_msleep(50);

	arm_smccc_smc(MORU_PSCI_SYSTEM_RESET, 0, 0, 0, 0, 0, 0, 0, &res);

	/* 여기까지 오면 리셋이 안 된 것이다 */
	shell_error(sh, "재부팅 실패 — SMC 가 돌아왔다 (a0=0x%08lx).", res.a0);
	shell_error(sh, "'moru reboot probe' 로 BL31 응답을 먼저 확인한다.");
	shell_error(sh, "PSCI 가 안 되면 대안은 CRU 글로벌 소프트 리셋이고");
	shell_error(sh, "그 레지스터는 RK3568 TRM Part1 CRU 장에 있다");
	shell_error(sh, "(docs/trm-grounding.md §5 의 1번 항목).");
	return -EIO;
}

/*
 * ── OTA 1단계: 재부팅 뒤 DRAM 이 보존되는가 (2026-11-06) ──────────────
 *
 * 왜 이 실험이 필요한가: U-Boot 에 ums·fastboot·dfu 가 없고 bootdelay=0 이라
 * USB 로 SD 카드를 갱신할 길이 없다. 그러나 fatwrite 가 있으므로
 * "Zephyr 가 RAM 에 받고 U-Boot 가 SD 에 쓴다" 가 가능하다
 * (docs/ota-strategy.md §2). 그 설계 전체가 **재부팅 뒤 DRAM 보존**이라는
 * 미지수 하나에 걸려 있다. PSCI SYSTEM_RESET 에서 BL31·DDR 초기화가
 * 메모리를 지우면 무의미하다. 보장이 없어 실험으로만 알 수 있다.
 *
 *   moru ota mark   -> moru reboot -> moru ota check
 *
 * 주소 선택: 0x50000000. DRAM(0x00200000~0x80000000, 실기 검증) 안이고,
 * Zephyr 영역(0x40000000 + 1 MiB)과 겹치지 않으며, U-Boot 가 재배치되는
 * 상단(0x80000000 근처)과도 멀다.
 *
 * ⚠ device_map() 은 PRE_KERNEL 에서 부르면 보드가 죽는다
 *   (z_mem_manage_init 이 그보다 뒤, kernel/init.c:293). 여기는 셸 스레드
 *   문맥이므로 적법하다 — moru_stage.c 가 바로 그것을 어겨 삭제됐다.
 */
#define MORU_OTA_PHYS		0x50000000UL
#define MORU_OTA_MAP_SIZE	0x1000U		/* 한 페이지면 충분하다 */
#define MORU_OTA_MAGIC		0x4d4f5255UL	/* "MORU" (LE 로 'U''R''O''M') */
#define MORU_OTA_PATTERN	0xa5c3f00dUL

/* 오프셋: [0]=매직 [4]=패턴 [8]=단조 카운터 */
#define MORU_OTA_OFF_MAGIC	0x00
#define MORU_OTA_OFF_PATTERN	0x04
#define MORU_OTA_OFF_COUNT	0x08

static mm_reg_t moru_ota_map(const struct shell *sh)
{
	static mm_reg_t base;

	if (base != 0) {
		return base;
	}

	device_map(&base, MORU_OTA_PHYS, MORU_OTA_MAP_SIZE, K_MEM_CACHE_NONE);
	if (base == 0) {
		shell_error(sh, "device_map 실패 — MAX_XLAT_TABLES(%d) 부족일 수 있다",
			    CONFIG_MAX_XLAT_TABLES);
	}
	return base;
}

static int cmd_moru_ota_mark(const struct shell *sh, size_t argc, char **argv)
{
	mm_reg_t base = moru_ota_map(sh);
	uint32_t count;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (base == 0) {
		return -ENOMEM;
	}

	/* 이전에 표시한 적이 있으면 카운터를 늘린다 (몇 번째 재부팅인지) */
	count = (sys_read32(base + MORU_OTA_OFF_MAGIC) == MORU_OTA_MAGIC)
			? sys_read32(base + MORU_OTA_OFF_COUNT) + 1U
			: 1U;

	sys_write32(MORU_OTA_MAGIC, base + MORU_OTA_OFF_MAGIC);
	sys_write32(MORU_OTA_PATTERN, base + MORU_OTA_OFF_PATTERN);
	sys_write32(count, base + MORU_OTA_OFF_COUNT);

	shell_print(sh, "0x%08lx 에 표시했다 — magic 0x%08lx pattern 0x%08lx count %u",
		    MORU_OTA_PHYS, MORU_OTA_MAGIC, MORU_OTA_PATTERN, count);
	shell_print(sh, "이제 'moru reboot' 한 뒤 'moru ota check' 를 친다.");
	shell_print(sh, "⚠ 캐시를 끈 매핑(K_MEM_CACHE_NONE)이라 쓰기가 바로 DRAM 에");
	shell_print(sh, "  들어간다 — 재부팅 전 flush 를 기다릴 필요가 없다.");
	return 0;
}

static int cmd_moru_ota_check(const struct shell *sh, size_t argc, char **argv)
{
	mm_reg_t base = moru_ota_map(sh);
	uint32_t magic, pattern, count;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (base == 0) {
		return -ENOMEM;
	}

	magic = sys_read32(base + MORU_OTA_OFF_MAGIC);
	pattern = sys_read32(base + MORU_OTA_OFF_PATTERN);
	count = sys_read32(base + MORU_OTA_OFF_COUNT);

	shell_print(sh, "0x%08lx: magic 0x%08x pattern 0x%08x count %u",
		    MORU_OTA_PHYS, magic, pattern, count);

	if (magic == MORU_OTA_MAGIC && pattern == MORU_OTA_PATTERN) {
		shell_print(sh, "✅ 보존됐다 — 재부팅을 넘어 DRAM 이 남는다.");
		shell_print(sh, "   RAM→SD OTA 경로가 열린다 (docs/ota-strategy.md §2).");
		return 0;
	}

	if (magic == 0U && pattern == 0U) {
		shell_warn(sh, "❌ 0 으로 지워졌다 — DDR 초기화가 메모리를 비운다.");
	} else {
		shell_warn(sh, "❌ 값이 다르다 — 보존되지 않거나 다른 용도로 쓰인 영역이다.");
	}
	shell_warn(sh, "   A/B 슬롯 chain-load 로 간다 (docs/ota-strategy.md §3).");
	shell_warn(sh, "   ⚠ 아직 'moru ota mark' 를 한 적이 없으면 이 결과는 무의미하다.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(moru_ota_subcmds,
	SHELL_CMD_ARG(mark, NULL, "0x50000000 에 매직을 써 둔다 (재부팅 전)",
		      cmd_moru_ota_mark, 1, 0),
	SHELL_CMD_ARG(check, NULL, "그 매직이 재부팅을 넘어 남았는지 본다",
		      cmd_moru_ota_check, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(moru_reboot_subcmds,
	SHELL_CMD_ARG(probe, NULL, "PSCI_VERSION 만 쏘아 BL31 응답을 확인한다",
		      cmd_moru_reboot_probe, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(moru_subcmds,
	SHELL_CMD_ARG(info, NULL, "시스템 정보 한 화면", cmd_moru_info, 1, 0),
	SHELL_CMD_ARG(reboot, &moru_reboot_subcmds,
		      "재부팅 (PSCI_SYSTEM_RESET). 'probe' 로 먼저 확인한다",
		      cmd_moru_reboot, 1, 1),
	SHELL_CMD(ota, &moru_ota_subcmds,
		  "OTA 실험 — 재부팅 뒤 DRAM 보존 여부를 가린다", NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(moru, &moru_subcmds, "MoruLab 시스템 관리", NULL);
