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
#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/crc.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/uart.h>

#include "moru.h"
#include "hwinfo_rk3566.h"
#include "udc_dwc3_regs.h"
#include "udc_dwc3_diag.h"

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
	shell_print(sh, "OTA 는 moru ota begin|status|verify|apply|abort");

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
#define MORU_OTA_MAX_SIZE	(256U * 1024U)	/* boot.scr 이 쓰는 고정 크기 */
#define MORU_OTA_MAP_SIZE	(MORU_OTA_HDR_SIZE + MORU_OTA_MAX_SIZE)

/*
 * ⚠ 매직을 두 개로 나눈다.
 *   "MORU" = 적용할 실제 OTA 이미지. boot.scr 이 이것을 보면 fatwrite 로
 *            SD 의 zephyr.bin 을 덮는다
 *   "MRK0" = DRAM 보존 확인용 표시(moru ota mark). boot.scr 이 무시한다
 * 하나로 쓰면 mark 한 뒤 재부팅할 때 boot.scr 이 쓰레기를 zephyr.bin 으로
 * 써 버린다 — setexpr 가 없어 크기를 변수로 읽을 수 없고 고정 256 KiB 를
 * 쓰기 때문이다.
 */
#define MORU_OTA_MAGIC		0x4d4f5255UL	/* "MORU" */
#define MORU_MARK_MAGIC		0x4d524b30UL	/* "MRK0" */
#define MORU_OTA_PATTERN	0xa5c3f00dUL

/*
 * 헤더 (boot.scr 이 itest.l 로 읽는다)
 *   [0]  매직
 *   [4]  페이로드 크기 (바이트)
 *   [8]  페이로드 CRC32 (crc32_ieee)
 *   [12] 패턴 — mark 용, OTA 에서는 0
 *   [16] 페이로드 시작
 */
#define MORU_OTA_OFF_MAGIC	0x00
#define MORU_OTA_OFF_SIZE	0x04
#define MORU_OTA_OFF_CRC	0x08
#define MORU_OTA_OFF_PATTERN	0x0c
#define MORU_OTA_HDR_SIZE	0x10U

static mm_reg_t moru_ota_map(const struct shell *sh)
{
	static mm_reg_t base;

	if (base != 0) {
		return base;
	}

	/*
	 * ⚠ K_MEM_CACHE_WB (캐시 켬) — 2026-11-06 수정.
	 *
	 * 처음에는 K_MEM_CACHE_NONE 이었다. 쓰기는 빨랐지만(175 KB 전송이
	 * 1.2초) **읽기가 멈췄다** — moru ota verify 의 crc32_ieee 가 175 KB 를
	 * 바이트 단위로 읽는데, Device 매핑은 접근마다 직렬화되어 극단적으로
	 * 느려진다. 셸이 멈춘 것처럼 보였다.
	 *
	 * 대가: 쓴 내용이 캐시에 남으므로 **재부팅 전에 밀어내야** DRAM 에
	 * 남는다. moru_ota_flush() 가 그 일을 하고, mark 와 apply 가 부른다.
	 * sys_cache_data_flush_range() 는 CONFIG_ARM64_DCACHE_ALL_OPS=y 덕에
	 * 쓸 수 있다 (그 설정은 EL2 부팅 문제를 고치면서 켠 것이다 — 그때의
	 * 수확이 여기서 또 쓰인다).
	 */
	device_map(&base, MORU_OTA_PHYS, MORU_OTA_MAP_SIZE, K_MEM_CACHE_WB);
	if (base == 0) {
		shell_error(sh, "device_map 실패 — MAX_XLAT_TABLES(%d) 부족일 수 있다",
			    CONFIG_MAX_XLAT_TABLES);
	}
	return base;
}

/*
 * 캐시를 DRAM 으로 밀어낸다. WB 매핑이므로 재부팅을 넘기려면 필수다.
 * 재부팅 뒤 U-Boot 와 다음 Zephyr 가 읽는 것은 DRAM 의 내용이다.
 */
