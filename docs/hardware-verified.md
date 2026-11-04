# Radxa Zero 3E — 실기 검증 기록

**2026-11-03.** 보드에 Radxa 공식 Debian 11 (bullseye) 이미지를 올려 부팅시킨 뒤,
USB gadget 으로 올라온 ADB 를 통해 라이브 디바이스트리와 sysfs 를 직접 읽었다.
여기 적힌 "✅" 값은 추측이 아니라 **동작하는 하드웨어에서 읽은 것**이다.

## 어떻게 읽었나 — 시리얼 없이

이 보드의 Linux 콘솔은 1,500,000 baud 인데 손에 있던 어댑터가 그 속도를 못 냈다
(아래 "시리얼 함정"). 그래서 시리얼을 포기하고 USB 로 들어갔다.

공식 이미지는 부팅하면서 복합 USB gadget 을 올린다 (`idVendor 0x1D6B` =
Linux Foundation, 제품명 `OTG Utils`, 벤더 문자열 `Radxa`):

| 인터페이스 | 클래스 / 서브클래스 | 정체 |
|---|---|---|
| 0 | `255` / `0x42` | **ADB** |
| 1 | `2` / `6` | CDC-ECM 이더넷 |
| 2 | `10` / `0` | ECM 데이터 |
| 3 | `17` / `0` | USB Billboard |

`brew install android-platform-tools` 뒤 `adb shell` 로 바로 쉘이 열렸다.
IP 설정도 SSH 도 랜선도 필요 없었다.

```bash
adb devices          # 0123456789ABCDEF  device
adb shell
```

**가젯이 보이는 것 자체가 부팅 성공의 증거다** — 커널이 죽었으면 가젯이 존재할 수 없다.
시리얼이 조용할 때 보드를 의심하기 전에 USB 장치 목록을 먼저 봐야 한다.

한계: `adb root` 는 거부된다 (`adbd cannot run as root in production builds`).
`uid=2000` 이라 `dmesg`·`/proc/iomem` 주소·`/dev/mem` 이 모두 막힌다.
**레지스터를 읽어야 하는 항목은 이 경로로 확인할 수 없다.**

## 검증된 값

### 시스템

| 항목 | 값 |
|---|---|
| 커널 | `5.10.160-26-rk356x` aarch64 SMP |
| 배포판 | Debian GNU/Linux 11 (bullseye) |
| 호스트명 | `radxa-zero3` |
| CPU | part `0xd05` = **Cortex-A55**, **4 코어** |
| L1 D-cache 라인 | **64 바이트** (`getconf LEVEL1_DCACHE_LINESIZE`) |
| DRAM | `reg = <0x0 0x00200000 0x0 0x7fe00000>` → **0x00200000 ~ 0x80000000, 2 GB** |
| 온칩 SRAM | `sram@fdcc0000` (Zephyr 적재용으론 작음) |

`CONFIG_DCACHE_LINE_SIZE=64` 가 맞다. `DCACHE_LINE_SIZE_DETECT=y` 는 여전히 쓸 수 없다
(`usb_buf.h:44` 가 `CONFIG_DCACHE_LINE_SIZE` 를 무조건 참조하는데 그 경우 미정의가 된다).

### USB 컨트롤러 — 우리가 겨냥한 그것

라이브 DT 경로: `/sys/firmware/devicetree/base/usbdrd/dwc3@fcc00000`
(Rockchip 관행대로 `usbdrd` 래퍼 노드 **안에 중첩**돼 있다. 래퍼는
`compatible = "rockchip,rk3568-dwc3", "rockchip,rk3399-dwc3"` 에 클럭 4개
(`ref_clk` `suspend_clk` `bus_clk` `pipe_clk`)를 묶은 것뿐이라 Zephyr 에는 필요 없다.)

| 속성 | 읽은 값 | 우리 DTS |
|---|---|---|
| `compatible` | `snps,dwc3` | 같음 ✅ |
| `reg` | `0x0 0xfcc00000 0x0 0x00400000` | 같음 ✅ |
| `interrupts` | `0x00 0xa9 0x04` = **SPI 169, level-high** | 같음 ✅ |
| `maximum-speed` | `high-speed` | 같음 ✅ |
| `dr_mode` | `otg` | (Zephyr 는 device 전용) |
| `phy_type` | `utmi_wide` (16비트 UTMI) | — |
| `phy-names` | `usb2-phy` → `usb2-phy@fe8a0000` | — |

**이 컨트롤러가 동작한다는 직접 증거:** `/sys/class/udc/fcc00000.dwc3/` 가
`state = configured`, `current_speed = high-speed`, `function = radxa-otgutils` 로
gadget 을 돌리고 있었고, **우리가 그 gadget 을 통해 보드와 대화하면서 이 값들을 읽었다.**

### SoC 의 USB 전체 배치

```
usbdrd/dwc3@fcc00000   snps,dwc3   dr_mode="otg"      ← device 가능한 유일한 것
usbdrd/dwc3@fd000000   snps,dwc3   (xhci-hcd 자식)     ← host 전용
usb@fd800000  fd840000  fd880000  fd8c0000             ← ehci/ohci, host 전용
usb2-phy@fe8a0000  usb2-phy@fe8b0000   rockchip,rk3568-usb2phy
nocp-vi-usb-peri-pipe@fe102c00
```

dwc2 는 이 SoC 에 **없다**.

### 라이브 DT 가 켜 둔 quirk 전체

```
quirk-skip-phy-init
snps,dis-del-phy-power-chg-quirk     snps,dis-tx-ipgap-linecheck-quirk
snps,dis-u1-entry-quirk              snps,dis-u2-entry-quirk
snps,dis-u2-freeclk-exists-quirk     snps,dis_enblslpm_quirk
snps,dis_rxdet_inp3_quirk            snps,dis_u2_susphy_quirk
snps,parkmode-disable-ss-quirk       snps,usb2-lpm-disable
snps,xhci-trb-ent-quirk
```

대부분 SuperSpeed · LPM · xHCI 전용이라 High Speed device 모드인 우리에게는 해당이 없다.
열거가 불안정하면 `dis_enblslpm` 과 `dis_rxdet_inp3` 를 먼저 검토한다.

표기 주의: Linux 는 `snps,dis_u2_susphy_quirk` (밑줄), 우리 Zephyr 바인딩은
`snps,dis-u2-susphy-quirk` (하이픈). 바인딩이 우리 것이므로 Zephyr 관행을 따른다.

## 시리얼 함정 — 기록해 둘 가치가 있음

**증상:** 보드에 전원을 넣어도 1,500,000 baud 에서 한 바이트도 오지 않는다.
보드·SD 카드·이미지·배선을 차례로 의심하며 시간을 버렸다.

**원인:** 어댑터 칩이 그 속도를 못 낸다. Silicon Labs **CP2102** (idProduct `0xEA60`,
bcdDevice `0x0100`) 의 데이터시트 최대 보율은 **1,000,000 bps** 다.
CP2102**N** 은 3 Mbps 지만 구형 CP2102 는 1.5 Mbps 가 구조적으로 불가능하다.

**함정이 깊은 이유:** macOS 의 `IOSSIOSPEED` ioctl 이 **성공한다.**
`tio -b 1500000` 이 `Connected` 를 찍고 조용히 앉아 있다. 설정이 받아들여진 것은
칩이 그 속도를 만든다는 증거가 아니다.

**가리는 방법:** 115200 으로 낮춰 본다.
- 쓰레기 바이트가 쏟아지면 → 보드는 송신 중, 어댑터가 속도를 못 내는 것
- 여전히 0 바이트면 → 보드가 송신하지 않거나 선이 안 닿은 것

이 한 번의 시험이 "보드가 죽었나"와 "어댑터가 못 따라가나"를 갈라 준다.
**Radxa wiki 자체가 경고한다** — *"please check if your USB to TTL cable support
1.5Mbps baudrate. Some model of CP210X and PL2303x have…"*

**배선** (핀 이름은 **보드 기준**이라 어댑터와 교차한다):

| 어댑터 | 보드 40핀 헤더 |
|---|---|
| RXD | **8** (보드 TX) |
| TXD | **10** (보드 RX) |
| GND | **6** |
| VCC | 연결하지 않음 (PD 로 급전) |

TX→8, RX→10 으로 꽂으면 TX 와 TX 가 맞붙어 영원히 0 바이트다.

