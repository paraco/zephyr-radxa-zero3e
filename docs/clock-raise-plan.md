# 클럭 816 → 1800 MHz (2.2×) — 작업 계획과 **확정/미확정** 구분

**상태: 조사 중. 보드에 아무것도 쓰지 않았다 (2026-11-08).**

⚠ 이 문서의 규칙: **원문에서 읽은 값과 추측을 절대 섞지 않는다.** 이 값들은
전압 레귤레이터와 PLL 에 쓰인다. 틀리면 SoC 가 과전압으로 망가지거나 보드가
부팅하지 않는다(복구는 카드 재작성). 불확실한 것을 확실한 것처럼 적는 것이
아무것도 안 적는 것보다 나쁘다.

## 왜 이 레버인가

사다리 L6c 의 토큰당 시간은 오늘 23,600 ms → **358 ms (65.9배)** 가 됐다
(UART 폭주 16.6× · 4스레드 3.38× · `-O3` 1.18×). 남은 확정 레버는 이것
하나이고 **2.2배**다. 되면 토큰당 약 **163 ms = 초당 6토큰**이다.

## ✅ 원문으로 확정한 것

출처: `u-boot/arch/arm/include/asm/arch-rockchip/cru_rk3568.h`
(2026-11-08 raw.githubusercontent.com 원문 확인)

```c
#define APLL_HZ            (816 * MHz)      /* U-Boot 가 armclk 를 여기 둔다 */
#define OSC_HZ             (24 * MHz)
#define RK3568_PLL_CON(x)  ((x) * 0x4)
#define RK3568_MODE_CON    0xc0

struct rk3568_pll { con0; con1; con2; con3; con4; reserved0[3]; };  /* 0x20 */
struct rk3568_cru {
    struct rk3568_pll pll[6];
    unsigned int mode_con00;      /* 0x00C0 */
    ...
    unsigned int clksel_con[85];  /* 0x0100 */
    ...
};
check_member(rk3568_cru, mode_con00, 0xc0);
```

→ **APLL = `pll[0]` = CRU + 0x00** (con0 +0x00 · con1 +0x04 · con2 +0x08 …)
→ **`clksel_con[n]` = CRU + 0x100 + 4n**

코어 클럭 먹싱 (같은 파일):

| 레지스터 | 필드 | shift | 뜻 |
|---|---|---|---|
| `clksel_con[0]` | `CLK_CORE_PRE_SEL` | 7 | 0 = SRC · 1 = APLL |
| `clksel_con[2]` | `SCLK_CORE_PRE_SEL` | 15 | 0 = SRC · 1 = NPLL |
| `clksel_con[2]` | `SCLK_CORE_SRC_SEL` | 8 (mask 3) | 0 = APLL · 1 = GPLL · 2 = NPLL |
| `clksel_con[2]` | `SCLK_CORE_SRC_DIV` | 0 (mask 0x1f) | |
| `clksel_con[3]` | `GICCLK_CORE_DIV` / `ATCLK_CORE_DIV` | 8 / 0 | |
| `clksel_con[4]` | `PERIPHCLK_CORE_PRE_DIV` / `PCLK_CORE_PRE_DIV` | 8 / 0 | |
| `clksel_con[5]` | `ACLK_CORE_NIU2BUS_SEL` | 14 | |

실측(L8i·L8m, PMU/타이머 교차 확인): **CPU 816 MHz** — 위 `APLL_HZ` 와 일치한다.

## ❌ 아직 **확정되지 않은** 것 (조사 중)

이것들이 확정되기 전에는 **한 바이트도 쓰지 않는다.**

1. **PLL 비트 필드** — con0/con1/con2 안의 FBDIV·POSTDIV1·REFDIV·POSTDIV2·
   DSMPD·PWRDOWN·FRAC·LOCK 위치와 폭, 그리고 상위 16비트 write-mask 방식
   (`u-boot/drivers/clk/rockchip/clk_pll.c`)
2. **1800 MHz 의 PLL 파라미터** — `rk3568_pll_rates` 표에 **1800 항목이
   있는지부터**. 표에 있는 값을 쓰는 것과 내가 계산한 값을 쓰는 것은 위험이
   다르다 (`clk_rk3568.c`)
3. **변경 순서** — `rk3568_armclk_set_clk()` 원문. 올릴 때와 내릴 때 순서가
   다르다고 들었다(미확인). 분주 먼저인지, slow mode 를 거치는지, lock 폴링을
   어떻게 하는지
4. **CRU 물리 베이스** — 두 출처 교차 확인 필요 (`rk3568.dtsi` 의 `cru:` 노드 ·
   TF-A `rk3568_def.h`). 2차 정보로 `0xfdd20000` 을 들었으나 **대조 안 됨**