static void moru_ota_flush(mm_reg_t base, size_t len)
{
	sys_cache_data_flush_range((void *)base, len);
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
	count = (sys_read32(base + MORU_OTA_OFF_MAGIC) == MORU_MARK_MAGIC)
			? sys_read32(base + MORU_OTA_OFF_SIZE) + 1U
			: 1U;

	sys_write32(MORU_MARK_MAGIC, base + MORU_OTA_OFF_MAGIC);
	sys_write32(count, base + MORU_OTA_OFF_SIZE);
	sys_write32(0U, base + MORU_OTA_OFF_CRC);
	sys_write32(MORU_OTA_PATTERN, base + MORU_OTA_OFF_PATTERN);

	moru_ota_flush(base, MORU_OTA_HDR_SIZE);

	shell_print(sh, "0x%08lx 에 표시했다 — magic MRK0 pattern 0x%08lx count %u",
		    MORU_OTA_PHYS, MORU_OTA_PATTERN, count);
	shell_print(sh, "이제 'moru reboot' 한 뒤 'moru ota check' 를 친다.");
	shell_print(sh, "캐시를 DRAM 으로 밀어냈다 (WB 매핑이라 flush 가 필요하다).");
	shell_print(sh, "⚠ MRK0 은 boot.scr 이 무시한다. 실제 OTA 는 MORU 매직이다.");
	return 0;
}

/*
 * ── OTA 2단계: 수신 ──────────────────────────────────────────────────
 *
 * 데이터는 두 번째 CDC-ACM(cdc_acm_uart1, 호스트의 두 번째 포트)으로 온다.
 * 셸은 첫 번째 포트에 있으므로 전송 중에도 상태를 물어볼 수 있다.
 *
 * ── 왜 콜백 + 블록 읽기인가 (2026-11-06 수정) ─────────────────────────
 *
 * 처음에는 전용 스레드가 uart_poll_in() 을 1바이트씩 돌렸다. 실기에서
 * 120초를 넘겨도 끝나지 않았다. 원인 둘을 코드에서 확인했다:
 *
 *   ① usbd_cdc_acm.c:315 의 LOG_HEXDUMP_INF(buf->data, buf->len, "") 가
 *      **수신한 모든 바이트를 hexdump** 한다. LOG_LEVEL 이 INF 였으므로
 *      171 KB 가 전부 로그로 쏟아지고, 그것을 다시 CDC 로 내보내려 하면서
 *      전송이 사실상 멈췄다. → prj.conf 에서 WRN 으로 내렸다 (진짜 병목)
 *   ② cdc_acm_poll_in(:990-992)이 **1바이트 읽을 때마다**
 *      cdc_acm_work_submit(&rx_fifo_work) 를 한다. 171360 바이트면 워크큐
 *      왕복이 17만 번이다
 *
 * 블록 읽기(uart_fifo_read)로 ②를 64배 줄인다. ⚠ 그런데 그 함수는
 * **CDC 워크큐 문맥에서만** 부를 수 있다 — cdc_acm_fifo_read(:807)가
 * check_wq_ctx() 를 확인하고 아니면 __ASSERT_NO_MSG(false) 로 죽는다.
 * 콜백은 :317-318 에서 irq_cb_work 워크 항목으로 제출되므로 그 문맥이
 * 맞다. 따라서 **전용 스레드가 아니라 콜백**이어야 하고, 그러면 스레드
 * 자체가 필요 없어진다.
 *
 * 코드로 미리 확인해 왕복 하나를 아꼈다 — 스레드에서 fifo_read 를 불렀다면
 * 보드가 assert 로 죽었을 것이다 (docs/bringup-rules.md 규칙 5).
 */
static struct {
	bool		active;
	uint32_t	expect;
	uint32_t	got;
	uint32_t	crc;		/* 호스트가 알려준 기대 CRC32 */
	uint32_t	overrun;	/* 기대보다 더 온 바이트 (버린다) */
	mm_reg_t	base;
	/* ── 계측 (2026-11-06) — 추측을 멈추고 수치로 본다 ───────────────
	 * 수신이 0 바이트인데 원인이 콜백 미호출인지 쓰기 쪽인지 가리려면
	 * 런타임 수치가 필요하다. 소프트웨어 후보를 네 개 기각하고도
	 * 좁혀지지 않아 계측을 넣는다 (docs/bringup-rules.md 규칙 4).
	 */
	uint32_t	cb_calls;	/* 콜백이 몇 번 불렸나 */
	uint32_t	cb_bytes;	/* fifo_read 가 돌려준 바이트 합 */
	uint32_t	cb_empty;	/* rx_ready 가 0 이어서 그냥 끝난 횟수 */
	bool		use_poll;	/* true 면 poll_in 스레드 경로 */
	bool		verified;	/* verify 가 CRC 일치를 확인했다 */
} moru_ota;