**어댑터는 허브를 거치지 말고 Mac 에 직접 꽂는다.** 허브 2단
(`Root → USB2.1 Hub → USB2.0 Hub → CP2102`)을 거쳤을 때 1초 주기로
연결/끊김이 반복됐다.

## 아직 미확정 — 레지스터 읽기가 필요한 것

`adb` 가 root 를 못 줘서 확인하지 못했다. 모두 **Zephyr 가 처음 뜨는 순간
`udc_dwc3.c` 의 init 이 스스로 읽어 로그로 드러낸다.**

| 항목 | 어디서 확인되나 |
|---|---|
| `GSNPSID` 실제 값 | `udc_dwc3_init` 의 코어 ID 게이트 (`!= 0x5533` → `-ENODEV`) |
| 물리 엔드포인트 수 (DT 는 8 로 추측) | `GHWPARAMS3` 를 읽어 DT 값과 비교 |
| `DEPCMD` CMDSTATUS 성공 코드 | `dwc3_dep_cmd` 가 원시 값을 돌려준다 |
| DEVT `USB_RESET` 수신 | ISR 의 이벤트 디코드 |
| TRB self-test 결과 | init 말미 |
| U-Boot `go` 적재 주소 | 적재 절차 확정 시 |

## SD 카드 굽기 — 걸려 넘어진 것들

- **식별자가 매번 바뀐다.** 카드를 뺐다 꽂으면 `disk5` → `disk4` 가 된다.
  굽기 직전에 `diskutil list external physical` 로 다시 확인한다.
- **파이프 안의 `sudo` 는 비밀번호를 못 받는다.** `xz -dc … | sudo dd` 는
  `sudo` 의 stdin 이 파이프라 불안정하다. `sudo` 를 맨 앞으로:

  ```bash
  sudo sh -c 'diskutil unmountDisk /dev/diskN && xz -dc IMG.xz | dd of=/dev/rdiskN bs=4m'
  ```

- **`sudo` 자격 캐시는 터미널 탭마다 따로다** (`tty_tickets`). 다른 탭의
  `sudo -v` 는 도움이 안 된다. 한 명령으로 묶는 편이 낫다.
- **굽기 검증은 파티션 테이블로 한다.** `sudo` 없이도 증명된다 —
  공식 이미지를 구우면 GPT 로 `config`(16.8 MB) + `boot`(314.6 MB) + rootfs 가 나온다.
- 이미지 무결성: `xz -t`, 그리고 sector 64 에 `RKNS`(Rockchip idbloader),
  sector 16384 에 `d00dfeed`(U-Boot FIT).

## 연결 상태 보기

`tools/board-status.sh` 가 3초마다 갱신한다 — USB 가젯 · ADB · USB 이더넷 ·
유선 이더넷 · 시리얼을 한 화면에 보여 준다.

## dwc3 7a 단계 — EP0 설정 (2026-11-03 구현)

레지스터 인코딩을 추측하지 않기 위해 허용 라이선스 출처만 썼다. Zephyr 인트리에는
dwc3 가 없고(`drivers/usb/udc` 에 dwc2 만), Linux 드라이버는 GPL-2.0 이라 참조할 수
없다. 남은 것은 **Fuchsia** (BSD-2-Clause, `LICENSE.fuchsia` 로 출처 표기)였다.

처음에는 Fuchsia 의 `.cc` 드라이버 파일도 열지 않기로 제약을 걸었지만, 다시 따져
완화했다 — BSD 계열 라이선스는 Apache-2.0 프로젝트에 출처 표기만으로 포함 가능하고,
필요한 것은 **하드웨어 레지스터 인코딩 값**이라 창작적 표현이 아니다. 드라이버 로직은
베끼지 않고 상수만 확인했다.

### 이번에 확정한 값

| 값 | 내용 | 출처 |
|---|---|---|
| **DEPSTRTXFER 파라미터 순서** | `PAR0` ← TRB 주소 **상위**(63:32) · `PAR1` ← **하위**(31:0) | `dwc3-commands.cc` 의 databook 인용 주석 |
| **EP_TYPE 값** | USB 2.0 `bmAttributes[1:0]` 인코딩 (control 0 · isoc 1 · bulk 2 · int 3) | 같은 파일이 USB 타입 enum 을 변환 없이 넣음 |
| DEPXFERCFG | `PAR0` = 전송 리소스 개수 (EP0 은 1) | 〃 |
| DEPSTARTCFG | `COMMANDPARAM` = 리소스 ID 베이스 — EP0/EP1 은 **0**, EP2 이상은 **2** | 〃 |
| DEPCFG `FIFO_NUM` 규칙 | OUT 엔드포인트와 EP0-IN 은 0, 그 외는 `물리ep >> 1` | 〃 |
| DEPEVT 타입 | XferComplete 1 · XferInProgress 2 · XferNotReady 3 · StreamEvt 6 · CmdCmplt 7 | `dwc3-types.h` |
| GHWPARAMS3 | `NUM_EPS[17:12]` · `NUM_IN_EPS[22:18]` | `dwc3-regs.h` |
| 속도 값 | `DEVSPD_HIGH=0` · `CONNECTSPD_HIGH=0` (인코딩이 서로 다름) | 〃 |

**DEPSTRTXFER 의 순서가 가장 위험한 자리였다.** PAR 번호와 주소가 역순(`PAR0`이
`PAR1`보다 높은 오프셋)인 데다, 주소의 상·하위 배치도 번호 순서와 반대다. 추측했다면
반대로 넣었을 것이고, TRB 를 엉뚱한 물리주소에서 읽어 조용히 실패했을 것이다.

### 함께 발견한 필수 우회 처리

엔드포인트 명령을 내는 동안 `GUSB2PHYCFG` 의 `SUSPENDUSB20`·`ENBLSLPM` 이 켜져 있으면
USB 2.0 PHY 클럭이 게이팅돼 **명령이 완료되지 않을 수 있다.** 명령 전에 두 비트를 지우고
끝난 뒤 복원해야 한다 (Synopsys DWC_usb3 Programming Guide 3.2.2). `dwc3_dep_cmd()` 에
넣었다.

### 구현한 순서

```
udc_dwc3_enable()
  ├ 이벤트 버퍼 설정
  ├ DEVTEN  (USB_RESET · CONNECTION_DONE · DISCONNECT)
  ├ DCFG    (DEVSPD = HIGH, DEVADDR = 0)
  ├ IRQ 연결
  ├ dwc3_ep0_configure()
  │   ├ DEPSTARTCFG(base=0)
  │   ├ DEPCFG      ep0out, ep0in   (control, mps 64, XFER_CMPL_EN|XFER_NRDY_EN)
  │   ├ DEPXFERCFG  ep0out, ep0in   (리소스 1개)
  │   └ DALEPENA    = BIT(0)|BIT(1)
  ├ dwc3_ep0_arm_setup()            (SETUP TRB: 8바이트, IOC|LST → DEPSTRTXFER)
  └ DCTL.RUN_STOP = 1               ← 버스에 붙는 것은 맨 마지막
```

EP0 설정과 SETUP 무장이 `RUN_STOP` **보다 먼저**여야 한다. 순서가 거꾸로면 호스트의 첫
SETUP 을 놓친다. USB 리셋은 컨트롤러의 엔드포인트 상태를 날리므로, ISR 에서
`USB_RESET` 을 받을 때마다 둘을 다시 한다.

### 이 단계에서 기대하는 로그

응답(DATA/STATUS 단계)은 아직 구현하지 않았다. 따라서 호스트는 요청을 타임아웃으로
처리하고 몇 번 재시도한 뒤 포기한다. **같은 `GET_DESCRIPTOR` 가 반복해 보이는 것이
정상이고, 그것이 곧 "SETUP 경로가 살아 있다" 는 증거다.**

```
DEVT USB_RESET (type=1 info=0x0000)
EP0 configured (mps=64, DALEPENA=0x00000003)
EP0 SETUP armed: trb=0x... buf=0x... status=0x... rsrc=0x...
DEVT CONNECTION_DONE (type=2 ...)
connected: DSTS=0x... CONNECTSPD=0
DEPEVT ep=0 type=1 status=0x... params=0x...
SETUP 80 06 00 01 00 00 40 00 (residue=0 trbsts=0)
  bmRequestType=0x80 bRequest=0x06 wValue=0x0100 wIndex=0x0000 wLength=64
```