5. **`vdd_cpu` 레귤레이터** — Radxa Zero 3E 의 그 노드 `compatible`·버스·주소·
   전압 범위, 그 드라이버의 **출력 전압 레지스터 번호와 인코딩**, 그리고
   `cpu0_opp_table` 의 1800 MHz `opp-microvolt`

## 전압이 선결 조건이다

2차 정보(Linux OPP, 미대조): 816 MHz = 850 mV · 1104 = 900 · 1416 = 1025 ·
1608 = 1100 · **1800 = 1150 mV**.

⚠ **주파수만 올리면 불안정하다.** 순서는 반드시 **전압 먼저, 클럭 나중**이다.
   그리고 전압을 **너무 높게** 쓰면 SoC 가 망가진다 — 인코딩을 원문으로
   확정하지 않으면 손대지 않는다.

## 경로 선택 — U-Boot 쪽이 유력하다

| | Zephyr 에서 | **U-Boot `boot.cmd` 에서** |
|---|---|---|
| I2C | **드라이버 없음** — RK3566 용을 새로 써야 한다 | `i2c dev` · `i2c mw` 가 **이미 있다** |
| CRU 쓰기 | `device_map` + `sys_write32` | `mw.l` |
| 적용 시점 | 런타임 | **Zephyr 진입 전** (더 안전 — 캐시·MMU 전) |
| 되돌리기 | 이미지 교체(OTA) | `boot.scr` 재작성 (**카드 작업**) |
| 관측 | UART 로 보인다 | ⚠ **1.5 Mbps 라 한 줄도 안 보인다** |

U-Boot 쪽이 작업량이 훨씬 적다. 대가는 **눈을 가리고 쓰는 것**이다. 완화책:
- `sdcard/boot.scr.known-good` 과 `boot.scr.pre-try` 가 이미 있다 — 되돌릴 판이 남아 있다
- Zephyr 가 뜨면 `moru info` 가 **실측 주파수**를 찍는다. 그것이 유일한 관측 수단이다
- 클럭만 먼저 올리고 전압은 나중에? **안 된다.** 순서가 반대면 불안정하다

## 다음 단계 (순서대로)

1. **[진행 중]** 위 ❌ 다섯 항목을 1차 출처에서 확정
2. 카드의 **원시 부트 영역 백업** — `boot.scr` 뿐 아니라 idbloader·U-Boot 까지
   바이트 단위로. 지금 없다
3. `boot.cmd` 에 **읽기만 하는 판**을 먼저 넣어 `i2c probe` 로 레귤레이터가
   그 주소에 있는지 확인 (⚠ 출력이 안 보이므로, 확인은 Zephyr 쪽에서 I2C 를
   읽어야 할 수도 있다 — 그러면 Zephyr I2C 가 필요해진다. 재검토할 지점)
4. 전압 올리기 → 전원 재투입 → `moru info` 로 보드가 사는지 확인
5. 클럭 올리기 → `moru info` 의 실측 주파수 확인
6. L6c 재측정 (12/12 일치 + 토큰당 시간)

⚠ 3번에서 모순이 하나 드러났다 — **U-Boot 는 쓰기가 쉽지만 읽기(확인)가
  불가능하고, Zephyr 는 읽기가 쉽지만 I2C 드라이버가 없다.** 이 모순을
  해결하지 못하면 "썼는데 맞게 썼는지 모르는" 상태로 전압을 올리게 된다.
  그것은 받아들일 수 없다. 조사 결과를 보고 다시 설계한다.

---

# 2026-11-08 조사 결과 — **목표를 1800 → 1608 MHz 로 바꾼다**

⚠ 아래 값들은 WebFetch 의 **요약 모델을 거쳤다.** 바이트 단위 보증이 없다.
  **쓰기 직전에 raw URL 을 직접 열어 한 번 더 대조한다.** 특히
  `RK3036_PLLCON1_*` 매크로와 OPP 1800/1608 항목.

## 계획을 바꾸는 사실 둘

### (1) **U-Boot 표에 1800 MHz 가 없다**

`u-boot/drivers/clk/rockchip/clk_rk3568.c` 의 `rk3568_pll_rates`
(열 순서 `rate, refdiv, fbdiv, postdiv1, postdiv2, dsmpd, frac`):

```c
RK3036_PLL_RATE(1608000000, 1,  67, 1, 1, 1, 0),
RK3036_PLL_RATE(1416000000, 1, 118, 2, 1, 1, 0),
RK3036_PLL_RATE(1104000000, 1,  92, 2, 1, 1, 0),
RK3036_PLL_RATE( 816000000, 1,  68, 2, 1, 1, 0),
```

**표의 최대는 1608 MHz 다.** 그리고 `rk3568_cpu_rates`(분주 표)의 최대는
**1416 MHz** 다 — 1608 의 분주값조차 표에 없다.