static void moru_ota_rx_cb(const struct device *dev, void *user_data)
{
	uint8_t buf[64];
	int n, i;

	ARG_UNUSED(user_data);

	moru_ota.cb_calls++;

	/* uart_irq_update() 는 void 다 — 조건으로 쓸 수 없고 먼저 부른다 */
	uart_irq_update(dev);

	if (!uart_irq_rx_ready(dev)) {
		moru_ota.cb_empty++;
		return;
	}

	while (uart_irq_rx_ready(dev)) {
		n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		moru_ota.cb_bytes += (uint32_t)n;

		if (!moru_ota.active) {
			continue;	/* 수신 중이 아니면 버린다 */
		}

		for (i = 0; i < n; i++) {
			if (moru_ota.got < moru_ota.expect) {
				sys_write8(buf[i], moru_ota.base +
						   MORU_OTA_HDR_SIZE +
						   moru_ota.got);
				moru_ota.got++;
			} else {
				moru_ota.overrun++;
			}
		}

		if (moru_ota.got >= moru_ota.expect) {
			moru_ota.active = false;
		}
	}
}

/*
 * poll_in 경로 — `moru ota begin <크기> <crc> poll` 로 고른다.
 *
 * 콜백 경로가 0 바이트였는데, 그 수정에서 로그(hexdump)와 경로(스레드→콜백)
 * 를 **동시에** 바꿔 어느 쪽 효과인지 가릴 수 없게 됐다(규칙 2 위반).
 * 두 경로를 한 빌드에 두면 **같은 왕복에서** 비교할 수 있다.
 *
 * poll_in 은 문맥 제약이 없다(cdc_acm_poll_in:980 에 check_wq_ctx 가 없다).
 * 대신 1바이트마다 rx_fifo_work 를 제출한다.
 */
static void moru_ota_poll_thread(void *a, void *b, void *c)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));
	unsigned char ch;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		if (!moru_ota.active || !moru_ota.use_poll) {
			k_msleep(100);
			continue;
		}

		if (uart_poll_in(dev, &ch) == 0) {
			moru_ota.cb_calls++;
			moru_ota.cb_bytes++;
			if (moru_ota.got < moru_ota.expect) {
				sys_write8(ch, moru_ota.base +
						   MORU_OTA_HDR_SIZE +
						   moru_ota.got);
				moru_ota.got++;
			} else {
				moru_ota.overrun++;
			}
			if (moru_ota.got >= moru_ota.expect) {
				moru_ota.active = false;
			}
		} else {
			moru_ota.cb_empty++;
			k_yield();
		}
	}
}

K_THREAD_DEFINE(moru_ota_poll_tid, 4096, moru_ota_poll_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(12), 0, 0);

/*
 * dwc3 레지스터를 **앱에서 직접 읽는다** (드라이버 편집 0).
 *
 * 로그가 터미널 출력 중에 잘려 EP 설정 상태를 알 수 없었다 — 매번
 * `ep5 DEPCFG ... status` 에서 끊겼다. DALEPENA 하나면 EP 가 몇 개 켜졌는지
 * 즉시 판정된다:
 *   CDC 하나 → 비트 0,1,2,3,5        = 0x0000002f
 *   CDC 둘   → 비트 0,1,2,3,4,5,7,9  = 0x000002bf
 * 계획(roadmap B3)은 드라이버에 진단 API 를 넣는 것이었지만 지금 필요한
 * 것은 읽기뿐이므로 앱에서 매핑해 읽는다. 드라이버를 안 건드리므로
 * 위험이 낮고(USB 셸을 잃지 않는다) 왕복도 아낀다.
 */
/*
 * dwc3 진단 — 드라이버의 스냅샷 API 를 쓴다 (udc_dwc3_diag.h).
 *
 * 처음에는 앱에서 device_map 으로 레지스터를 직접 읽었다. 그것으로
 * DALEPENA 를 확인해 EP 6개가 켜진 것을 알아냈고(로그가 잘려 보이지 않던
 * 것), 그 덕에 "두 번째 CDC EP 미설정" 가설을 기각했다. 이제 드라이버가
 * 스냅샷 API 를 제공하므로 그것을 쓴다 — 읽는 사이 상태가 바뀌어 **찢긴
 * 값**을 보는 것을 줄이고, 카운터도 함께 받는다.
 */