첫 실기 부팅에서 여기서 드러날 **미확인 값**: `CMDSTATUS` 성공 코드, `GSNPSID` 실제 값,
`GHWPARAMS3` 의 엔드포인트 개수(DT 의 8 과 대조), DEPSTRTXFER 의 전송 리소스 인덱스.

### 알고 있는 빚

- **ISR 에서 DEPCMD 를 5번 동기로 기다린다** (각 최대 5 ms → 최악 25 ms). 열거 로그를
  보려는 지금 단계에서는 받아들이지만 [8] 에서 워크큐로 옮긴다.
- `udc_dwc3_init()` 에도 이전 단계의 DEPSTARTCFG 확인 코드가 남아 있다.
- `CMDIOC` 는 켜지 않는다 — `CMDACT` 를 동기로 기다리므로 완료 이벤트가 중복이다.
  전송 리소스 인덱스는 명령 후 `DEPCMD.COMMANDPARAM` 에서 읽는다.

### 검토에서 고친 것 (2026-11-03)

독립 검토를 받아 5건을 고쳤다. 비트 위치·필드 폭·`DEPSTRTXFER` 파라미터 순서는
전부 일치한다는 확인을 받았고, 지적은 모두 캐시 배치·정리 누락·동시성·출처 표기였다.

**① TRB 링이 캐시 라인을 공유하고 있었다 (잠복 버그).**
`dwc3_ep0_trb[2][2]` 는 16 B × 4 = 64 B 라, OUT 행과 IN 행이 캐시 라인 하나를
같이 썼다. 이 SoC 의 DMA 는 캐시 코히런트하지 않고 Zephyr 의 arm64
`arch_dcache_range(K_CACHE_INVD)` 는 부분 라인에도 `dc ivac` 를 그대로 적용한다.
그러면 OUT TRB 를 invalidate 할 때 같은 라인의 IN 쪽 dirty 데이터가 버려지고,
IN TRB 를 flush 할 때 OUT TRB 의 낡은 `HWO=1` 이 하드웨어가 갱신한 값을 덮어쓴다.
지금은 OUT[0] 만 써서 터지지 않지만 **IN 경로가 들어오는 [7b] 에서 간헐적으로
깨질 자리였다.** 구조체에 `__aligned(CONFIG_DCACHE_LINE_SIZE)` 를 주어 원소마다
자기 라인을 단독 점유하게 했고, `BUILD_ASSERT` 로 고정했다.

**② `enable()` 의 오류 경로가 IRQ·DEVTEN 을 켠 채 남겼다.**
`udc_enable` 이 실패하면 보통 disable 이 불리지 않아 되돌릴 기회가 없다.
`irq_connect()` 를 EP0 무장 뒤(RUN_STOP 직전)로 옮기고 — ISR 이 EP0 명령을 내게
됐으므로 설정 중 인터럽트가 겹치지 않게 하는 효과도 있다 — 실패 시 DEVTEN 과
DALEPENA 를 되돌리게 했다. RUN_STOP 전에는 버스 이벤트가 없어 인터럽트를 놓치지
않는다.

**③ 락 정책 서술이 현실과 달랐다.**
`dwc3_dep_cmd` 주석은 "udc 락 안에서만" 이라고 했지만 ISR 호출은 락 없이 들어온다.
지금은 겹치지 않는다 — 스레드 쪽 EP0 접근은 `enable()` 뿐이고 그 시점에 IRQ 가
아직 연결되지 않았으며 RUN_STOP 전이다. **[8] 에서 `ep_enable`/`ep_enqueue` 가
udc 락 아래에서 같은 EP 에 DEPCMD 를 내기 시작하면 깨진다.** 주석에 선택지
(ISR→워크큐, 또는 스레드 구간에서 IRQ 차단)를 명시했다. 워크큐 쪽이 ISR 의 25 ms
busy wait 도 같이 없애므로 선호한다.

**④ 명령 status 가 기본 로그 레벨에서 안 보였다.**
`CMDSTATUS` 의 성공 코드를 모르니 코드가 성공/실패를 판정할 수 없다. 즉 하드웨어가
명령을 거절해도 함수는 0 을 돌려준다. 그런데 status 는 `LOG_DBG` 였고 "EP0
configured" 만 `LOG_INF` 로 찍혀, **비트가 틀려도 성공처럼 보였다.** status 들을
`LOG_INF` 로 올리고, 요약 줄을 "EP0 commands issued — 성공 판정은 위 status 확인
필요" 로 바꿨다. 값을 확인한 뒤 판정 로직을 넣고 레벨을 내린다.

**⑤ 출처 표기가 사실과 달랐다.**
헤더 머리말이 "Fuchsia 의 드라이버 소스(.cc)는 열지 않았다" 고 적어 둔 상태에서
`dwc3-commands.cc` 를 열었다. 작업 중간에 제약을 완화하면서 머리말을 고치지 않은
것이다. 머리말을 바로잡고, **무엇을 가져왔고(레지스터 오프셋·비트 위치·필드 값
인코딩 = 하드웨어 사실) 무엇을 가져오지 않았는지(드라이버 구조·상태 기계·전송
큐·오류 처리)** 를 명시했다. 완화한 이유도 적었다 — 레지스터 헤더에 없는 두 값이
`.cc` 에만 있었고 추측하면 조용히 실패하는 자리였다.

**⚠ 외부 공개 전 확인 사항:** `udc_dwc3.c` 의 SPDX 는 Apache-2.0 단독이다. 그 파일의
코드는 직접 썼고 명령 발행 순서는 databook 이 규정하는 하드웨어 요구사항이지만,
그 순서를 BSD-2 출처와 대조해 확인한 것은 사실이다. 배포 전에 라이선스 표기를 한 번
더 검토할 것.

덧붙여 쓰이지 않던 공용 `DWC3_SPEED_*` 를 제거했다 — `DCFG.DEVSPD` 와
`DSTS.CONNECTSPD` 는 인코딩이 달라(`LOW` 의 유무) 하나로 묶으면 틀린다.

### 실기에서 확인해야 할 열린 질문

코드만으로는 확정할 수 없어 첫 실기 로그로 가려야 하는 것들이다.

1. **USB_RESET 때 활성 EP0 전송에 `DEPENDXFER` 가 필요한가.** `enable()` 에서 무장한
   SETUP 전송이 아직 활성인 채로 리셋 처리가 `DEPSTARTCFG`→…→`DEPSTRTXFER` 를
   다시 낸다. 하드웨어가 이를 허용하는지 모른다. 전송 리소스 인덱스는 로그로만
   남기고 저장하지 않아 지금은 `DEPENDXFER` 를 낼 수도 없다.
   **첫 열거가 안 되면 가장 먼저 의심할 곳이다.**
2. **DATA/STATUS 응답 없이 SETUP 을 곧바로 재무장하는 것을 하드웨어가 받는가.**
   거절되더라도 `CMDSTATUS` 를 판정하지 않으므로 "armed" 로 찍힌다 (④ 와 연결).
3. **SETUP TRB 에 `ISP`(bit 10) 가 필요한가.** 지금은 `IOC|LST` 만 세운다. SETUP 8
   바이트가 TRB 크기와 같아 `IOC` 로 완료 이벤트가 올 것으로 보지만 확정하지 못했다.
4. **`DEPCFG`×2 → `DEPXFERCFG`×2 순서가 규정인가.** EP 별로 `CFG`·`XFERCFG` 를
   짝지어 내는 구현도 있다. 어느 쪽이 databook 규정인지 확인하지 못했다.
5. `dwc3_trb_reclaim` 이 `-EBUSY` 면 재무장 없이 반환해 SETUP 이 영구히 막힌다.
   원인(캐시·메모리 가시성)을 가리기 위한 의도된 동작이지만 복구 경로가 없다.

## dwc3 7b 단계 — DATA/STATUS 단계 (2026-11-03)

### 구조적 전환 — ISR 을 위/아래 절반으로 쪼갰다

`udc_setup_received()` 의 계약이 이것을 **요구**한다 — *"can only be called from
thread context because it depends on UDC lock"* (udc_common.h). ISR 에서 부를 수 없다.

그래서 ISR 은 세 가지만 한다: 처리할 이벤트가 있는지 보고, `GEVNTSIZ.EVNTINTRPTMASK`
로 이벤트 인터럽트를 **마스크**하고(`GEVNTCOUNT != 0` 이면 레벨 인터럽트가 계속
걸린다), `k_event_post` 로 스레드를 깨운다. 스레드가 `udc_lock_internal()` 을 잡고
링을 비운 뒤 마스크를 해제한다. **이벤트를 두 번 복사하지 않는다** — 링은 DMA
메모리에 그대로 두고 스레드가 직접 읽으므로 유실도 없고 별도 큐도 없다.