1800 의 PLL 파라미터는 **아무 출처에도 없다.** 조사자가 계산한
`(1, 75, 1, 1, 1, 0)` 은 **계산값이고 원문이 아니다.**

### (2) lock 폴링에 **타임아웃이 없다**

`rk3036_pll_set_rate` 는 lock 비트(con1 bit 10)가 설 때까지 `udelay(1)` 로
돈다. **빠져나올 길이 없다.** 계산한 VCO 가 허용 범위 밖이면 **영구 정지**다.
그리고 RK3568 의 VCO 허용 범위는 **TRM 으로 확인하지 못했다.**

## 그래서 1608 MHz 로 간다

| | 1800 MHz | **1608 MHz** |
|---|---|---|
| PLL 파라미터 출처 | **없음** (계산값) | **U-Boot 표 원문** |
| 필요 전압 (Linux OPP) | 1.15 V | **1.10 V** |
| 816 대비 | 2.206× | **1.971×** |
| VCO | 1800 (표의 어느 항목보다 높다) | 1608 (표에 있다) |

**차이는 12% 다.** 검증되지 않은 PLL 파라미터를 타임아웃 없는 lock 폴링에
넣어 얻을 12% 가 아니다. 1608 로 **1.97배**를 가져가고, 그게 자리잡은 뒤에
1800 을 따로 본다.

→ 토큰당 358 ms → 약 **182 ms = 초당 5.5토큰** 전망.

## ✅ 이제 출처가 둘 이상인 것

**CRU 베이스 = `0xfdd20000`** — 두 출처 일치:
- Linux `arch/arm64/boot/dts/rockchip/rk356x-base.dtsi`:
  `cru: clock-controller@fdd20000`, `reg = <0x0 0xfdd20000 0x0 0x1000>`
  (⚠ `rk3568.dtsi` 가 아니라 **`rk356x-base.dtsi`** 에 있다)
- TF-A `plat/rockchip/rk3568/rk3568_def.h`: `#define CRU_BASE 0xfdd20000`

→ **APLL con0 = 0xfdd20000** · con1 = 0xfdd20004 · con2 = 0xfdd20008
→ MODE_CON = 0xfdd200c0 (APLL 은 shift 0, mask 0x1)
→ clksel_con[n] = 0xfdd20100 + 4n

**PLL 비트 필드** (`clk_pll.c`, RK3568 은 `pll_rk3328` 타입 → RK3036 식):
```
con0:  FBDIV   [11:0]      POSTDIV1 [14:12]
con1:  REFDIV  [5:0]       POSTDIV2 [8:6]     DSMPD bit12    PWRDOWN bit13
con2:  FRAC    [23:0]
LOCK:  con1 bit 10   (APLL 의 PLL() 인자 lock_shift = 10)
MODE:  SLOW 0 · NORMAL 1 · DEEP 2
```

**쓰기 마스크 — `mw.l` 로 쓸 때 결정적이다** (`hardware.h`):
```c
#define rk_clrsetreg(addr, clr, set)  writel(((clr)|(set))<<16 | (set), addr)
#define rk_setreg(addr, set)          writel((set)<<16 | (set), addr)
#define rk_clrreg(addr, clr)          writel((clr)<<16, addr)
```
**상위 16비트가 write-enable 이다.** 즉 `mw.l` 로 필드를 바꿀 때
`(마스크<<16) | 값` 꼴로 써야 하고, **다른 비트는 건드려지지 않는다.**
이것이 `mw.l` 을 쓸 수 있게 해 주는 성질이다 — read-modify-write 가 필요 없다.

**올릴 때의 순서** (`rk3568_armclk_set_clk`, 원문):
1. `clksel_con[0]` CLK_CORE_PRE_SEL ← SRC
2. `clksel_con[2]` PRE_SEL←SRC · SRC_SEL←APLL · SRC_DIV 필드←1
3. `old < hz` 면 **분주를 먼저**: `clksel_con[3]`(GICCLK·ATCLK) ·
   `[4]`(PERIPHCLK·PCLK) · `[5]`(ACLK_CORE_NDFT)
4. **그다음** PLL 설정
(내릴 때는 PLL 먼저, 분주 나중)

**`vdd_cpu`** — 드라이버는 `rk808-regulator.c` 가 **아니라 `fan53555.c`** 다:
```
rk3566-radxa-zero-3.dtsi, &i2c0 안:
  vdd_cpu: regulator@40 {
      compatible = "rockchip,rk8600";   reg = <0x40>;
      fcs,suspend-voltage-selector = <1>;
      regulator-min-microvolt = <712500>;  regulator-max-microvolt = <1390000>;
      regulator-ramp-delay = <2300>;
  };
I2C0 = 0xfdd40000 (rk356x-base.dtsi · TF-A rk3568_def.h 둘 다)

fan53555.c, RK8600: vsel_min = 712500 · vsel_step = 12500 · 64단계
  VSEL0 = 0x00 · VSEL1 = 0x01 · CONTROL = 0x02
  bit7 = BUCK_EN · bit6 = MODE · bits[5:0] = sel
  → V(µV) = 712500 + 12500 × sel
```
계산: 850 mV → sel 11 (0x0B) · 1.10 V → sel 31 (0x1F) · 1.15 V → sel 35 (0x23)

