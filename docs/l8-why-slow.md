# L8 — 왜 23.6초/토큰인가 (원인 규명 완료)

L6c 가 실기에서 **토큰당 23.6초**(0.042 tok/s)를 냈다. A55 한 코어로
0.8B Q4 는 보통 1~4 tok/s 이므로 **25~100배** 느렸다. 성능 플래그를 뒤집기
전에 원인을 재는 것이 L8a~L8d 였다.

## 셈이 맞는다

| 원인 | 배수 | 근거 | 우리가 고칠 수 있나 |
|---|---|---|---|
| CPU **815 MHz** (최대 1.8 GHz) | **2.2×** | PMU 사이클 카운터, 3회 일치 (L8b) | ❌ CRU+PMIC — U-Boot/TF-A 영역 |
| **`SMPEN=0`** → 내부 공유 캐시 무력 | **~5~10×** | `CPUECTLR_EL1`=0x0020000000040000 · 대역이 크기와 무관하게 평평 (L8c·L8d) | ❌ **EL3 가 쓰기를 막는다** |
| **한 코어** (4개 중 1) | **≤4×** | `CONFIG_MP_MAX_NUM_CPUS 1` | ✅ Zephyr SMP + PSCI CPU_ON |
| repack 없음 · `-ffp-contract=off` | 작다 | 모델이 거의 Q4_K 라 repack 효과는 미지 | ✅ |

**2.2 × 5~10 × 4 ≈ 44~88배.** 설명해야 했던 25~100배와 맞는다.

## 확정된 사실

```
CLIDR_EL1 0x82000023   L1 I+D 분리 · D 32 KiB · L2 통합 512 KiB
SCTLR_EL1 0x30d0180d   M=1 C=1 I=1        — 캐시는 켜져 있다
MAIR/페이지표           AttrIndx=4 → 0xff = Normal WB RW-alloc · SH=3
CPUECTLR_EL1           0x0020000000040000 → **SMPEN(bit 6) = 0**
CPU 주파수              **815 MHz** (PMU, 3회)
읽기 대역 (크기 무관)   L1 837 · L2 730 · DRAM 707 MiB/s
부팅 진입 EL            **EL2**
EL2 에서 CPUECTLR       읽기 ✅ / 쓰기 ❌ (트랩 → 리셋)
```

Cortex-A55 에서 `SMPEN=0` 이면 **내부 공유 캐시 가능 메모리가 사실상
비캐시로 동작한다.** 페이지가 캐시 가능이라고 말하는데도 L1·L2·DRAM 대역이
평평했던 것이 그 증상이다.

## 왜 우리가 고칠 수 없나

`CPUECTLR_EL1` 쓰기는 EL3 가 `ACTLR_EL3.CPUECTLR`(bit 1)로 허락해야 한다.
**TF-A(BL31)가 허락하지 않았다.** 증거:

- EL2 훅에서 **읽기**는 된다 (`[0]` 흔적이 값을 가져왔다)
- 같은 자리에서 **쓰기**를 하면 보드가 한 글자도 못 찍고 리셋된다
- 캐시가 켜진 뒤 런타임에 쓰면 `esr 0x8a000000` (EC 0x22) 예외
  — ARM 이 "캐시·MMU 를 켠 뒤의 SMPEN 변경은 미정의" 라고 적은 그대로다

즉 **SMPEN 과 CPU 클럭은 둘 다 U-Boot/TF-A 가 정하고, Zephyr 에서 되돌릴
수 없다.** 이 이미지로 베어메탈·RTOS 를 돌리면 **SoC 능력의 1/10 수준**으로
동작한다.

⚠ 이것은 이 보드의 **부트 펌웨어**에 대한 발견이다. Radxa 의 U-Boot 는
리눅스가 cpufreq·coherency 를 맡는다고 보고 넘긴다. 리눅스는 둘 다 켠다.

## 그래서 할 수 있는 것

| | 무엇 | 얻는 것 | 비용 |
|---|---|---|---|
| **A** | Zephyr SMP + PSCI CPU_ON 으로 **4코어** | ≤4× → **6초/토큰** | Zephyr 작업. PSCI 는 `moru reboot probe` 로 BL31 응답을 먼저 가린다 |
| **B** | repack · `-ffp-contract=fast` · `-O3` | 미지, 아마 1.2~2× | 골든값 재생성 필요(결정 1 의 전제가 바뀐다) |
| **C** | U-Boot/TF-A 를 다시 빌드해 SMPEN·클럭 설정 | **~10×** | 보드 부트 체인 교체. 되돌리기 어렵다 |
| **D** | 지금을 받아들이고 **더 작은 모델** | — | 0.8B 가 23.6초면 0.1B 급은 3초대 |

