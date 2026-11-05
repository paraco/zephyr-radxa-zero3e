# 하드웨어 사실의 출처 — TRM 근거 대장

이 저장소의 모든 하드웨어 상수(주소·오프셋·비트·인터럽트 번호·클럭)에는
출처가 있어야 한다. 이 문서는 **그 등급을 정하고, 지금 무엇이 미확인인지,
그리고 TRM 이 애초에 답할 수 없는 영역은 어디인지**를 적는다.

정본 문서: **Rockchip RK3568 TRM Part1 V1.3 / Part2 V1.1**
(RK3566 은 RK3568 과 같은 TRM 을 쓴다. 다이 차이는 주변장치 개수·핀 수다.)
Part1: `rockchip.fr/Rockchip%20RK3568%20TRM%20V1.3%20Part1.pdf` (11.3 MB)
⚠ 전체 파싱은 시간 초과한다 — **장 단위로 접근한다.**
⚠ 내려받기는 사용자 승인을 받는다 (이전 승인은 이어지지 않는다).

---

## 1. 왜 TRM 이 정본이어야 하는가 — 실측된 이유

**Zephyr 인트리 `dts/arm64/rockchip/rk3568.dtsi` 를 신뢰할 수 없다.**
같은 노드 하나(`uart2`)에서 두 값이 의심스럽다:

| 인트리 값 | 우리 값 | 근거 |
|---|---|---|
| `clock-frequency = <12000000>` | **24000000** | 분주비 계산 + 실측. 12 MHz 면 115200 에서 오차 7%, 1500000 에서 50% — 한 바이트도 못 받는다. **실기로 오류 입증** |
| `interrupts = <GIC_SPI 118 IRQ_TYPE_EDGE ...>` | **IRQ_TYPE_LEVEL** (`apps/moru/app.overlay`) | Linux 메인라인 DT. **TRM 미확인** |

하나가 실측으로 틀렸음이 확인된 파일의 다른 값도 의심해야 한다. 그리고
**트리거 타입은 TRM 인터럽트 표만이 정본**이다 — Linux DT 도 2차 출처다.

지금 폴링 모드(`SHELL_BACKEND_SERIAL_API_POLLING=y`)라 트리거 타입이
무해하지만, 인터럽트 구동으로 바꾸는 순간 정본이 필요해진다.

---

## 2. 출처 등급

새 하드웨어 상수를 코드에 넣을 때 **등급과 출처를 주석에 쓴다.
등급 없는 상수는 커밋하지 않는다.**

| 등급 | 뜻 | 예 |
|---|---|---|
| **T** | TRM 에서 직접 확인 (절·쪽 기록) | USB2PHY GRF 비트 (Part1 V1.3 3.9절 419쪽) |
| **L** | 이 보드에서 실측 (라이브 DT·sysfs·레지스터 읽기) | DRAM `0x00200000~0x80000000`, dwc3 SPI 169 |
| **D** | Linux 메인라인 디바이스트리 (MIT 선택 가능) | uart2 트리거 타입, 칩 ID 의 OTP 오프셋 |
| **C** | 타 OS 드라이버에서 **사실만** 확인 (코드는 베끼지 않음) | GPIO v2 오프셋 (FreeBSD, BSD-2) · OTP 절차 (Linux, GPL-2.0) |
| **I** | 계산·추론 | uart2 24 MHz (분주비가 정수여야 한다는 제약에서) |
| **G** | 근거 없는 추측 | **코드에 남아 있으면 안 된다** |

`T` 와 `L` 이 어긋나면 **`L` 이 이긴다** (실물이 문서를 이긴다). 어긋난
사실 자체를 기록한다.

---

## 3. 오늘까지의 실패를 TRM 이 막을 수 있었나 — 건별 판정

**정직한 결론을 먼저 쓴다: 막을 수 있었던 것은 7건 중 1건이다.**
TRM 을 확보하는 것이 bring-up 삽질의 해답이 아니다. 이 착각을 막기 위해
판정을 남긴다.

