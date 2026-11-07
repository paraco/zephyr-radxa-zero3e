/*
 * smpen.S — Cortex-A55 의 `CPUECTLR_EL1.SMPEN` 을 **부팅 초기에** 켠다.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ── 왜 ──────────────────────────────────────────────────────────────────
 *
 * 실기에서 측정했다 (apps/llm_l8d):
 *     CPUECTLR_EL1 = 0x0020000000040000  →  SMPEN(bit 6) = **0**
 *     읽기 대역: L1 32 KiB 837 · L2 256 KiB 730 · DRAM 32 MiB 707 MiB/s
 * 크기를 바꿔도 평평하다 — **캐시가 먹지 않는다.**
 *
 * 그런데 페이지 표는 캐시 가능이라고 말한다 (apps/llm_l8c):
 *     AttrIndx=4 → MAIR 0xff = Normal WB RW-alloc · **SH=3(내부 공유)**
 *
 * 둘이 모순이 아니다. Cortex-A55 에서 `SMPEN=0` 이면 **내부 공유 캐시 가능
 * 메모리가 사실상 비캐시로 동작한다.** ARM 문서가 "캐시와 MMU 를 켜기 전에
 * SMPEN 을 설정해야 한다" 고 적은 그 비트다.
 *
 * U-Boot·TF-A 가 켜 주지 않았고, **Zephyr arm64 도 이 비트를 전혀 건드리지
 * 않는다** (`grep SMPEN arch/ soc/` 가 비어 있다). 그래서 우리가 켠다.
 *
 * ── 왜 여기(이 시점)인가 ────────────────────────────────────────────────
 *
 * 런타임에 켜 보려 했더니 **예외가 났다** (apps/llm_l8d):
 *     "Synchronous Abort" esr 0x8a000000 → EC 0x22 (PC 정렬 오류)
 * EL3 트랩(EC 0x18)이 **아니다** — 쓰기 자체는 먹었고, "캐시 켜진 뒤의
 * SMPEN 변경은 미정의" 라는 ARM 경고가 그대로 나타난 것이다.
 *
 * `z_arm64_el1_plat_prep_c` 는 `arch/arm64/core/reset.S:83` 에서 불리고
 * **MMU·캐시가 켜지기 전**이다. weak 심볼이라 덮어쓰면 된다 —
 * **Zephyr 수정 0건.**
 *
 * ── 왜 어셈블리인가 ─────────────────────────────────────────────────────
 *
 * reset.S:20-22 의 주석이 못박는다:
 *     Note: - Stack is not yet available
 *           - x23, x24 and x25 must be preserved
 * **스택이 없다.** C 함수는 프롤로그에서 스택을 쓰므로 쓸 수 없다.
 * 이 코드는 x0 만 쓰고 x23~x25 를 건드리지 않는다.
 *
 * ── 진입 EL 을 잘못 짚었다 (2026-11-07) ────────────────────────────────
 *
 * 처음에 EL1 훅만 덮었다. `moru info` 가 "CPU … EL1" 을 찍으니 EL1 로
 * 들어온다고 본 것이다. **틀렸다** — 그 값은 Zephyr 가 EL2 에서 EL1 로
 * **내려온 뒤**의 CurrentEL 이다.
 *
 * 실기에서 EL1 훅만 덮은 이미지를 돌렸더니 `SMPEN` 이 여전히 **0** 이었다.
 * arm64 는 U-Boot/TF-A 가 보통 **EL2** 로 넘기고, 그러면 `reset.S:69` 의
 * `z_arm64_el2_plat_prep_c` 가 불린다. EL1 훅은 지나가지 않는다.
 *
 * 그래서 **세 훅을 전부 덮는다.** 셋 다 weak 이고, 어느 EL 로 들어와도
 * 하나는 걸린다. 같은 코드를 세 번 두는 것이 부담스럽지만, "어느 EL 로
 * 들어오는가" 를 추측하는 것보다 낫다 — 추측이 이미 한 번 틀렸다.
 *
 * ⚠ `CPUECTLR_EL1` 은 EL1 이름이지만 **EL2·EL3 에서도 접근된다.**
 *   (상위 EL 은 하위 EL 의 구현 정의 레지스터를 볼 수 있다.)
 *
 * ⚠ `CPUECTLR_EL1` 은 **구현 정의** 레지스터다 (S3_0_C15_C1_0). Cortex-A55·
 *   A53·A57·A72 에서 이 인코딩이고 SMPEN 이 bit 6 다. 다른 코어로 옮기면
 *   **다시 확인해야 한다.**
 *
 * ⚠ EL1 에서 쓰려면 EL3 가 `ACTLR_EL3.CPUECTLR` 로 허락해야 한다. EL2 라면
 *   `ACTLR_EL2` 다. 막혀 있으면 쓰기가 무시되거나 트랩된다 — L8d 가 읽어
 *   찍으므로 **먹었는지 아닌지 보인다.**
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <zephyr/arch/cpu.h>

/*
 * 세 훅이 같은 일을 한다. 매크로로 묶지 않고 펼쳐 쓴다 —
 * 어셈블리 매크로는 이 파일을 읽는 사람에게 한 겹 더 숨기는 일이다.
 * x0 만 쓰고 x23~x25 는 건드리지 않는다 (reset.S:20-22 의 제약).
 */

GTEXT(z_arm64_el3_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el3_plat_prep_c)
	mrs	x0, S3_0_C15_C1_0
	orr	x0, x0, #(1 << 6)
	msr	S3_0_C15_C1_0, x0
	isb
	ret

GTEXT(z_arm64_el2_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el2_plat_prep_c)
	mrs	x0, S3_0_C15_C1_0
	orr	x0, x0, #(1 << 6)
	msr	S3_0_C15_C1_0, x0
	isb
	ret

GTEXT(z_arm64_el1_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el1_plat_prep_c)
	mrs	x0, S3_0_C15_C1_0
	orr	x0, x0, #(1 << 6)
	msr	S3_0_C15_C1_0, x0
	isb
	ret