**A 가 먼저다** — Zephyr 안에서 되고, 4배는 확실하고, 되돌리기 쉽다.
C 는 가장 크지만 보드의 부트 체인을 바꾸는 일이라 **사용자 결정이 필요하다**.

## 측정에서 내가 틀린 것들 (기록)

이 칸들에서 네 번 틀렸고 전부 측정으로 바로잡았다. 남겨 두는 이유는 같은
함정을 다시 밟지 않기 위해서다.

| 틀린 것 | 어떻게 드러났나 |
|---|---|
| "모델 창이 캐시 안 됨" (32 MiB 한 점 비교) | **한 점으로는 두 가설을 못 가른다** — 32 MiB 는 L2 를 넘어 캐시와 무관하게 DRAM 대역에 묶인다. 크기 쓸기 + 페이지 표 직독으로 가렸다 |
| CPU "120~155 MHz" (반복당 4사이클 가정) | PMU 로 직접 재니 **815 MHz**. 가정이 5.5배 틀렸다 (실제 ~21사이클) |
| 진입 EL 이 EL1 | `moru info` 의 "EL1" 은 Zephyr 가 **내려온 뒤**의 CurrentEL. 실제는 **EL2** |
| 세 훅에 쓰기를 넣어도 된다 | 콘솔 초기화 **전**이라 죽으면 진단 불가능. 읽기만 하는 판으로 바꿔 어느 EL 인지 먼저 가렸다 |

**공통점:** 측정이 가설을 **구분할 수 있는지** 먼저 따지지 않았다.
`docs/bringup-rules.md` 규칙 7("출력을 못 읽는 곳에는 입증된 문법만")과
같은 결의 교훈이다.

---

## 2026-11-07 저녁 — SMP 는 이 보드에서 **부팅을 막는다** (실기 확인)

### 무엇을 재려 했나

주 코어의 `SMPEN=0` 은 L8d 가 확인했다. 가설: **PSCI `CPU_ON` 으로 보조
코어를 올리면 BL31 이 그 코어의 리셋 경로를 돌면서 SMPEN 을 켜 줄 수 있다.**
맞다면 멀티코어가 4배가 아니라 20~40배가 되고 펌웨어 재빌드가 불필요하다.

### 결과 — 침묵. 그리고 대조군으로 원인을 좁혔다

| 이미지 | 설정 | 115200 출력 |
|---|---|---|
| `llm_l8e` | `SMP=y` · `MP_MAX_NUM_CPUS=4` · `PM_CPU_OPS_PSCI=y` | **없음** |
| `llm_l8f` | `SMP=y` · `MP_MAX_NUM_CPUS=1` · `PM_CPU_OPS_PSCI=y` | **없음** |
| `llm_l8d` (대조군) | SMP 없음 | **전체 출력** ✅ |

세 번 모두 **같은 카드·같은 `boot.scr`·같은 CP2102 직결**이다. 대조군이
카드의 `zephyr.bin` 경로로 측정 전체를 찍었으므로 **카드·U-Boot·콘솔은
무죄다.**

`MP_MAX_NUM_CPUS=1` 이면 `z_smp_init` 의 루프가 0회라 **`CPU_ON` 을 한 번도
부르지 않는다.** 그런데도 침묵했다. 따라서:

> 범인은 `CPU_ON` 이 **아니다.** `CONFIG_SMP` 골격 자체이거나,
> `PM_CPU_OPS_PSCI` 의 `psci_init`(`PRE_KERNEL_1` — 콘솔보다 먼저
> `PSCI_VERSION` SMC)이다.

⚠ 아직 그 둘을 가리지 않았다. `moru reboot probe` 가 런타임에
`PSCI 1.1 (raw 0x00010001)` 을 받았다는 사실은 **`psci_init` 의 무죄를
증명하지 않는다** — 셸에서 쏜 SMC 와 `PRE_KERNEL_1` 의 SMC 는 시점이 다르다.

### 이 목표에 주는 뜻

멀티코어는 **빠른 승리가 아니다.** 그리고 L8 의 두 큰 인자는 SMP 를
필요로 하지 않는다:

| 인자 | 배수 | SMP 필요? |
|---|---|---|
| 클럭 815 MHz → 1.8 GHz | 2.2× | 아니다 |
| SMPEN 0 → 1 (캐시가 먹는다) | 5~10× | 아니다 |
| 코어 1 → 4 | 4× | **그렇다** |

