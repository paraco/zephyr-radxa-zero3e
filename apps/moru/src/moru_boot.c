/*
 * MoruLab Zephyr RTOS — 부팅 메시지 (Linux dmesg 형식)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 모든 출력은 LOG_INF 다. printk 는 동기 블로킹이라 쓰지 않는다.
 * 값은 DT·Kconfig·시스템 레지스터에서 읽은 것만 찍는다.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/app_version.h>
#include <zephyr/version.h>
#include <zephyr/sys/util.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(moru, LOG_LEVEL_INF);

#ifndef MORU_BUILD_TIME
#define MORU_BUILD_TIME "unknown"
#endif

/*
 * 보드 모델. DT 루트의 model 속성은 바인딩에 없어 매크로가 생성되지 않으므로
 * (DT_PROP 불가, dt_prop() 도 빈 값) 고정 문자열로 둔다. 이 앱은 이미 보드
 * 전용이다 — 아래 SoC 문구와 main.c 의 GPIO0 주소도 하드코딩이다.
 */
#define MORU_MODEL "Radxa ZERO 3E"

#define SEP "===================================================="

/*
 * 로그 타임스탬프를 24 MHz 사이클 카운터로 바꾼다.
 *
 * Zephyr 기본값은 log_core.c:282 가 사이클 주파수를 보고 고르는데, 이 보드는
 * 24 MHz > 1 MHz 라 k_uptime_get()(1000 Hz)이 선택된다. 그러면 Linux 형식
 * 출력의 µs 끝 세 자리가 늘 0 이 되어 "[    0.123000]" 처럼 찍힌다.
 * arm arch timer 가 TIMER_HAS_64BIT_CYCLE_COUNTER 를 select 하므로
 * k_cycle_get_64() 를 쓸 수 있고, 그걸 걸면 진짜 µs 가 나온다.
 *
 * PRE_KERNEL_1 우선순위 0 에 둔다 — 다른 어떤 로그보다 먼저 바꿔야 한다.
 * 시간 기준이 중간에 바뀌면 log_output 이 모든 줄을 '현재' 주파수로 나누므로
 * 앞선 줄들의 시각이 틀리게 찍힌다. k_cycle_get_64() 는 CNTVCT_EL0 를 읽을
 * 뿐이고 그 카운터는 리셋부터 돌므로 이 시점에도 안전하다.
 */
static log_timestamp_t moru_cycle_timestamp(void)
{
	return (log_timestamp_t)k_cycle_get_64();
}

static int moru_boot_timestamp(void)
{
	uint64_t freq = read_cntfrq_el0();

	/*
	 * CNTFRQ_EL0 를 펌웨어가 채우지 않는 경우가 있다. 값이 터무니없으면
	 * 기본 타임스탬프(1 ms)를 그대로 쓴다 — 해상도를 잃는 편이 시각이
	 * 엉뚱하게 찍히는 것보다 낫다.
	 */
	if (freq < 1000000U || freq > 1000000000U) {
		return 0;
	}

	(void)log_set_timestamp_func(moru_cycle_timestamp, (uint32_t)freq);
	return 0;
}
SYS_INIT(moru_boot_timestamp, PRE_KERNEL_1, 0);

/* lib_helpers.h 에는 midr_el1·ctr_el0 접근자가 없어 read_sysreg() 를 쓴다 */
#define read_midr_el1()	read_sysreg(midr_el1)
#define read_ctr_el0()	read_sysreg(ctr_el0)

static const char *cpu_part_name(uint32_t implementer, uint32_t part)
{
	if (implementer != 0x41) {
		return "unknown";
	}
	switch (part) {
	case 0xd03: return "Cortex-A53";
	case 0xd04: return "Cortex-A35";
	case 0xd05: return "Cortex-A55";
	case 0xd07: return "Cortex-A57";
	case 0xd08: return "Cortex-A72";
	case 0xd09: return "Cortex-A73";
	case 0xd0a: return "Cortex-A75";
	case 0xd0b: return "Cortex-A76";
	default:    return "unknown";
	}
}

static int moru_boot_banner(void)
{
	uint64_t midr = read_midr_el1();
	uint32_t part = (midr >> 4) & 0xfff;
	uint32_t impl = (midr >> 24) & 0xff;

	LOG_INF(SEP);
	LOG_INF("MoruLab Zephyr RTOS %s (build %s)", APP_VERSION_STRING,
		STRINGIFY(APP_BUILD_VERSION));
	LOG_INF("Zephyr %s, built %s", KERNEL_VERSION_STRING, MORU_BUILD_TIME);
	LOG_INF(SEP);
	LOG_INF("Machine model: %s", MORU_MODEL);
	LOG_INF("SoC: Rockchip RK3566 (%s SoC 정의 재사용)", CONFIG_SOC);
	LOG_INF("CPU: %s, MIDR 0x%08llx, EL%u, MPIDR 0x%08llx",
		cpu_part_name(impl, part), (unsigned long long)midr,
		(unsigned int)GET_EL(read_currentel()),
		(unsigned long long)read_mpidr_el1());
	{
		uint64_t ctr = read_ctr_el0();
		/* DminLine = CTR_EL0[19:16], 단위는 4 B 워드 수의 log2 */
		uint32_t dline = 4U << ((ctr >> 16) & 0xf);

		LOG_INF("Cache: min D-line %u B (CTR_EL0 0x%08llx)", dline,
			(unsigned long long)ctr);
	}
	return 0;
}
SYS_INIT(moru_boot_banner, PRE_KERNEL_2, 1);

static int moru_boot_memory(void)
{
	uint64_t freq = read_cntfrq_el0();

	LOG_INF("Memory: 0x%08lx + %lu KiB (zephyr,sram)",
		(unsigned long)CONFIG_SRAM_BASE_ADDRESS,
		(unsigned long)CONFIG_SRAM_SIZE);
	LOG_INF("Timer: ARM arch timer %u.%06u MHz, %u ticks/s, %s",
		(unsigned int)(freq / 1000000U),
		(unsigned int)(freq % 1000000U),
		(unsigned int)CONFIG_SYS_CLOCK_TICKS_PER_SEC,
		IS_ENABLED(CONFIG_TICKLESS_KERNEL) ? "tickless" : "periodic");
	return 0;
}
SYS_INIT(moru_boot_memory, PRE_KERNEL_2, 2);

static int moru_boot_console(void)
{
#define CON DT_CHOSEN(zephyr_console)
	LOG_INF("Console: %s %u 8N1, shell on same port",
		DT_NODE_FULL_NAME(CON), (unsigned int)DT_PROP(CON, current_speed));
	LOG_INF("Board: %s", CONFIG_BOARD_TARGET);
	return 0;
}
SYS_INIT(moru_boot_console, POST_KERNEL, 1);
