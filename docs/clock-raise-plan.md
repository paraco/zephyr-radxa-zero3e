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