따라서 **클럭과 SMPEN 을 먼저 노린다.** 그 둘만으로 11~22× 이고, SMP 는
그 뒤에 따로 다룬다.

### 측정값 (대조군 L8d4, 재확인)

```
CLIDR_EL1 0x0000000082000023  (LoUIS=0 LoC=2)
  L1 D: 64 B × 4 way × 128 set = 32 KiB
  L2  : 64 B × 16 way × 512 set = 512 KiB
CPUECTLR_EL1 = 0x0020000000040000   SMPEN=0
읽기 대역: 32 KiB 830 · 256 KiB 730 · 32 MiB 708 MiB/s
```

⚠ **32 KiB(L1)와 32 MiB(DRAM)가 830 대 708 MiB/s — 1.17배밖에 차이가
없다.** L1 캐시가 사실상 먹지 않는다는 직접 증거다. SMPEN=0 이면
inner-shareable 캐시 가능 메모리가 비캐시처럼 동작한다는 ARM 의 서술과
맞는다.

---

# ⚠⚠ 정정 (2026-11-07 밤) — 이 문서의 SMPEN 서술은 **전부 틀렸다**

위쪽 본문과 충돌하면 **이 절이 우선한다.**

## 무엇이 틀렸나 — 레지스터를 잘못 읽었다

ARM 자신의 TF-A 헤더(출처:
`https://raw.githubusercontent.com/ARM-software/arm-trusted-firmware/master/include/lib/cpus/aarch64/cortex_a55.h`,
2026-11-07 원문 확인):

```c
#define CORTEX_A55_CPUECTLR_EL1   S3_0_C15_C1_4      /* ← 진짜 CPUECTLR */
#define CORTEX_A55_CPUECTLR_EL1_L1WSCTL  (ULL(3) << 25)   /* 유일하게 정의된 필드 */

#define CORTEX_A55_CPUACTLR_EL1   S3_0_C15_C1_0      /* ← 우리가 읽던 것 */
#define CORTEX_A55_CPUACTLR_EL1_DISABLE_WRITE_STREAMING  (ULL(1) << 24)
#define CORTEX_A55_CPUACTLR_EL1_DISABLE_DUAL_ISSUE       (ULL(1) << 31)
#define CORTEX_A55_CPUACTLR_EL1_DISABLE_L1_PAGEWALKS     (ULL(1) << 49)
```

`apps/llm_l8d/src/main.c:53,57,62`·`apps/llm_l8e`·`apps/llm_l8f`·`soc_init/smpen.S`
가 모두 **`S3_0_C15_C1_0`** 을 쓴다. 그것은 **`CPUACTLR_EL1`** 이다.

그리고 **A55 의 `CPUECTLR_EL1` 에는 SMPEN 비트가 없다.** A55 는 DynamIQ(DSU)
기반이라 코히런시를 DSU 가 관리한다. SMPEN(bit 6)은 Cortex-A53/A57/A72 의
`CPUECTLR_EL1` 필드다. A55 에 그 비트를 찾으러 간 것 자체가 잘못이었다.