덤으로 7a 검토의 두 지적이 함께 해소됐다: ISR 에서 DEPCMD 를 25 ms 동기 대기하던
문제, 그리고 `dwc3_dep_cmd` 의 락 정책 불일치. 이제 이 함수는 **항상 스레드 문맥에서
UDC 락을 잡은 상태로만** 불린다.

인트리 `udc_dwc2.c` 의 모델(k_event + 전용 스레드 + `udc_lock_internal`)을 따랐다.
**라이선스 경계:** DWC3 레지스터 사실은 Fuchsia(BSD-2)뿐이지만, Zephyr 드라이버
구조는 인트리 `udc_dwc2.c` 가 Apache-2.0 이라 자유롭게 참조했다.

### 확정한 제어 전송 규칙

| 규칙 | 내용 |
|---|---|
| 2단계 (`wLength == 0`) | `TRBCTL_STATUS_2`, **EP0 IN** 에서 시작 |
| 3단계 | `TRBCTL_STATUS_3`, **데이터 단계의 반대 방향** (데이터 IN → 상태는 EP0 OUT) |
| STATUS 시작 시점 | 즉시가 아니라 **`XferNotReady(STATUS)`** 를 받은 뒤 |
| **OUT TRB 길이** | **MPS(64) 의 배수여야 한다** |
| IN ZLP | `보낸양 < wLength && 보낸양 % MPS == 0 && 보낸양 > 0` → 길이 0 `CONTROL_DATA` TRB |
| `TRBSTS == 2` | `SETUP_PENDING` — 새 SETUP 이 와 있다, 진행 중 전송 포기 |
| 제어 stall 해제 | SETUP 수신 시 하드웨어가 자동 해제 |

**OUT TRB 의 MPS 배수 규칙은 추측으로 절대 찾을 수 없다.** DWC3 는 짧은 패킷이
오거나 `TRB_BUFSIZ` 가 0 이 돼야 unchained OUT TRB 를 회수하므로, 배수가 아니면
제어 전송이 **영구히 회수되지 않는다.**

**ZLP 는 드라이버가 조건을 계산하지 않는다.** UDC 스택이 같은 조건을 이미 판정해
`bi->zlp` 로 알려 준다 (`usbd_endpoint.c` 의 `udc_ep_buf_set_zlp`). 드라이버는 플래그만
읽고 길이 0 TRB 를 거는 '방법' 만 담당한다 — 두 군데서 계산하면 판단이 어긋난다.
이것이 UDC 계층 분업의 전형이다: **스택이 "무엇을", 드라이버가 "어떻게".**

### 검토에서 고친 것

**① [치명] 스택의 SETUP 버퍼를 데이터 OUT 전송으로 걸고 있었다.**

스택은 제어 전송이 끝날 때마다 `bi->setup` 이 선 64바이트 버퍼를 EP0 OUT 큐에 넣어
둔다(`usbd_enqueue_setup`). 이것은 다음 SETUP 8바이트를 담을 **그릇**이고 전송 요청이
아니다 — `udc_setup_received()` 가 큐에서 이 버퍼를 만나면 **일부러 `break` 해서 큐에
남겨 둔다**(`udc_common.c`). `handle_xfer_next` 가 `bi->setup` 을 검사하지 않아
`CONTROL_DATA` 64바이트 OUT 전송으로 시작했고, 그러면:

- SETUP TRB 와 **같은 슬롯**(`DWC3_EP0_TRB(OUT,0)`)을 덮어쓴다
- 이미 전송이 진행 중인 EP 에 `DEPSTRTXFER` 를 또 낸다
- `ep0_state` 가 `DATA_OUT` 이 되어, 뒤이어 오는 SETUP 의 XferComplete 가 SETUP 이
  아니라 데이터 완료로 처리된다

**첫 제어 전송부터 열거가 실패한다.** 건너뛰게 고쳤다.

**② [높음] stall·시작 실패 뒤 상태 기계가 되돌아오지 않아 영구 정지했다.**

스택은 지원하지 않는 요청에서 `ctrl_xfer_stall` 로 가 `udc_ep_set_halt` 를 부르지만
드라이버 상태 기계는 건드리지 않는다. **호스트는 열거 중 지원하지 않는 요청을 흔히
보낸다.** 그때 상태가 `DATA_WAIT`/`TWO_STAGE` 로 남고 SETUP TRB 도 무장되지 않으면,
이후 SETUP 이 와도 데이터 완료로 오해해 멈춘다. `ep_set_halt`·시작 실패·`ep_dequeue`
세 경로 모두 상태를 `SETUP` 으로 되돌리고 TRB 를 재무장하게 했다.

**③ 상태/데이터 단계를 `ep0_state` 로 거르지 않았다.** 낡은 `ep0_nrdy_status` 때문에
데이터 단계 진행 중에 상태 TRB 가 걸릴 수 있었다 — 상태 단계는 데이터 단계의 반대
방향이라 `udc_ep_is_busy` 로 막히지 않는다.

**④ `UDC_EVT_RESET` 을 USB_RESET 에서 올리고 있었다.** 스택의 RESET 처리가 그 자리에서
`udc_device_speed()` 를 읽어 `status.speed` 를 확정하는데, `DSTS.CONNECTSPD` 는
**CONNECTION_DONE 뒤에야 유효**하다. 7a 에서는 속도가 고정값이라 드러나지 않았고,
`device_speed` 를 실제 레지스터 읽기로 바꾸면서 생긴 문제다. `CONNECTION_DONE` 으로
옮겼다 (인트리 dwc2 도 열거 완료 시점에 올린다).

**⑤ `caps.addr_before_status` 를 세우지 않았다.** 이 플래그가 없으면 스택은
**상태 단계가 끝난 뒤** `udc_set_address()` 를 부른다(`usbd_ch9.c` 의
`post_status_stage`). DWC3 는 SET_ADDRESS 를 받으면 DEVADDR 를 곧바로 쓰고 그 다음
상태 단계를 시작하는 절차이므로, 플래그를 세웠다. 드라이버 주석의 "상태 단계 전에
불린다" 는 서술이 **사실이 아니었던 것**도 함께 고쳤다.

**⑥ `enable()` 이 EP0 상태를 초기화하지 않았다.** 전송 중 disable 후 다시 enable 하면
낡은 상태로 시작해 첫 SETUP 이 데이터 완료로 오해된다.

**⑦ 계획 밖 변경을 되돌렸다.** `CONNECTION_DONE`/`DISCONNECT` 에서 올리던
`UDC_EVT_VBUS_READY`/`VBUS_REMOVED` 를 제거했다 — 이 컨트롤러는 VBUS 변화를 감지하지
않고(`caps.can_detect_vbus` 를 세우지 않았다) DISCONNECT 는 버스 분리이지 전원 변화가
아니다. 의미가 다른 이벤트를 올리면 스택 상태가 실제와 어긋난다.

검토가 확인해 준 것: 데드락·재귀 잠금 문제 없음(k_mutex 는 재귀적이고 우선순위를
상속한다), `EVNTINTRPTMASK` 가 영구 고착되는 경로 없음(해제는 `handle_events` 반환값과
무관하게 실행된다), 캐시 유지 방향·시점 정확, `net_buf` 누수·이중 제출 없음,
버퍼 정렬 안전(`USB_BUF_ALIGN == USB_BUF_GRANULARITY == CONFIG_DCACHE_LINE_SIZE`).

### 여전히 실기가 필요한 것

- 하드웨어가 `SETUP_PENDING` 을 어느 TRB 에 보고하는가
- `XferNotReady(DATA)` 전에 데이터 TRB 를 시작해도 되는가 (현재는 무시하는 설계 —
  Zephyr 모델에서는 스택이 버퍼를 비동기로 주므로 NRDY 가 먼저 오는 것이 정상이다)
- stall 을 걸 물리 EP 가 방향별로 맞는가
- 방향 불일치(호스트가 데이터 단계를 조기 종료하는 등)에 대한 검사가 없다
- `DEPENDXFER` 가 없어 활성 TRB 를 정확히 끝내지 못한다 — `rsrc_id` 보관과 함께 [8]
- `CONNECTSPD == 3` (48 MHz FS) 가 이 보드에서 나올 수 있는가

