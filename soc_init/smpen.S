/*
 * smpen.S — 부팅 초기 훅에서 **읽기만** 한다 (진단).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ── 어디까지 왔나 ───────────────────────────────────────────────────────
 *
 * 실기 측정 (apps/llm_l8c · llm_l8d):
 *   · 페이지 표는 Normal WB RW-alloc · SH=3(내부 공유) — 캐시 가능이다
 *   · `CPUECTLR_EL1` = 0x0020000000040000 → **SMPEN(bit 6) = 0**
 *   · 읽기 대역이 크기와 무관하게 평평하다 (L1 837 · L2 730 · DRAM 707 MiB/s)
 *   · CPU 는 815 MHz (PMU 로 직접 측정)
 *
 * Cortex-A55 에서 SMPEN=0 이면 내부 공유 캐시 가능 메모리가 사실상 비캐시로
 * 동작한다 — 증상이 맞는다. 그래서 **부팅 초기에 켜려** 했다.
 *
 * ── 두 번 틀렸다 ────────────────────────────────────────────────────────
 *
 * ① EL1 훅만 덮었다 → SMPEN 이 그대로 0 이었다.
 *    `moru info` 의 "EL1" 은 Zephyr 가 **내려온 뒤**의 CurrentEL 이다.
 *    arm64 는 보통 EL2 로 들어온다.
 *
 * ② 세 훅 전부에 **쓰기**를 넣었다 → 보드가 **한 글자도 못 찍고** 죽었다.
 *    훅은 콘솔 초기화 전이라 출력 수단이 없다. try 슬롯 덕에 복구됐다.
 *
 * ⚠ ②에서 "쓰기가 거부됐다" 고 결론 내릴 수 없다. 침묵은 여러 가지를
 *   뜻한다 — 어느 훅에서 죽었는지, 레지스터를 **읽을** 수는 있는지,
 *   아니면 전혀 다른 이유인지 모른다.
 *
 * ── 그래서 이 판은 쓰지 않는다 ──────────────────────────────────────────
 *
 * 세 훅이 각자 **흔적만 남긴다**:
 *   · 자기가 실행됐다는 표식 (어느 EL 로 들어오는지 알려 준다)
 *   · 그 시점의 `CPUECTLR_EL1` 값 (그 EL 에서 **읽기**가 되는지 알려 준다)
 * 앱이 나중에 그것을 찍는다. 쓰기는 **하지 않는다** — 다음 판에서 어느
 * 훅에 넣을지 알고 넣는다.
 *
 * ⚠ 제약 (reset.S:20-22): 스택이 없고 x23·x24·x25 를 보존해야 한다.
 *   x0·x1 만 쓴다. MMU·캐시가 꺼진 상태라 쓰기는 DRAM 으로 직행한다.
 *   `.noinit` 에 두어 Zephyr 의 bss 지우기가 덮지 않게 한다.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <zephyr/arch/cpu.h>

/*
 * 흔적 저장소. 앱이 `rk3566_boot_trace` 로 읽는다.
 *   [0] 실행된 훅 비트맵 (bit0=EL3 bit1=EL2 bit2=EL1)
 *   [1] EL3 훅이 본 CPUECTLR
 *   [2] EL2 훅이 본 CPUECTLR
 *   [3] EL1 훅이 본 CPUECTLR
 */
.section .noinit, "aw"
.align 3
.globl rk3566_boot_trace
rk3566_boot_trace:
	.quad 0, 0, 0, 0

GTEXT(z_arm64_el3_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el3_plat_prep_c)
	adrp	x1, rk3566_boot_trace
	add	x1, x1, :lo12:rk3566_boot_trace
	ldr	x0, [x1]
	orr	x0, x0, #1
	str	x0, [x1]
	mrs	x0, S3_0_C15_C1_0
	str	x0, [x1, #8]
	ret

GTEXT(z_arm64_el2_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el2_plat_prep_c)
	adrp	x1, rk3566_boot_trace
	add	x1, x1, :lo12:rk3566_boot_trace
	ldr	x0, [x1]
	orr	x0, x0, #2
	str	x0, [x1]
	mrs	x0, S3_0_C15_C1_0
	str	x0, [x1, #16]
	ret

GTEXT(z_arm64_el1_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el1_plat_prep_c)
	adrp	x1, rk3566_boot_trace
	add	x1, x1, :lo12:rk3566_boot_trace
	ldr	x0, [x1]
	orr	x0, x0, #4
	str	x0, [x1]
	mrs	x0, S3_0_C15_C1_0
	str	x0, [x1, #24]
	ret