| 실패 | TRM 판정 | 설명 |
|---|---|---|
| `CONFIG_INIT_STACKS` 로 부팅 불가 (왕복 3회) | **무관** | Zephyr arm64 리셋 경로 문제. TRM 에 Zephyr 는 없다 |
| `IRQ_TYPE_EDGE` 를 원인으로 오판 | **막을 수 있었다** | TRM 인터럽트 표가 트리거 타입을 확정한다. 가설을 바로 버릴 수 있었다 |
| `DW8250` busy-detect 를 원인으로 오판 | **부분** | TRM UART 장이 USR/busy-detect 레지스터 존재를 답한다. 원인은 아니었지만 **빨리 버릴 수 있었다** |
| `moru_stage.c` 의 `device_map()` @ `PRE_KERNEL_1` | **무관** | Zephyr init 순서 |
| LED 스레드 스택 1024 로 크래시 | **무관** | Zephyr 스택 요구량 |
| tio 버퍼링·보율 불일치·`mdir` 미확인 | **무관** | 호스트 도구 |
| 기준선 없이 변경 20개를 쌓음 (왕복 4회) | **무관** | 절차. [bringup-rules.md](bringup-rules.md) 참조 |

즉 **가장 비싼 두 항목(왕복 3회 + 4회)은 TRM 과 무관했다.** 비용을 줄이는
것은 TRM 이 아니라 작업 규율이다.

---

## 4. 증상을 보고 TRM 을 볼 일인지 즉시 판정하는 표

| TRM 이 정본인 것 | TRM 사정권 밖 (봐도 답이 없다) |
|---|---|
| 레지스터 주소·오프셋·비트 이름·RO/RW | Zephyr Kconfig 의 상호작용·`select` 번짐 |
| 메모리맵·예약 영역 | Zephyr init level 순서, 링커 배치 |
| 인터럽트 번호·**트리거 타입** | U-Boot 가 넘길 때의 MMU·캐시·EL 상태 |
| 클럭 트리·분주·게이트·소프트리셋 | TF-A / BL31 이 실제로 지원하는 PSCI 범위 |
| IO 다중화(iomux)·드라이브 세기 | 호스트 도구 (tio·mtools·picotool·ioreg) |
| 리셋 종류와 절차 (CRU_GLB_SRST) | 보드 배선·헤더 핀 배치 (Radxa 사양서) |
| 전원 도메인·PHY 설정 | SD 카드 굽기 절차, U-Boot 환경변수 |
| hiword-mask 규약 | Zephyr 드라이버 모델·DT 바인딩 |

**판정 규칙:** 증상이 "하드웨어가 이 비트에 어떻게 반응하는가" 면 TRM.
"소프트웨어가 왜 이 순서로 도는가" 면 TRM 이 아니다.

---

## 5. 지금 미확인 — 다음에 TRM 을 받으면 볼 목록

우선순위 순. 각 항목에 **지금 등급**과 **TRM 의 어느 장**인지 적었다.

| 우선 | 항목 | 지금 | TRM 위치 | 필요한 이유 |
|---|---|---|---|---|
| **1** | `CRU_GLB_SRST_FST` / `_SND` 오프셋·매직값 | **없음** | Part1 CRU 장 | **B4 `moru reboot`** — PSCI(BL31)가 NS-EL1 SMC 에 응답하지 않으면 이것이 유일한 대안 |
| **2** | uart2 인터럽트 트리거 타입 (SPI 118) | D | Part1 인터럽트 요약표 | 인터럽트 구동으로 바꿀 때 필요. 오늘 오판한 지점 |
| **3** | RK3566 UART 에 `USR`/busy-detect 가 있는가 | **미확인** (`apps/moru/prj.conf:93` 에 숙제로 적힘) | Part1 UART 장 | `UART_NS16550_DW8250_DW_APB` 를 켜야 하는지 판정 |
| 4 | GPIO0 base `0xfdd60000` + `SWPORT_DR_L` 0x00 · `DDR_L` 0x08 | C (FreeBSD) + L | Part1 GPIO 장 | 등급을 T 로 올린다. 지금 네 앱 전부가 이 값에 의존 |
| 5 | uart2 base `0xfe660000`, 레지스터 간격 | 인트리 dtsi | Part1 메모리맵 | §1 의 이유로 인트리 dtsi 를 신뢰할 수 없다 |
| 6 | uart2 실제 클럭 소스와 분주 (24 MHz 확인) | I + L | Part1 CRU 클럭 트리 | 계산으로 24 MHz 라 추론했다. 클럭 트리로 확정 |
| 7 | `0x00000000~0x00200000` 이 왜 DRAM 에서 빠졌나 | 추론(TF-A 예약) | Part1 메모리맵 | `INIT_STACKS` 조사와 관련될 수 있다 |
| — | OTP 컨트롤러 레지스터·절차 | C (Linux) | **TRM 양쪽에 없음 — 확인 완료** | Part2 33개 장 전부 확인. 더 찾을 필요 없다 |