⚠ **레지스터는 1.5 V 까지 표현한다** (sel 63). DT 의 1.39 V 상한은
  Linux 안에서만 작동하는 보호다. **직접 쓰면 그 보호가 없다.**

**Linux OPP** (`rk3568.dtsi`, `opp-table-0`): 408/600/816 = 850 mV ·
1104 = 900 · 1416 = 1025 · **1608 = 1100** · 1800 = 1150 · 1992 = 1150.
⚠ RK3566 전용 `rk3566.dtsi` 의 OPP 는 **대조하지 못했다.** 위는 RK3568 기준이다.

## ❌ 아직 위험한 미지수 하나 — **어느 VSEL 레지스터가 살아 있나**

`fcs,suspend-voltage-selector = <1>` 이므로 활성 전압 레지스터가 **VSEL0(0x00)
일 것으로 보이지만**, 조사자가 그 case 문 원문을 못 봤고 **실제로는 칩의
VSEL 핀 상태가 결정한다** — 소스로는 알 수 없고 회로도가 필요하다.

⚠ 이것이 틀리면: 전압이 **안 올라간 상태로** 클럭을 올린다 → 저전압 불안정.
  (과전압보다는 낫지만 조용한 실패다.)

**→ 추측하지 않는다. 읽어서 확인한다.**

---

# 설계 모순이 풀렸다 — U-Boot 의 `i2c read` 가 메모리에 쓴다

전 회차에 적은 모순("U-Boot 는 쓰기만 되고 확인이 안 되며, Zephyr 는 확인만
되고 쓸 수단이 없다")의 해법:

**U-Boot 의 `i2c read <chip> <addr> <len> <memaddr>` 는 읽은 값을 메모리에
넣는다.** 그러면 Zephyr 가 그 메모리를 읽어 **UART 로 찍는다.**
Zephyr I2C 드라이버가 필요 없다.

```
i2c dev 0
i2c read 0x40 0 3 0x51000000     ← VSEL0 · VSEL1 · CONTROL 을 메모리로
```

그리고 **CRU 는 메모리 맵이라 Zephyr 가 직접 읽는다** — 중계가 필요 없다.

주소 선택 `0x51000000`: sram0(0x40000000~0x50000000) 밖, OTA 슬롯
(0x50000000+8 MiB) 밖, 모델 창(0x58000000~) 밖. Zephyr 는 `device_map` 으로
임의 물리 주소를 비캐시로 매핑해 읽는다(비캐시라 U-Boot 가 쓴 값이 그대로 보인다).

## 1단계 — **읽기만 한다. 전기적으로 아무것도 바꾸지 않는다**

`boot.cmd` 에 더할 것:
```
# 블록이 돌았는지 알리는 표식 (i2c 명령이 없는 U-Boot 일 수도 있다)
mw.l 0x51000000 0x4d4f5255 1          # "MORU"
mw.l 0x51000008 0xdeadbeef 1          # i2c read 가 덮어쓸 자리
if i2c dev 0; then
    i2c read 0x40 0 3 0x51000008
fi
```
새 앱 `apps/llm_l9a` 가 찍는다:
- 표식이 "MORU" 인가 → boot.cmd 블록이 돌았나
- `0x51000008` 의 3바이트가 `0xdeadbeef` 그대로인가 → **`i2c` 명령이 없거나 실패**
- VSEL0 · VSEL1 · CONTROL 의 실제 값 → **어느 쪽이 살아 있고 지금 몇 V 인가**
- APLL con0/con1/con2 · MODE_CON · clksel_con[0..5] → **지금 클럭 구성**
- PMU 실측 주파수 (교차 검증)

⚠ 이 단계는 카드 작업이 **한 번** 필요하다(`boot.scr`). 그 왕복으로
  미지수 셋이 측정값으로 바뀐다 — 어느 VSEL, 지금 전압, 지금 PLL 구성.

## 그 뒤 순서 (1단계 결과를 보고 확정)

2. **카드의 원시 부트 영역 백업** (아직 없다)
3. 전압을 **1.10 V(sel 31)** 로 — 1608 용. 전원 재투입 → `moru info` 로 생존 확인
4. 클럭을 1608 로 — 분주 먼저, PLL 나중 (위 순서대로)
5. L9a 로 실측 주파수·전압 재확인
6. L6c 재측정 (12/12 일치 + 토큰당 시간)