static const char *ep0_state_name(uint8_t st)
{
	switch (st) {
	case 0:  return "SETUP";
	case 1:  return "DATA_IN";
	case 2:  return "DATA_OUT";
	case 3:  return "STATUS_IN";
	case 4:  return "STATUS_OUT";
	default: return "?";
	}
}

static int cmd_moru_usb(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *udc = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_dwc3_diag d;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = udc_dwc3_diag_get(udc, &d);
	if (err != 0) {
		shell_error(sh, "진단을 읽지 못했다 (%d)", err);
		return err;
	}

	shell_print(sh, "dwc3  core 0x%04x release 0x%04x · 물리 EP %u",
		    (unsigned int)FIELD_GET(DWC3_GSNPSID_CORE_ID, d.gsnpsid),
		    (unsigned int)FIELD_GET(DWC3_GSNPSID_RELEASE, d.gsnpsid),
		    (unsigned int)d.hw_num_eps);
	shell_print(sh, "  GCTL      0x%08x  DCFG 0x%08x  DCTL 0x%08x",
		    d.gctl, d.dcfg, d.dctl);
	shell_print(sh, "  DSTS      0x%08x  speed=%u linkstate=%u",
		    d.dsts,
		    (unsigned int)FIELD_GET(DWC3_DSTS_CONNECTSPD, d.dsts),
		    (unsigned int)FIELD_GET(DWC3_DSTS_USBLNKST, d.dsts));
	shell_print(sh, "  GUSB2PHY  0x%08x", d.gusb2phycfg0);
	shell_print(sh, "  DALEPENA  0x%08x  ← 활성 EP 비트맵", d.dalepena);
	if (d.dalepena == 0x2bfU) {
		shell_print(sh, "     CDC 2개 (셸 + OTA) 정상");
	} else if (d.dalepena == 0x2fU) {
		shell_warn(sh, "     CDC 1개뿐이다");
	}
	shell_print(sh, "  EP0       state=%s wLength=%u",
		    ep0_state_name(d.ep0_state), (unsigned int)d.ep0_wlength);
	shell_print(sh, "  bulk mps  CDC0 in=%u out=%u · CDC1 in=%u out=%u",
		    (unsigned int)d.bulk_in_mps, (unsigned int)d.bulk_out_mps,
		    (unsigned int)d.bulk_in1_mps, (unsigned int)d.bulk_out1_mps);
	if (d.bulk_in_mps == 512U) {
		shell_print(sh, "     ✅ 512 — HS 디스크립터가 쓰인다");
	} else if (d.bulk_in_mps == 64U) {
		shell_warn(sh, "     ❌ 64 — FS 디스크립터다. 속도 확정 시점 문제");
	}

	shell_print(sh, "");
	shell_print(sh, "카운터 (bring-up 에서 쫓던 증상은 ⚠ 표시)");
	shell_print(sh, "  reset %u · connect %u · disconnect %u · setup %u",
		    d.stats.reset, d.stats.connect_done, d.stats.disconnect,
		    d.stats.setup);
	shell_print(sh, "  xfer_complete %u · xfer_not_ready %u",
		    d.stats.xfer_complete, d.stats.xfer_not_ready);
	shell_print(sh, "  ⚠ cmd_timeout %u · trb_hw_owned %u · no_queued_buf %u",
		    d.stats.cmd_timeout, d.stats.trb_hw_owned,
		    d.stats.no_queued_buf);
	if (d.stats.cmd_timeout || d.stats.trb_hw_owned ||
	    d.stats.no_queued_buf) {
		shell_warn(sh, "  → 0 이 아니다. 과거 열거 실패를 일으킨 증상들이다");
	}
	return 0;
}