## dwc3 8단계 — 일반 엔드포인트 (2026-11-03)

bulk / interrupt 엔드포인트와, 7b 가 남긴 빚인 `rsrc_id` 보관 + `DEPENDXFER` 를
함께 넣었다.

### 확정한 사실 — 네 가지 모두 내 설계와 달랐다

| 항목 | 내가 가정한 것 | 사실 |
|---|---|---|
| `rsrc_id` 출처 | `DEPSTRTXFER` 뒤 `DEPCMD.COMMANDPARAM` 을 읽는다 | **`DEPEVT_CMD_CMPLT`(타입 7) 이벤트에서만** 온다. `CMD_TYPE[27:24] == DEPSTRTXFER(6)` 인 이벤트의 `RSRC_ID[22:16]`. 따라서 `DEPSTRTXFER` 에 **`CMDIOC=1`** 이 필요하다 |
| TX FIFO | `GTXFIFOSIZ` 를 할당해야 한다 | **할당하지 않는다.** 하드웨어 기본 배치를 쓴다 |
| OUT TRB 길이 | EP0 처럼 MPS 배수로 올린다 | **올리지 않는다.** 실제 길이를 걸고 짧은 패킷은 `TRB_SPR` 과 남은 바이트로 판별한다. MPS 규칙은 EP0 제어 전송 전용이었다 |
| 첫 전송 시점 | 버퍼가 오면 바로 건다 | **`XferNotReady` 를 한 번 받은 뒤에야** 첫 `DEPSTRTXFER` 를 낸다 (`got_not_ready`) |

7a 에서 "CMDIOC 는 켜지 않는다 — 완료 이벤트가 중복이다" 라고 판단하고 rsrc_id 를
`DEPCMD` 에서 읽어 로그만 남겨 뒀는데, **둘 다 틀렸다.** CMDIOC 는 일반
엔드포인트의 `DEPSTRTXFER` 에 반드시 필요하다.

덤으로 얻은 것:
- **`DALEPENA` 는 `DEPCMD.CMDACT` 가 서 있는 동안 지우면 안 된다**
- `DEPENDXFER` 에 `CMDIOC` 를 끄면 완료 이벤트가 오지 않으므로, `DALEPENA` 를
  지우거나 메모리를 놓기 전에 **약 1 ms 기다려** 진행 중 DMA 버스트가 끝나게 해야 한다
- IN ZLP 는 **별도 TRB** 로 건다. databook 의 `Normal-ZLP` 전송 타입은 실제로는
  동작하지 않는 것으로 알려져 있다
- `DEPSTARTCFG` 의 베이스 2 는 **`COMMANDPARAM` 으로** 전달한다 (파라미터 레지스터가 아니다)

### 설계

물리 엔드포인트마다 TRB 링 하나, 한 번에 전송 하나. TRB 체인·스캐터개더·'진행 중
전송'(`DEPUPDXFER`) 전략은 넣지 않았다 — Zephyr 의 net_buf 가 연속 메모리를 주고
`udc_ep_is_busy()` 가 큐를 직렬화하므로 TRB 하나로 충분하다. 성능이 필요해지면
LINK TRB 로 순환 링을 만들면 된다.

TRB 배열은 처음부터 **원소마다 캐시 라인을 단독 점유**하게 만들었다 (7a 검토가 EP0
에서 잡았던 문제를 설계에 박아 둔 것이다).

### 검토에서 고친 것

**① [치명] `DEPSTARTCFG` 의 베이스 2 가 전달되지 않았다.**
`dwc3_dep_cmd(..., DEPSTARTCFG, 0, 0, 0, &st)` 는 `COMMANDPARAM=0`, 즉 **base=0** 으로
나간다. 주석과 로그는 "base=2" 라고 말하고 있었다. 더 나쁜 것은, 이걸 위해
`dwc3_dep_cmd_ioc()` 를 만들어 놓고 **정작 필요한 그 자리에서 쓰지 않았다**는 점이다.
base=0 이면 EP0 의 전송 리소스를 다시 초기화해 EP0 이 깨지거나, EP2 이상의 리소스가
EP0/EP1 과 겹친다.

**② [확실] 낡은 `XferComplete` 가 새 버퍼를 가짜 완료시켰다.**
`xfer_complete` 가 `udc_ep_is_busy` 를 확인하지 않았다. `dequeue`/`disable` 이 busy 를
지운 직후 새 버퍼가 큐에 들어왔는데 이전 전송의 `XferComplete` 가 링에 남아 있으면,
TRB 의 HWO 가 이미 0 이라 `reclaim` 이 성공한다 — 시작도 안 한 버퍼를 이전 전송의
길이로 완료 처리하고 OUT 은 `net_buf_add` 까지 한다.

**③ [확실] `buf->size - buf->len` 은 남은 공간이 아니다.**
net_buf 는 앞쪽에 headroom 을 둘 수 있어, 그 식은 headroom 만큼 **버퍼 끝을 넘어선다.**
`net_buf_tailroom()` 은 `size - headroom - len` 이다. `net_buf_tail()` / `net_buf_tailroom()`
으로 바꿨다. 아울러 하드웨어가 TRB 가 덮는 크기보다 큰 패킷을 받으면 넘쳐 쓰므로
길이를 MPS 배수로 **내림**하고, 공간이 0 이면 전송을 시작하지 않는다 (주소 0·길이 0
의 OUT TRB 는 동작이 정의돼 있지 않다).

**④ [확실] 이벤트의 ep 필드에 범위 검사가 없었다.** 5비트(0~31)인데 배열은
`DWC3_NUM_PHYS_EPS`(16)개다 — 배열 밖 쓰기. `udc_get_ep_cfg()` 의 NULL 도 막지 않아
`XferNotReady` 경로에서 NULL 역참조가 가능했다.

**⑤ `rsrc_id` 경합 창.** `DEPSTRTXFER` 는 동기로 끝나지만 그 `CMD_CMPLT` 이벤트는
링에 쌓였다가 다음 스레드 실행 때 처리된다. 스레드가 락을 놓은 사이 스택이
`ep_disable`/`ep_dequeue`/`ep_set_halt` 를 부르면 `rsrc_valid == false` 라 `DEPENDXFER`
를 생략했고, **하드웨어 전송이 살아 있는데 호출자가 net_buf 를 풀에 반환**해 이후
DMA 가 반환된 버퍼에 썼다. 이제 그 자리에서 밀린 이벤트를 먼저 소화한다 (락을 쥐고
있고 ep 는 아직 busy 라 새 전송이 시작되지 않는다).

**⑥ `ep_disable` 이 실패를 숨겼다.** `CMDACT` 때문에 `DALEPENA` 를 못 지웠는데 0 을
돌려줘, 스택은 꺼진 줄 알지만 하드웨어에서는 활성인 채로 남았다. `-EIO` 를 돌려준다.
또 일반 엔드포인트가 모두 꺼지면 `started_cfg2` 를 되돌려, 호스트가 버스 리셋 없이
SET_CONFIGURATION 을 다시 보낼 때 `DEPSTARTCFG` 가 다시 나가게 했다.

**⑦** `INTERVAL` 은 interrupt 엔드포인트에만 넣는다 (bulk 에는 의미가 없다).
`DEPSTRTXFER` 실패 시 다음 버퍼로 넘어가 큐가 멈추지 않게 했다. 낡은 주석 세 곳을
정리했다.

검토가 확인해 준 것: 비트·필드 대조 전부 일치, 물리 번호 변환식과 역변환 정확,
`net_buf` 짝 일관·이중 제출/누수 없음, ISO 는 `caps.iso = 0` 으로 `ep_check_config`
가 거부하므로 충분, 캐시 유지 방향·시점 정확.

### 남은 의문 (실기 또는 databook 필요)

- **`rsrc_id` 가 `DEPCMD[22:16]` 에서도 읽히는가.** Fuchsia 는 이벤트만 쓰지만 Linux 는
  `DEPSTRTXFER` 직후 `DEPCMD` 도 읽는다고 한다. 읽을 수 있다면 ⑤ 의 경합 창 자체가
  사라진다. databook 확인 사항
- **일반 엔드포인트 OUT 길이의 MPS 배수 요건.** 지금은 안전하게 내림하지만, 실제
  요건이 무엇인지는 확인하지 못했다