이미 확보된 것(다시 받을 필요 없음): USB2PHY_U3_GRF `0xFDCA0000` 전체 비트
(Part1 V1.3 3.9절 419쪽) · CRU_NS `0xFDD20000` · OTP_NS `0xFE38C000` ·
`CRU_GATE_CON26`(0x0368)·`CON34`(0x0388) · `CRU_SOFTRST_CON22`(0x0458)·
`CON28`(0x0470) · hiword-mask 규약.

---

## 6. 남은 계획(B1~B4)에서 TRM 이 필요한 지점

| 묶음 | TRM 필요 | 비고 |
|---|---|---|
| **B1** Linux 형식 부팅 메시지 | **없음** | 전부 Zephyr 쪽 |
| **B2** `moru info` · `VERSION` · 셸 모듈 | **없음** | hwinfo 는 이미 TRM 근거 확보 |
| **B3** `moru usb` 진단 API | **없음** | dwc3 는 Synopsys/Fuchsia, PHY GRF 는 3.9절 확보 |
| **B4** `moru reboot` | **PSCI 실패 시 필수** | §5 의 1번 항목. `moru reboot probe` 로 PSCI 를 먼저 가린다 |

→ **TRM 을 다시 받을 실제 사유는 B4 와 §5 의 2·3번뿐이다.** 그 셋을 묶어
한 번에 받는다 (장 단위 접근).

---

## 7. 미해결 — `CONFIG_INIT_STACKS` 는 TRM 영역이 아니다

유죄는 입증됐고(실기 3회) **기전은 설명되지 않았다.** 확인된 범위:

- 동기 마커(`uart_poll_out`)조차 안 나오므로 **`PRE_KERNEL_2` 이전**에 죽는다.
  → 스레드 스택 칠하기(`thread.c`)는 범인이 아니다. 스레드 생성 전이다.
- `arch/arm64/core/reset.S:199-215` 가 `__reset_prep_c` **뒤**, MMU·EL 전환
  **앞**에서 `sp` 기준으로 ISR 스택을 0xAA 로 칠한다.
- 그 루프는 이 보드 수치로 정상이다: `ISR_STACK_SIZE=4096` =
  `__z_interrupt_stack_SIZEOF` 0x1000, 시작·끝 차이 0xFF8(8의 배수)이므로
  `beq` 가 정확히 종료하고 영역을 벗어나 쓰지 않는다.
- 같은 성질의 쓰기(`__reset_prep_c` 의 .bss 지우기)는 **성공한다.**

즉 **정적 분석으로는 결함이 없다.** 다음 실험은 TRM 이 아니라 이 셋이다:

1. **다른 arm64 보드에서 `INIT_STACKS=y` 가 되는지** — Zephyr 일반 문제인지
   이 보드 특정 문제인지 가른다 (실기 0회, QEMU 로 가능).
2. **`reset.S` 의 그 루프만 지우고 `INIT_STACKS=y`** — 참이면 범인이 그
   루프로 확정되고 **Zephyr 쪽에 보고할 거리**가 된다 (실기 1회).
3. **`reset.S` 통과 여부를 GPIO raw MMIO 로** — MMU 가 꺼진 상태에서도 물리
   주소 직접 쓰기는 된다. UART 는 못 쓴다 (Zephyr 가 115200 으로 재설정하기
   전이라 U-Boot 의 1.5 Mbps 로 나가고, 쓰는 CP2102 는 1 Mbps 한계다).

TRM 이 간접적으로 관련될 수 있는 유일한 지점은 §5 의 7번(DRAM 하단 예약
영역)이다. 그것이 `z_interrupt_stacks` 와 겹칠 가능성은 낮지만 — 이미지는
`0x40000000` 에 있고 예약 영역은 `0x00200000` 아래다 — 메모리맵으로
교차 확인할 가치는 있다.

---

## 관련 문서

- [bringup-rules.md](bringup-rules.md) — 실기 왕복 규율 (반복한 실수와 체크리스트)
- [hardware-verified.md](hardware-verified.md) — 실기 기록 (시간순)
- `README.md` §출처와 라이선스 — 라이선스 관점의 출처 표