static int cmd_moru_ota_begin(const struct shell *sh, size_t argc, char **argv)
{
	mm_reg_t base = moru_ota_map(sh);
	unsigned long size, crc;
	char *end;

	if (base == 0) {
		return -ENOMEM;
	}

	size = strtoul(argv[1], &end, 0);
	if (*end != '\0' || size == 0UL || size > MORU_OTA_MAX_SIZE) {
		shell_error(sh, "크기가 잘못됐다 (1 ~ %u)", MORU_OTA_MAX_SIZE);
		return -EINVAL;
	}

	crc = strtoul(argv[2], &end, 16);
	if (*end != '\0') {
		shell_error(sh, "CRC32 는 16진수로 준다 (예: 1a2b3c4d)");
		return -EINVAL;
	}

	ARG_UNUSED(argc);

	moru_ota.base = base;
	moru_ota.expect = (uint32_t)size;
	moru_ota.crc = (uint32_t)crc;
	moru_ota.got = 0;

	/* 받는 중에 재부팅되어도 적용되지 않게 매직을 먼저 지운다 */
	sys_write32(0U, base + MORU_OTA_OFF_MAGIC);

	moru_ota.overrun = 0;
	moru_ota.cb_calls = 0;
	moru_ota.cb_bytes = 0;
	moru_ota.cb_empty = 0;
	moru_ota.verified = false;
	moru_ota.use_poll = (argc >= 4) && (strcmp(argv[3], "poll") == 0);

	{
		const struct device *dev =
			DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1));

		if (!device_is_ready(dev)) {
			shell_error(sh, "OTA 포트가 준비되지 않았다");
			return -ENODEV;
		}

		if (moru_ota.use_poll) {
			/* 콜백을 떼야 poll_in 이 받는다 — usbd_cdc_acm_enable
			 * (:367)이 cb == NULL 일 때만 poll_in 용 rx_fifo_work 를
			 * 제출하고, irq_rx_enable 이 켜져 있으면 경로가 섞인다 */
			uart_irq_rx_disable(dev);
			uart_irq_callback_user_data_set(dev, NULL, NULL);
			moru_ota.active = true;
		} else {
			uart_irq_rx_disable(dev);
			uart_irq_callback_user_data_set(dev, moru_ota_rx_cb, NULL);
			moru_ota.active = true;
			uart_irq_rx_enable(dev);
		}
	}

	shell_print(sh, "수신 시작 — %lu 바이트, 기대 CRC32 0x%08lx", size, crc);
	shell_print(sh, "호스트에서 두 번째 CDC 포트로 부어 넣는다:");
	shell_print(sh, "  cat zephyr.bin > /dev/cu.usbmodemXXXXXX3");
	shell_print(sh, "진행은 'moru ota status', 끝나면 'moru ota verify'.");
	return 0;
}

static int cmd_moru_ota_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (moru_ota.expect == 0U) {
		shell_print(sh, "수신한 적이 없다. 'moru ota begin <크기> <crc32>'.");
		return 0;
	}

	shell_print(sh, "%s — %u / %u 바이트 (%u %%)",
		    moru_ota.active ? "받는 중" : "멈춤/완료",
		    moru_ota.got, moru_ota.expect,
		    (unsigned int)((uint64_t)moru_ota.got * 100U / moru_ota.expect));
	shell_print(sh, "경로: %s", moru_ota.use_poll ? "poll_in 스레드" : "콜백");
	shell_print(sh, "계측: 호출 %u회 · 읽은 바이트 %u · 빈 호출 %u",
		    moru_ota.cb_calls, moru_ota.cb_bytes, moru_ota.cb_empty);
	if (moru_ota.cb_calls == 0U) {
		shell_warn(sh, "  → 한 번도 불리지 않았다. 수신 경로가 막혀 있다");
		shell_warn(sh, "     'moru usb' 로 DALEPENA 를 먼저 본다");
	} else if (moru_ota.cb_bytes == 0U) {
		shell_warn(sh, "  → 불렸지만 읽은 바이트가 0 이다 (링버퍼가 빈다)");
	}
	if (moru_ota.overrun != 0U) {
		shell_warn(sh, "기대보다 %u 바이트 더 왔다 (버렸다) — 크기가 틀렸나?",
			   moru_ota.overrun);
	}
	return 0;
}