- `canceling` 과 `want_event=true` 경로는 아직 호출처가 없다 (DEPENDXFER 를 항상
  이벤트 없이 낸다). 켜게 되면 `xfer_next` 가 막히지 않는지 함께 봐야 한다
- IN 의 길이 0 TRB 가 주소 0 을 쓴다. 실기에서 문제가 되면 유효한 정렬 버퍼 주소를 준다

## 실기 기동 성공 — 그리고 USB 가 막힌 지점 (2026-11-03 ~ 11-04)

### Zephyr 가 실기에서 돌았다

LED 진단으로 확인했다. **2초 켜짐 / 2초 꺼짐** 패턴이 나오고 동시에 Linux 가
뜨지 않았다 (`OTG Utils` 없음, ADB 없음). 두 증거가 서로를 보강한다.

이것으로 **한 번에 일곱 가지가 증명됐다:**

| | |
|---|---|
| `boot.scr` → `fatload` → `go` | U-Boot 적재 경로 전체 동작 |
| `go` 로 Zephyr 기동 | **캐시·MMU 우려는 기우였다** — 이 U-Boot 에 `dcache`/`icache` 명령이 없어 걱정했던 부분 |
| 적재 `0x40000000` / 진입 `0x40001084` | 맞음 |
| 보드 DTS | SoC · GIC · 타이머 · 메모리 동작 |
| `device_map()` | MMU 장치 매핑 동작 |
| GPIO 레지스터 배치 | FreeBSD(BSD-2) 출처가 정확 |
| `k_msleep` | ARM arch 타이머와 스케줄러 동작 |

### LED 진단이 필요했던 이유, 그리고 한 번 오판한 것

이 보드에서 쓸 수 있는 출력 채널이 둘뿐이다 — UART2 @ 1,500,000 baud(손에 있는
CP2102 가 최대 1 Mbps 라 못 읽는다)와 USB OTG(우리가 만들고 있는 dwc3 자신). 그래서
dwc3 가 조용할 때 **"Zephyr 가 안 뜬 것"과 "뜨지만 dwc3 가 실패한 것"을 구분할 수
없었다.** LED 가 그 둘과 무관한 제3의 채널이다.

**⚠ 처음에 '빠르게 5번 + 쉼' 패턴을 썼다가 오판했다.** 이 보드의 Linux 는 같은 LED 에
`heartbeat` 트리거(빠른 두 번 + 쉼)를 걸어 두기 때문에 빠른 패턴끼리는 눈으로
구분되지 않는다. 실제로 `boot.scr` 가 빠져 Linux 가 뜬 상태를 "Zephyr 성공"으로
읽을 뻔했다. ADB 확인으로 잡았다.

교훈 둘:
- **패턴은 Linux 가 내지 않는 모양으로** — 2초 네모파는 어떤 트리거도 내지 않는다
- **판정은 눈이 아니라 ADB 로** — `OTG Utils`/ADB 유무가 Linux 인지 아닌지를 확정한다

### GPIO 레지스터 — FreeBSD 에서 얻었다

Zephyr 에 Rockchip GPIO 드라이버가 없어(인트리 `rk3568.dtsi` 에 `gic` 와 `uart2` 뿐)
레지스터를 직접 썼다. 배치는 추측하지 않고 **FreeBSD `sys/arm64/rockchip/rk_gpio.c`**
(SPDX: BSD-2-Clause)에서 확인했다 — `RK_GPIO_TYPE_V2`:

```
SWPORTA_DR  = 0x00   (핀 0~15, 핀 16~31 은 +4)
SWPORTA_DDR = 0x08   (같은 방식)
VER_ID      = 0x78   (V2 == 0x01000c2b)
```

rk3566/rk3568 의 GPIO 는 **상위 16비트가 write-enable** 인 형식이다. 비트 n 을 1 로
만들려면 `(1 << n) | (1 << (n + 16))`, 0 으로 만들려면 `(1 << (n + 16))` 만 쓴다.
read-modify-write 가 필요 없고 **다른 핀을 건드리지 않는다** — 진단용으로 중요한 성질.

대상 핀은 보드의 라이브 DT 를 ADB 로 읽어 확인했다:
`/gpio-leds/board-led/gpios = <0x37 0x00 0x00>`, phandle `0x37` = `/pinctrl/gpio0@fdd60000`
→ **gpio0 @ 0xfdd60000, 핀 0, ACTIVE_HIGH**.

### USB 가 막힌 지점 — 빠진 초기화 둘

**① `GCTL.PRTCAPDIR` 를 설정하지 않았다.**
이 필드가 코어를 호스트로 쓸지 디바이스로 쓸지 정한다 (`HOST=1`/`DEVICE=2`/`OTG=3`).
3단계에서 `init` 주석에 *"포트 방향(GCTL.PRTCAPDIR)은 [4] 이후로 미룬다"* 라고 적어
두고 **끝내 넣지 않았다.** 속도(`DCFG.DEVSPD`)는 7a 에서 챙겼는데 방향만 빠졌다.
설정하지 않으면 호스트가 열거를 시도해도 응답하지 않아 **버스에 아예 나타나지 않는다.**

**② Rockchip USB2 PHY 는 별개의 장치다.**
`fe8a0000.usb2-phy` (`rockchip,rk3568-usb2phy`)에 `otg-port`/`host-port` 서브노드와
`rockchip,usbgrf`(= `syscon@fdca0000`)가 있다. GRF 레지스터로 전원·모드를 제어하며,
Zephyr 에는 이 드라이버가 없고 우리 DT 는 PHY 를 참조조차 하지 않는다. PLL 이 suspend
상태면 dwc3 를 device 로 돌려놔도 버스를 구동하지 못한다.

PLL 깨우는 값은 **FreeBSD `rk_usb2phy.c`** (BSD-2-Clause)의 `rk3568_regs.clk_ctl`:

```
offset 0x0008,  enable 0x00100000,  disable 0x00100010
```

GPIO 와 같은 hiword-mask 형식 — 비트 20(write-enable)을 세우고 비트 4(PLL suspend)를
0 으로 쓴다. 임시로 `udc_dwc3_init` 에 넣었다.

**⚠ 이것으로 충분한지는 모른다.** FreeBSD 드라이버는 `"Only host is supported right now"`
라고 명시하고 있어 **OTG 포트 전용 비트**(포트 suspend 해제, vbus/id 감지)는 그 출처에
없다. 여전히 열거가 안 되면 RK3566/RK3568 TRM 의 USB2PHY GRF otg-port 레지스터를
찾아야 한다.

### U-Boot 에 대해 알아낸 것

이미지의 sector 16384 FIT 를 추출해 문자열로 확인했다:

```
bootcmd=bootflow scan; run distro_bootcmd; ...
preboot=usb start; pci enum          ← 매 부팅마다 USB 를 '호스트 모드로' 초기화한다
boot_targets=mmc1 mmc0 nvme scsi usb pxe dhcp spi
boot.scr  boot.scr.uimg  bootmeth_script  extlinux/extlinux.conf
fatload  ext4load  bootm  booti  source  usb
"start application at address 'addr'"   ← go 가 있다
```

`dcache`/`icache` 명령은 **없다** (`CONFIG_CMD_CACHE` 미포함). 그래서 캐시를 끄고
점프할 수 없었는데, 실기에서 문제가 되지 않았다.

**`preboot` 가 `usb start` 를 돌린다**는 점이 중요하다 — PHY 에 전원과 클럭은 들어가
있지만 **호스트 모드**로 설정된다. 그래서 device 모드로 쓰려면 `PRTCAPDIR` 와 PHY 쪽을
모두 되돌려야 한다.

### macOS 쪽 함정

- **macOS 26 은 FAT 파티션을 마운트하지 못한다.** 구형 `msdosfs.kext` 가 코드 서명
  문제로 로드되지 않고(`Bad code signature`) `diskutil mount` 도 거부한다.
  **`mtools` 로 마운트 없이** 읽고 쓴다 (`mdir`/`mcopy`/`mdel` + `/dev/rdiskNsM`).
- **내장 SD 슬롯은 `internal` 로 보고된다.** "external 인지"로 안전을 가리는 방법이
  통하지 않는다. 크기와 파티션 이름(`config` + `boot`)으로 확인한다.
- 디스크 번호는 꽂을 때마다 바뀐다. 매번 다시 확인한다.

## EP0 SETUP 수신 — 미해결 (2026-11-04)