(교차 확인: 메인라인 TF-A 의 `lib/cpus/aarch64/cortex_a55.S` 는 SMPEN 을
세우지 않는다. `CPUECTLR` 참조는 erratum 778703 의 `L1WSCTL` 한 곳뿐이다.
그런데도 Linux 는 이 SoC 에서 정상 부팅한다. "A55 에서 SMPEN 이 코히런시에
필수" 라는 전제가 애초에 성립하지 않는다.)

## 무너지는 결론들

| 주장 | 상태 |
|---|---|
| "주 코어 SMPEN=0" | **무효.** `CPUACTLR_EL1` 의 bit 6 을 읽었다. 그 비트는 ARM 이 공개 정의한 바 없다 |
| "SMPEN 0→1 이면 5~10×" | **무효.** 근거가 없다 |
| "L1≈DRAM(830 vs 708)이 SMPEN=0 의 직접 증거" | **무효.** 아래 참조 |
| "2.2× × 5~10× × 4× ≈ 44~88× 로 셈이 맞는다" | **무효.** 가운데 인자가 사라졌다 |
| 부팅 훅에서 쓰다가 보드가 죽은 일 | 설명이 바뀐다 — `CPUACTLR_EL1` 의 **정의되지 않은 비트에 쓴 것**이다. 죽는 게 당연하다 |
| 런타임 쓰기 트랩(`esr 0x8a000000`, EC 0x22) | `CPUECTLR` 가 아니라 `CPUACTLR` 에 대한 트랩이었다 |

## 대역폭 측정 자체가 의심스럽다

815 MHz 에서 L1 읽기 **830 MiB/s = 사이클당 약 1 바이트**다. A55 의 L1 히트로는
터무니없이 낮다(A55 는 L1 에서 사이클당 16 바이트 급이어야 한다). 즉 이 숫자는
캐시 상태가 아니라 **측정 루프 자체**를 재고 있을 가능성이 크다 — 바이트 단위
로드, 의존성 체인, 언롤 없음.

따라서 "L1 과 DRAM 이 1.17배뿐이니 캐시가 안 먹는다" 는 **측정이 그렇게 말할 자격이
없다.** 캐시가 켜졌는지는 `SCTLR_EL1`.C/M/I 와 PTE 속성으로 직접 확인해야 한다.

## 살아남는 사실

1. **클럭 815 MHz 는 실측이고, 이제 원인이 밝혀졌다.** 업스트림 U-Boot 가
   `APLL_HZ = 816 MHz` 로 **의도적으로** 둔다 —
   `arch/arm/include/asm/arch-rockchip/cru_rk3568.h` 의 `APLL_HZ`,
   `drivers/clk/rockchip/clk_rk3568.c` 의 `rk3568_clk_init()` 이 armclk 가
   `APLL_HZ` 와 다르면 `rk3568_armclk_set_clk(priv, APLL_HZ)` 를 부른다.
   데이터시트 최대는 1.8 GHz → **2.2× 가 실재하고 손이 닿는다.**
2. **전압을 같이 올려야 한다.** Linux OPP(`rk3568.dtsi`):
   408/600/816 MHz = 850 mV · 1104 = 900 · 1416 = 1025 · 1608 = 1100 ·
   1800 = 1150 mV. Zero 3E 의 CPU 전원은 별도 레귤레이터 `vdd_cpu`
   (compatible `rockchip,rk8600`, i2c0 주소 0x40) 다
   (`rk3566-radxa-zero-3.dtsi`). 주파수만 올리면 불안정하다.
3. **SMP 선례가 있다.** Zephyr 에 `roc_rk3568_pc//smp` 와 `rock_3b//smp` 가
   있고, 문서에 보조 코어 1~3 이 올라와 `thread_b` 가 cpu 1 에서 도는 출력이
   있다. 같은 A55·GICv3·PSCI 0.2 구성이다. 그 defconfig 는 우리 보드
   defconfig 와 `SMP`/`MP_MAX_NUM_CPUS`/`PM_CPU_OPS*` 말고는 같다
   (`ARMV8_A_NS=y`·`CACHE_MANAGEMENT=y`·`ARM_ARCH_TIMER=y`·
   `TICKLESS_KERNEL=y`·`CLOCK_CONTROL=y` 전부 우리도 켜 있다).
   ⚠ 한 가지 다르다 — 그 보드는 U-Boot 에서
   **`dcache flush; icache flush; dcache off; icache off; go 0x40000000`**
   으로 띄운다. 우리 `boot.cmd` 는 캐시를 끄지 않고 `go` 한다.

## PSCI 판정의 한계 (스스로 깎는다)

QEMU 에서 `SMP=y`+PSCI 는 죽고 `SMP=y`+PSCI 없음은 떴다. 그런데 그 **QEMU
빌드에는 `CONFIG_ARMV8_A_NS` 가 없었고**(`# CONFIG_ARMV8_A_NS is not set`)
실기 빌드에는 **있다**(`CONFIG_ARMV8_A_NS=y`, 보드 defconfig). 보안 상태가
다르면 SMC 가 BL31 에 닿는 경로가 달라진다. 따라서 **그 QEMU 결과를 실기로
그대로 옮길 수 없다.**

실기 증거는 여전히 두 변수가 섞여 있다:
`{SMP+PSCI 죽음}` 대 `{SMP 없음+PSCI 없음 정상}`.

→ 다음 QEMU 시험은 **`ARMV8_A_NS=y` 를 맞춰서** 돌린다.

## 다음 순서

1. QEMU 에 `ARMV8_A_NS=y` 를 맞추고 PSCI 유무를 다시 가린다 (왕복 0회)
2. 레지스터를 **제대로** 읽는 판을 만든다 — 진짜 `CPUECTLR_EL1`(S3_0_C15_C1_4),
   `SCTLR_EL1`(C/M/I), `CTR_EL0`, 그리고 **제대로 된 대역폭 루프**
   (64비트 로드·독립 스트림 여러 개·언롤). 지금 숫자는 신뢰할 수 없다
3. 그 결과를 보고 클럭(2.2×)으로 갈지 결정한다. 클럭은 전압과 함께다