static int cmd_moru_ota_verify(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t calc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (moru_ota.base == 0 || moru_ota.expect == 0U) {
		shell_error(sh, "받은 것이 없다.");
		return -EINVAL;
	}

	if (moru_ota.got != moru_ota.expect) {
		shell_error(sh, "덜 받았다 — %u / %u", moru_ota.got, moru_ota.expect);
		return -EAGAIN;
	}

	/* 캐시 매핑이라 빠르지만, 수치를 보여 주고 시작한다 — 멈춘 것처럼
	 * 보이지 않게 하는 것이 진단에 중요하다 (NONE 매핑 때 그렇게 보였다) */
	shell_print(sh, "CRC32 계산 중 — %u 바이트 ...", moru_ota.expect);

	calc = crc32_ieee((const uint8_t *)(moru_ota.base + MORU_OTA_HDR_SIZE),
			  moru_ota.expect);

	shell_print(sh, "CRC32 계산 0x%08x / 기대 0x%08x", calc, moru_ota.crc);
	if (calc != moru_ota.crc) {
		shell_error(sh, "❌ 어긋난다 — 다시 받는다 (begin).");
		return -EIO;
	}

	/*
	 * 헤더를 채우고 캐시를 밀어낸다. 매직은 **아직 쓰지 않는다** —
	 * boot.scr 의 적용 경로(3단계)가 없는 상태에서 매직을 쓰면 다음
	 * 부팅에 아무 일도 일어나지 않지만, 3단계가 들어간 뒤 실수로 반쪽
	 * 이미지가 적용되는 길을 미리 막아 둔다. apply 가 매직을 쓴다.
	 */
	sys_write32(moru_ota.expect, moru_ota.base + MORU_OTA_OFF_SIZE);
	sys_write32(calc, moru_ota.base + MORU_OTA_OFF_CRC);
	sys_write32(0U, moru_ota.base + MORU_OTA_OFF_PATTERN);
	moru_ota_flush(moru_ota.base, MORU_OTA_HDR_SIZE + moru_ota.expect);

	moru_ota.verified = true;

	shell_print(sh, "✅ 일치한다. 헤더(크기·CRC)를 쓰고 캐시를 밀어냈다.");
	shell_print(sh, "   이제 'moru ota apply' 로 적용할 수 있다.");
	return 0;
}

/*
 * ── OTA 적용 ─────────────────────────────────────────────────────────
 *
 * 매직 "MORU" 를 쓰고 재부팅한다. boot.scr 이 그것을 보고 fatwrite 로
 * SD 의 zephyr.bin 을 교체한다 (sdcard/boot.cmd 의 OTA 블록).
 *
 * ⚠ verify 를 통과해야만 매직을 쓴다. U-Boot 는 CRC 를 검증할 수 없다 —
 *   crc32 결과를 변수로 받을 setexpr 가 없다(실측). 그래서 **여기가 유일한
 *   무결성 관문**이고, 반쪽 이미지가 SD 에 쓰이는 것을 막는 유일한 수단이다.
 *
 * ⚠ 이미지 뒤를 256 KiB 까지 0 으로 채운다. boot.scr 이 고정 256 KiB 를
 *   쓰기 때문이다(크기를 변수로 읽을 수 없다). 0 을 채우면 쓰레기가 아니라
 *   0 이 SD 에 들어가고, 그 영역은 Zephyr 의 .bss 라 arch_bss_zero() 가
 *   어차피 지운다.
 *
 * ⚠ 벽돌이 되지 않는다. boot.scr 이 먼저 기존 zephyr.bin 을
 *   zephyr.bin.old 로 백업하고, fatwrite 가 실패하면 기존 것을 그대로
 *   부팅한다. 매직이 틀리면 OTA 블록 자체가 건너뛰어진다.
 */
static int cmd_moru_ota_apply(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t pad;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (moru_ota.base == 0 || moru_ota.expect == 0U) {
		shell_error(sh, "받은 것이 없다.");
		return -EINVAL;
	}
	if (!moru_ota.verified) {
		shell_error(sh, "'moru ota verify' 를 먼저 통과해야 한다.");
		shell_error(sh, "U-Boot 는 CRC 를 검증할 수 없으므로 여기가 유일한");
		shell_error(sh, "무결성 관문이다 — 건너뛰면 반쪽 이미지가 SD 에 쓰인다.");
		return -EPERM;
	}

	/* 이미지 뒤를 0 으로 — boot.scr 이 고정 256 KiB 를 쓴다 */
	pad = MORU_OTA_MAX_SIZE - moru_ota.expect;
	shell_print(sh, "뒤쪽 %u 바이트를 0 으로 채운다 (고정 256 KiB 쓰기용) ...",
		    pad);
	(void)memset((void *)(moru_ota.base + MORU_OTA_HDR_SIZE + moru_ota.expect),
		     0, pad);

	/* 매직을 마지막에 쓴다 — 그 전에 전원이 끊겨도 적용되지 않는다 */
	sys_write32(MORU_OTA_MAGIC, moru_ota.base + MORU_OTA_OFF_MAGIC);
	moru_ota_flush(moru_ota.base, MORU_OTA_HDR_SIZE + MORU_OTA_MAX_SIZE);

	shell_print(sh, "매직 MORU 를 썼다. 재부팅하면 boot.scr 이 적용한다:");
	shell_print(sh, "  zephyr.bin -> zephyr.bin.old 백업 후 교체");
	shell_print(sh, "⚠ USB 가 끊기므로 이 셸도 끊긴다. 재부팅 뒤 'moru info'");
	shell_print(sh, "  의 build 해시가 바뀌었으면 성공이다.");

	log_panic();
	k_msleep(50);

	{
		struct arm_smccc_res res = { 0 };

		arm_smccc_smc(MORU_PSCI_SYSTEM_RESET, 0, 0, 0, 0, 0, 0, 0, &res);
		shell_error(sh, "재부팅 실패 — SMC 가 돌아왔다 (a0=0x%08lx).", res.a0);
	}
	return -EIO;
}