### 증상

컨트롤러는 "정상 완료" 를 보고하는데 메모리에는 아무 일도 일어나지 않았다.

```
depevt=0x0000c040
trb0 ctl=0x00000823 st=0x00000008
trb1 ctl=0x00000000 st=0x00000000
buf =00 00 00 00 00 00 00 00
```

`depevt` 해독 (Linux `struct dwc3_event_depevt` 배치):

| 필드 | 비트 | 값 | 뜻 |
|---|---|---|---|
| one_bit | 0 | 0 | 엔드포인트 이벤트 |
| endpoint_number | 5:1 | 0 | 물리 EP0 (OUT) |
| endpoint_event | 9:6 | 1 | **XferComplete** |
| status | 15:12 | 0xC | **LST=1 IOC=1 BUSERR=0 SHORT=0** |

즉 "IOC·LST 를 세운 그 TRB 가 오류 없이 완료됐다". 그런데 우리가 읽는
TRB 는 무장한 그대로다 — `ctl=0x823` (HWO=1, TRBCTL=2 SETUP, IOC, LST),
`st=0x8` (BUFSIZ 8 이 줄지 않음), 버퍼 전부 0.

그래서 `dwc3_ep0_setup_received` 가 조기 반환하고 SETUP 이 스택에
전달되지 않는다. 호스트는 데이터 단계에서 `XferNotReady(DATA)` 를
반복하다 포기하고 버스 리셋을 되풀이한다. **반복되는 `set_address = 0`
과 리셋 고리는 이 문제의 하류 증상이지 별개 문제가 아니다.**

### 배제한 가설 (실기 데이터로)

| 가설 | 근거 |
|---|---|
| TRB 슬롯 오독 | `trb1` 이 `ctl=0 st=0` — 쓰인 적 없음. EP0 링은 TRB 2개(`DWC3_EP0_TRB_COUNT`)지만 슬롯 0 만 쓰인다 |
| 캐시 미무효화 | `CONFIG_CACHE_MANAGEMENT=y`, `DCACHE=y`, 라인 64 B. `dwc3_trb_reclaim` 이 읽기 전 매번 `invd` |
| TRB 채우기 순서 | ptr/status 쓰고 flush, control(HWO) 쓰고 다시 flush. `BUILD_ASSERT` 로 캐시 라인 공유도 막음 |
| `DEPSTRTXFER` PAR 순서 | PAR0=상위 32비트, PAR1=하위 32비트. Fuchsia `dwc3-commands.cc` (BSD-2) 와 일치 |
| write-back 경합 | 200 µs 폴링(10 µs 간격, 매번 `invd`)에도 HWO 가 내려가지 않음 |

### 다음에 확인할 것

**컨트롤러에 넘긴 물리 주소와 우리가 읽는 주소가 같은가.** 지금까지
가상 포인터(`%p` = `0x4002b440`)만 봤고 `k_mem_phys_addr(trb)` 의
반환값을 확인한 적이 없다. `dwc3_ep0_start` 에 `trb_phys` 를 찍는
`LOG_DBG` 가 들어가 있다.

다르면 그것이 답이다 — DMA 는 다른 물리 주소에 쓰고 우리는 가상
주소를 읽는 것이다. 같으면 이웃 링(`ep1ring[0]`) 덤프를 본다.

### 임시로 남겨 둔 진단 코드

걷어낼 대상:

- `dwc3_ep0_setup_received` 의 HWO 경로 — TRB/버퍼/이웃 링 덤프
- `dwc3_trb_reclaim` 폴링 루프 (`DWC3_TRB_WB_TIMEOUT_US`)
- `dwc3_ep0_setup_received(dev, raw)` 의 `raw` 인자 (덤프용)

### 진단 설정에 대한 교훈

| 설정 | 결과 |
|---|---|
| `CONFIG_STACK_SENTINEL` + `CONFIG_THREAD_NAME` | ✅ **필수.** 스택 넘침을 "쓰레기 주소 점프" 가 아니라 `FATAL ERROR 2: Stack overflow` + 스레드 이름으로 보여 준다. 이 둘이 없어 두 번 헤맸다 |
| `CONFIG_FRAME_POINTER` + `CONFIG_EXCEPTION_STACK_TRACE` | ❌ **쓸 수 없다.** 켜면 Zephyr 가 기동조차 못 한다 (LED·콘솔 모두 무음). 역추적을 쫓느라 돌아간 것이 낭비였다 |
| `CONFIG_LOG_MODE_IMMEDIATE` | 유용하지만 호출자 스택을 많이 쓴다. arm64 에서 1024 기본 스택은 거의 전부 부족하다 |

**진단 도구가 진단 대상을 망가뜨린 사례도 있었다** — LED 생존 신호
스레드를 1024 바이트로 선언해 그 스레드가 크래시의 원인이 됐다.
드라이버 스택을 4배로 늘려도 증상이 그대로였던 이유가 이것이다
(엉뚱한 스레드를 늘렸다).

## 재무장 시점 — 두 수정이 서로를 무효화한다 (2026-11-04, 전체 로그 분석)

`tio -L` 로 **전체 부팅 로그**를 받아 분석하자 창 단위로 읽을 때 보이지
않던 것이 드러났다. 부팅 하나를 배너로 잘라 센 결과:

| | 미뤄진 재무장 | 즉시 재무장 |
|---|---|---|
| `USB reset` | 36 | 36 |
| `EP0 commands issued`(configure) | 37 | 36 |
| `ep0 cmd 6`(DEPSTRTXFER = 무장) | **22** | **36** |
| `SETUP` 수신 | **6** (전부 `00 05`) | **0** |
| `TRB still HW-owned` | 2 | 8 |
| `device address = 1` | 1 | 0 |
| `<err>` | 0 | 0 |

### 두 실패 양상

**미뤄진 재무장** (`handle_events` 끝에서 한 번):
`dwc3_ep0_configure()` 의 DEPSTARTCFG 가 전송 리소스를 초기화하므로 그
시점에 기존 TRB 는 무효다. 한 배치에 리셋이 2~3개 오면 configure 는 매번
실행되는데 무장은 배치 끝에 한 번뿐 → **그 배치 내내 TRB 가 없다.**
로그에 리셋 3연속 후 358 ms 공백이 찍혔다.

**즉시 재무장** (configure 직후):
무장 누락은 사라졌지만(36/36) **SETUP 수신이 0 이 됐다.** 재무장이 같은
배치의 SETUP XferComplete 를 처리하기 '전' 에 일어나, `memset` 과
`dwc3_trb_fill`(HWO=1)이 받아 둔 SETUP 을 매번 파괴한다.

### 그래서 알게 된 것

미뤄진 재무장에서 6개가 통과한 이유는 **순서** 때문이다 — 배치 안의
XferComplete 를 먼저 읽고 나서 덮어썼다. 즉 올바른 해법은
**"즉시 무장하되 이미 받아 둔 SETUP 은 보존"** 이다.

### 아직 모르는 것 (다음 측정)

**리셋 핸들러 진입 시점의 TRB 상태.** HWO 가 0 이면 그 자리에 읽지 않은
SETUP 이 있다는 뜻이고, 그러면 덮어쓰기 전에 처리하거나 보존해야 한다.
HWO 가 1 이면 받은 것이 없으니 그냥 재무장해도 된다. 이 한 줄을 찍으면
설계가 결정된다.

### 부수 관찰

- `XferNotReady(DATA)` 가 51번 — 호스트는 계속 데이터 단계로 진행한다.
  즉 컨트롤러는 SETUP 을 ACK 하고 있다 (제어 엔드포인트는 규격상 SETUP 을
  항상 ACK 한다). 우리 버퍼까지 오지 않는 것이 문제다.
- `CONNECTSPD` 가 0(HS)과 1(FS) 사이를 오간다 (41 대 31). DCFG.DEVSPD 는
  HS 로 고정해 두었다. 열거 실패에 따른 호스트의 속도 낮춤 재시도로 보이나,
  SETUP 수신이 풀린 뒤에도 FS 로 남으면 따로 봐야 한다.

### 방법론 교훈

**창 단위로 로그를 읽는 것이 세 번 잘못된 방향을 만들었다.**
`TRB still HW-owned` 을 핵심 증상으로 보고 여러 번 추적했으나 전체
로그에서는 36번 중 2번이었다. 빈도를 확인하지 않은 채 한 줄에 매달렸다.
`tio -L --log-file` 로 전체를 받아 집계하는 것이 비교할 수 없이 빠르다.