static int cmd_moru_ota_abort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	moru_ota.active = false;
	uart_irq_rx_disable(DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart1)));
	moru_ota.got = 0;
	moru_ota.expect = 0;
	moru_ota.overrun = 0;
	if (moru_ota.base != 0) {
		sys_write32(0U, moru_ota.base + MORU_OTA_OFF_MAGIC);
	}
	shell_print(sh, "취소했다. 매직도 지웠다.");
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
	count = sys_read32(base + MORU_OTA_OFF_SIZE);

	shell_print(sh, "0x%08lx: magic 0x%08x pattern 0x%08x count %u",
		    MORU_OTA_PHYS, magic, pattern, count);

	if (magic == MORU_MARK_MAGIC && pattern == MORU_OTA_PATTERN) {
		shell_print(sh, "✅ 보존됐다 — 재부팅을 넘어 DRAM 이 남는다.");
		shell_print(sh, "   RAM→SD OTA 경로가 열린다 (docs/ota-strategy.md §2).");
		return 0;
	}

	if (magic == 0U && pattern == 0U) {
		shell_warn(sh, "매직이 0 이다. 둘 중 하나다:");
		shell_warn(sh, "  ① OTA 적용 뒤 boot.scr 의 mw.l 이 지웠다 (정상)");
		shell_warn(sh, "  ② mark 한 적이 없거나 DDR 초기화가 비웠다");
		shell_warn(sh, "  → count 가 0 이 아니면 ①이다 (크기 필드가 남아 있다)");
	} else {
		shell_warn(sh, "❌ 값이 다르다 — 보존되지 않거나 다른 용도로 쓰인 영역이다.");
	}
	shell_warn(sh, "   A/B 슬롯 chain-load 로 간다 (docs/ota-strategy.md §3).");
	shell_warn(sh, "   ⚠ 아직 'moru ota mark' 를 한 적이 없으면 이 결과는 무의미하다.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(moru_ota_subcmds,
	SHELL_CMD_ARG(begin, NULL,
		      "수신 시작: begin <크기> <crc32(16진수)> [poll]",
		      cmd_moru_ota_begin, 3, 1),
	SHELL_CMD_ARG(status, NULL, "수신 진행 상황", cmd_moru_ota_status, 1, 0),
	SHELL_CMD_ARG(verify, NULL, "받은 것의 CRC32 를 검사한다",
		      cmd_moru_ota_verify, 1, 0),
	SHELL_CMD_ARG(apply, NULL,
		      "적용: 매직을 쓰고 재부팅한다 (verify 통과 필요)",
		      cmd_moru_ota_apply, 1, 0),
	SHELL_CMD_ARG(abort, NULL, "수신 취소 + 매직 지우기",
		      cmd_moru_ota_abort, 1, 0),
	SHELL_CMD_ARG(mark, NULL, "DRAM 보존 확인 표시 (MRK0, 재부팅 전)",
		      cmd_moru_ota_mark, 1, 0),
	SHELL_CMD_ARG(check, NULL, "그 표시가 재부팅을 넘어 남았는지 본다",
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
		  "OTA — 수신·검증·DRAM 보존 확인", NULL),
	SHELL_CMD_ARG(usb, NULL, "dwc3 레지스터 (DALEPENA 로 EP 설정 판정)",
		      cmd_moru_usb, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(moru, &moru_subcmds, "MoruLab 시스템 관리", NULL);