## SETUP 수신 해결 — 리셋이 받아 둔 SETUP 을 지우고 있었다 (2026-11-04)

### 측정

리셋 핸들러 **진입 시점**(덮어쓰기 직전)의 TRB 상태를 36번 집계:

```
32번  ctl=0x00000823 st=0x00000008 HWO=1 state=0 buf=00 00 00 00   받은 것 없음
 4번  ctl=0x00000822 st=0x00000000 HWO=0 state=0 buf=00 05 01 00   SETUP 이 있다
```

`st=0` 은 BUFSIZ 가 8→0, 즉 **8바이트 전부 전송됨**. `ctl` 의 HWO 가
내려갔고 버퍼에 유효한 `SET_ADDRESS(1)` 가 담겨 있다. 그런데
`dwc3_ep0_configure()`+`dwc3_ep0_arm_setup()` 의 `memset` 이 이 4번을
매번 파괴했다 — 그래서 `setup_received` 가 0 이었다.

**왜 리셋 이벤트가 먼저 오는가:** SETUP 은 리셋보다 먼저 도착했지만
컨트롤러가 그 완료 이벤트를 리셋 이벤트 **뒤에** 올린다. 링 순서는 바꿀
수 없으므로 리셋 핸들러에서 거둬들여야 한다.

### 고친 뒤

```
리셋 전에 받아 둔 SETUP 처리   5
setup_received: SETUP          5   (이전 0)
device address = 1             5   (이전 0)
```

### 틀렸던 전제 — SET_ADDRESS 가 먼저 오는 것은 정상이다

"GET_DESCRIPTOR(`80 06`) 가 먼저 와야 한다" 고 보고 그것이 없는 것을
이상 신호로 해석했다. **xHCI 호스트(macOS)는 Address Device 명령으로
`SET_ADDRESS` 를 먼저 보내고 그 뒤에 `GET_DESCRIPTOR` 를 한다.**
`00 05` 가 첫 요청인 것은 정상이며, 이 전제 착오가 "U-Boot 유령 TRB" 라는
잘못된 가설로 이어졌다 (CRU 클럭 작업까지 검토했으나 불필요했다).

### 다음 벽 — 상태 단계가 완료되지 않는다

```
STATUS done            0
XferNotReady(DATA)    54
XferNotReady(STATUS)   0   ← 한 번도 없다
device address = 0    36   ← 리셋마다 되돌아간다
```

로그 순서:

```
22.838  device address = 1 (DCFG=0x00080808)   SET_ADDRESS 적용
22.856  Queue ep 0x80 len 0                    상태 단계 버퍼 내려옴
22.998  STATUS  state=0                        보류 — state 가 SETUP 으로 되돌아감
```

`SET_ADDRESS` 는 `wLength=0` 인 2단계 전송이라 다음은 STATUS 단계인데
`XferNotReady(STATUS)` 가 한 번도 오지 않는다. 그리고 상태 단계 버퍼가
`dwc3_handle_xfer_next` 에서 `state != STATUS && state != TWO_STAGE` 로
보류된다 — 같은 이벤트 배치의 다음 리셋이 state 를 SETUP 으로 되돌리기
때문으로 보인다.

### 측정할 것

**`XferNotReady` 의 원시 이벤트 워드.** 상태 필드는 비트 15:12 이고
Linux 는 `1`(CONTROL_DATA), `2`(CONTROL_STATUS), `9`(DATA|INVALTRB),
`0xa`(STATUS|INVALTRB) 를 구분한다. 우리 헤더는 `XNR_STAGE` 를
`GENMASK(13,12)` 로만 읽고 `XNR_REASON`(비트 15 = INVALTRB)은 정의만
해 두고 **쓰지 않는다**. INVALTRB 가 서 있으면 "호스트가 요청한 단계에
TRB 가 없다" 는 뜻이고 지금 증상에 직결된다.

## 링크가 HS/FS 사이를 요동친다 — PHY OTG 포트 설정이 남았다 (2026-11-04)

### 로그에 이미 있던 증거 (DSTS 해독)

`DSTS` 의 `USBLNKST`(비트 21:18)와 `CONNECTSPD`(비트 2:0)를 집계:

```
USBLNKST=0 (ON)             31회
USBLNKST=5 (EARLY_SUSPEND)   5회
CONNECTSPD=0 (High Speed)   23회
CONNECTSPD=1 (Full Speed)   13회
```

**리셋마다 HS 와 FS 사이를 왕복한다.** `DCFG.DEVSPD` 는 HS 로 고정해
두었으므로, HS chirp 핸드셰이크가 불안정해 FS 로 떨어지는 것이다.

그리고 결정적 관찰:

```
00.517  USB reset
00.633  CONNECTION_DONE
00.714  device address = 0     깨끗한 리셋, EP0 무장 완료
03.581  USB reset              3초간 완전한 침묵, 그리고 다시 리셋
```

**깨끗한 리셋 뒤 호스트가 3초간 아무것도 보내지 않는다.** 지금까지 받은
모든 SETUP 은 리셋 '전' 에 도착한 것이고, 정상적인 리셋 후 창에서 받은
것은 한 번도 없다.

### 왜 PHY 인가

같은 보드·같은 케이블에서 **Linux 는 안정적인 HS 가젯**으로 동작했다
(`OTG Utils` 등장, ADB `0123456789ABCDEF` 연결). 즉 하드웨어 경로는
정상이다. 차이는 PHY 설정이다 — Linux 의 `phy-rockchip-inno-usb2` 가
OTG 포트를 device 모드로 설정하지만, 우리는 **PLL suspend 해제 한 줄**
(FreeBSD `rk_usb2phy.c` 의 `clk_ctl`, offset 0x08)만 했고 나머지는
U-Boot 가 `preboot=usb start` 로 **호스트 모드**로 남겨 둔 값이다.

**⚠ PHY 를 일찍 배제한 것이 잘못이었다.** `CONNECTION_DONE` 이 온다는
이유로 "PHY 는 정상" 이라고 판단했는데, 링크가 올라오는 것과 안정적으로
HS 를 유지하는 것은 다르다.

### 다음 작업 — USB2PHY GRF 의 otg-port 레지스터

필요한 것: RK3566/RK3568 `USB2PHY0_GRF`(`syscon@fdca0000`)의 otg-port
설정 — 포트 suspend 해제, vbus/id 감지, device 모드 전환.

출처 후보:
- RK3568 TRM (공개 PDF, 전체 파싱은 시간 초과 — 장 단위 접근 필요)
- FreeBSD `rk_usb2phy.c` 는 `"Only host is supported right now"` 라 없음
- 허용 라이선스 베어메탈 프로젝트 (예: `4ms/rk3566-baremetal`) — 라이선스 확인 필요

### 검토에서 배제한 가설 (Fable, 2026-11-04)

**"SET_ADDRESS 의 DEVADDR 를 상태 단계 뒤로 미뤄야 한다"** — 틀렸다.

인트리 드라이버를 확인한 결과:

```
udc_dwc2.c:2199   data->caps.addr_before_status = true;   형제 Synopsys 코어
인트리 6개 중 5개가 true
```

Synopsys 코어는 **상태 단계 전에 DEVADDR 를 쓰는 것이 정상 패턴**이고
Zephyr 가 `caps.addr_before_status` 로 명시 지원한다. 우리 설정이 맞다.
적용 전에 확인한 것이 다행이었다.

다만 같은 검토의 **관찰**("깨끗한 리셋 뒤 3초 침묵", "모든 SETUP 이
리셋 전 도착")이 조사를 링크 계층으로 되돌려 준 결정적 기여였다.

### 이번에 들어간 수정

**리셋 처리 뒤 이벤트 배치를 끊는다.** Zephyr 의 usbd 스레드는 어떤
이벤트를 꺼내든 맨 먼저 `event_handler_ep_request()` 로 `ep_events` 를
비운다 (`usbd_core.c:161`). 그래서 RESET 과 SETUP 이 같은 큐에 함께 있으면
**언제나** SETUP 이 먼저 처리되고 리셋이 나중에 처리된다 — 제출 순서를
바꿔도 소용이 없다. 배치를 끊으면 usbd 가 리셋을 끝낸 뒤 다음 배치가
올라간다. 첫 리셋의 순서 역전이 이것으로 해소됐다.
