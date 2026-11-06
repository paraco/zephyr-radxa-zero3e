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

## USB2PHY GRF 레지스터 — 출처 확보 (2026-11-04)

### 출처

**Rockchip RK3568 TRM Part1 V1.3, 3.9절 "USBPHY_U3_GRF_Register
Description" (419쪽).** 벤더 공식 문서이므로 라이선스 제약이 없다.

`rockchip.fr/Rockchip%20RK3568%20TRM%20V1.3%20Part1.pdf` (11.3 MB).

**⚠ 조사 방법 교훈: 큰 벤더 PDF 는 내려받아 `pdftotext -layout` 으로
추출하고 `grep` 하는 것이 압도적으로 빠르다.** firecrawl 의 질의 추출로
852쪽을 두 번 시도했으나 한 번은 타임아웃, 한 번은 빈 결과였다. 로컬
처리는 몇 초였고 컨텍스트 비용도 없다.

### 메모리맵 (TRM 확인)

```
USBPHY_U3_GRF   0xFDCA0000   32KB   <- OTG PHY 의 GRF (우리가 쓰는 것)
USBPHY_U2_GRF   0xFDCA8000   32KB
USB2PHY_U3OTG   0xFE8A0000   64KB   <- PHY 자체 (DT 의 usb2-phy@fe8a0000)
USB2PHY_U2HOST  0xFE8B0000   64KB
```

### 레지스터 요약

| 이름 | offset | reset | 용도 |
|---|---|---|---|
| `USBPHY_U3_GRF_CON0` | 0x0000 | 0x0C52 | **OTG 포트 UTMI 제어** |
| `USBPHY_U3_GRF_CON1` | 0x0004 | 0x01D2 | HOST 포트 UTMI 제어 |
| `USBPHY_U3_GRF_CON2` | 0x0008 | 0x0000 | **bvalid/vbusvalid, PLL, 충전 감지** |
| `USBPHY_U3_GRF_CON3` | 0x000C | 0x0019 | drvvbus, fs 신호, HOST pulldown |
| `USBPHY_U3_GRF_CON4` | 0x0010 | 0x0000 | **OTG suspend 제어 (write_enable 없음!)** |
| `..._LS_CON` | 0x0040 | 0x30100 | linestate 필터 시간 |
| `..._DIS_CON` | 0x0044 | 0x30100 | disconnect 필터 시간 |
| `..._BVALID_CON` | 0x0048 | 0x30100 | bvalid 필터 시간 |
| `..._ID_CON` | 0x004C | 0x30100 | id 필터 시간 |
| `..._INT_MASK` | 0x0080 | 0x0000 | 인터럽트 마스크 (bvalid/id/disconnect 에지) |
| `..._INT_STATUS` | 0x0084 | 0x0000 | 인터럽트 상태 |
| `..._STATUS` | 0x00C0 | 0x0000 | **PHY 상태 (아직 읽어 보지 않음)** |

CON0~CON3 과 INT_MASK 는 hiword-mask ("Write enable for lower 16bits,
each bit is individual"). **CON4 는 예외로 write_enable 이 없다** (31:7 이 RO).

### device 모드에 필요한 비트 (TRM 비트 이름 그대로)

```
CON0 비트 10  usbotg_utmi_iddig      reset 1   GRF OTG plug iddig 지시자
CON0 비트  9  usbotg_utmi_iddig_sel  reset 0   1=GRF 지시자를 컨트롤러에
CON2 비트 15  bvalid_vbusvalid_sel   reset 0   1=GRF 값을 bvalid/vbusvalid 로
CON2 비트 14  bvalid_vbusvalid_grf   reset 0   컨트롤러에 줄 값
```

USB 디바이스는 **bvalid(VBUS 유효)** 없이는 D+ 풀업과 HS 협상을 끝낼 수
없다. reset 상태에서는 PHY 의 자체 VBUS 감지에 의존하는데, 이 보드의
USB-C 포트에서는 그 경로가 연결되지 않은 것으로 보인다. iddig 도 마찬가지로
USB-C 에는 ID 핀이 없다.

### 적용 결과 — 반영됐지만 해결되지 않았다

```
PHY peripheral 고정: CON0=0x06000600 CON2=0xc000c000
                     읽기: CON0=0x00000e52  CON2=0x0000c000
```

읽기값에 비트 9·10(CON0)과 14·15(CON2)가 모두 서 있다 — **쓰기는 성공**.
그러나 리셋 36회, CONNECTSPD 22:14 요동, STATUS done 1 로 변화가 없다.
이 두 비트만으로는 부족하다.

### 이전 PHY "수정" 은 무동작이었다

`CON2` 에 쓰던 `0x00100000` (FreeBSD `rk_usb2phy.c` 의 `clk_ctl` enable
마스크)은 비트 4 `usbphy_commononn` 을 0 으로 쓰는 것이고 **reset 값이 이미
0** 이다. PHY PLL 은 애초에 suspend 가 아니었고, 그래서 그 수정이 아무
변화를 만들지 못했다. 다른 SoC(rk3399)용 값을 그대로 가져온 결과다.

## 로그 분석 방법의 치명적 결함 (2026-11-04)

부팅 구간을 **배너(`PiZZa — Radxa Zero 3E dwc3 bringup`) 부터** 잘라
분석했는데, 그 배너는 `main()` 이 찍고 **`init` 로그는 `main()` 보다 먼저
나온다.** 즉 `DWC3 core 0x5533`, `PHY peripheral 고정`, `core+PHY reset`,
`GCTL PRTCAPDIR` 같은 **초기화 로그를 매번 잘라 버리고 있었다.**

그 결과 PHY 수정이 적용됐는지조차 확인하지 못해 "코드가 안 올라갔다" 고
오판했다. 올바른 방법:

```
N=$(grep -an "Radxa Zero 3E dwc3 bringup" "$LOG" | tail -1 | cut -d: -f1)
sed -n "$((N-140)),\$p" "$LOG"      # 배너 앞 140줄을 포함한다
```

## 다음 벽 — EP0 상태 기계와 리셋의 상호작용

```
XNR stage=2 (STATUS)                 호스트가 상태 단계를 요구
STATUS  state=0                      보류 (state 가 SETUP)
EP0 XferComplete but TRB still HW-owned   그 직전에 SETUP 을 놓쳤다
```

SETUP 을 놓치면 `ep0_state` 가 `SETUP` 에 머물고, 스택이 내려 준 상태 단계
버퍼는 `state != STATUS && state != TWO_STAGE` 로 **영구 보류**된다. 스택은
제어 전송이 진행 중이라고 믿고 드라이버는 아니라고 믿는 불일치다.

측정값: SETUP 6개(전부 `00 05`), `device address = 1` 6회,
**`STATUS done` 1회**, GET_DESCRIPTOR 0회.

읽어 볼 것: `USBPHY_U3_GRF_STATUS` (0x00C0) — PHY 가 실제로 무엇을 보고
있는지. 아직 한 번도 읽지 않았다.

## PHY 는 정상이다 — STATUS 실측으로 확정 (2026-11-04)

`USBPHY_U3_GRF_STATUS` (0x00C0) 를 `init` 과 매 `CONNECTION_DONE` 에서 읽어
해독했다 (TRM Part1 V1.3 3.9절).

```
PHY STATUS[init]=0x00384644 linestate=0 vbusvalid=1 bvalid=1 avalid=1
                            sessend=0 iddig=1 hostdisc=0 lsfsrcv=0
PHY STATUS[conn]=0x00384e55 linestate=1 ... lsfsrcv=1
PHY STATUS[conn]=0x00384656 linestate=1 ...
PHY STATUS[conn]=0x00384644 linestate=0 ...
```

| 비트 | 값 | 판정 |
|---|---|---|
| `vbusvalid` | 1 | VBUS 인식 ✅ |
| `bvalid` | 1 | 세션 유효 ✅ |
| `sessend` | 0 | 세션 종료 아님 ✅ |
| `iddig` | 1 | **B-device(주변장치)로 인식** ✅ |
| `hostdisc` | 0 | 호스트 분리 아님 ✅ |
| `linestate` | 0↔1 | SE0(리셋 중)과 J(유휴) 교대 |

**PHY 는 호스트 연결을 완전히 정상으로 인식한다.** `linestate` 가 오가는
것은 버스 리셋이 반복되기 때문이며 원인이 아니라 결과다. 따라서
**CONNECTSPD 의 HS/FS 요동도 PHY 문제가 아니다** — 열거 실패에 따른 호스트의
재시도다.

### 이로써 닫힌 조사선

- USB2 PHY 설정 부족 → **아님** (STATUS 가 정상)
- VBUS/ID 감지 실패 → **아님** (vbusvalid=1, iddig=1)
- 전기적 불안정 → **아님** (sessend=0, hostdisc=0)
- U-Boot xhci 유령 TRB → 아님 (앞서 배제)
- TRB 슬롯/캐시/write-back 경합 → 아님 (앞서 배제)
- 물리 주소 불일치 → 아님 (trb_virt == trb_phys == 0x4002b440)

### 남은 하나 — EP0 상태 기계와 리셋의 상호작용

```
XNR stage=2 (STATUS)                        호스트가 상태 단계를 요구
STATUS  state=0                             보류 (state 가 SETUP)
EP0 XferComplete but TRB still HW-owned     그 직전에 SETUP 을 놓쳤다
```

SETUP 을 놓치면 `ep0_state` 가 `SETUP` 에 머물고, 스택이 내려 준 상태 단계
버퍼는 `dwc3_handle_xfer_next` 의 `state != STATUS && state != TWO_STAGE`
조건으로 **영구 보류**된다. 스택은 제어 전송이 진행 중이라고 믿고 드라이버는
아니라고 믿는 불일치다.

측정값: SETUP 6개(전부 `00 05` SET_ADDRESS), `device address = 1` 6회,
**`STATUS done` 1회**, GET_DESCRIPTOR 0회, 리셋 36~38회.

다음에 볼 것: `dwc3_handle_xfer_next` 의 상태 단계 보류 조건과,
`dwc3_ep0_xfer_complete` 가 EP0 OUT 의 XferComplete 를 SETUP 이 아닌 것으로
처리할 때의 경로. 스택의 ch9 상태와 드라이버의 `ep0_state` 를 어떻게
동기화할지가 설계 문제다.

## 🎯 열거 성공 (2026-11-05)

```
+-o PiZZa dwc3 bringup@01100000  <IOUSBHostDevice, registered, matched, active>
USB Product Name = "PiZZa dwc3 bringup"   USB Vendor Name = "Zephyr Project"
idVendor = 0x2FE3   idProduct = 0x0004
/dev/cu.usbmodem1101  /dev/tty.usbmodem1101
```

보드 쪽 로그도 전 과정을 보여 준다:

```
DEPSTARTCFG(base=2) status=0x0
ep 0x81 (phys 3) enabled: type=3 mps=16   CDC 알림 (인터럽트)
ep 0x82 (phys 5) enabled: type=2 mps=64   CDC IN  (벌크)
ep 0x01 (phys 2) enabled: type=2 mps=64   CDC OUT (벌크)
usbd_cdc_acm: Configuration enabled       SET_CONFIGURATION 완료
```

### 마지막 두 수정이 결정적이었다

**① `CONFIG_LOG_MODE_IMMEDIATE` 를 버렸다 — 이것이 가장 큰 원인이었다.**

즉시 모드는 로그 한 줄을 그 자리에서 UART 로 밀어내며 스레드를 멈춘다.
115200 에서 100자는 약 9 ms 이고, 리셋 처리 하나가 12줄 이상을 찍으니
**100 ms 가 넘는다.** USB 규격은 `SET_ADDRESS` 가 **50 ms** 안에 완료되어야
한다고 정하므로, 로그가 그 예산을 통째로 먹고 호스트가 타임아웃·리셋을
반복했다 (실기에서 26~38회).

**진단 도구가 진단 대상을 망가뜨린 두 번째 사례다** (첫 번째는 1024바이트
LED 스레드가 크래시 원인이었던 것). 그리고 이것이 설명하는 범위가 넓다 —
`PRTCAPDIR`·재무장 시점·배치 중단·RESET 순서·PHY 강제 등 **프로토콜 수정이
전부 효과가 없었던 이유**가 원인이 프로토콜이 아니었기 때문이다.

**② EP0 의 `DEPENDXFER` 구멍을 메웠다.**

EP0 의 `DEPSTRTXFER` 에 `CMDIOC` 를 켜서 `CMD_CMPLT` 이벤트로 `rsrc_id` 를
받아 두고, 리셋 때 EP0 양방향에 `DEPENDXFER` 를 낸다. Linux 의
`dwc3_gadget_reset_interrupt()` 도 재무장 '전에'
`dwc3_ep0_reset_state()` → `dwc3_ep0_end_control_data()` 로 같은 일을 한다.
그 단계가 빠져 있어, 호스트가 부분 소비한 TRB 가 컨트롤러 시야에 `HWO=1`
로 남았다 — `TRB still HW-owned` ×8 의 설명이다.

주의점 둘:
- `dwc3_ep_end_xfer()` 는 `rsrc_valid` 가 없으면 `dwc3_handle_events()` 를
  되부르므로, 리셋 핸들러(그 함수 안)에서 쓰면 **재귀**가 된다 →
  재귀하지 않는 `dwc3_ep0_end_xfer()` 를 따로 만들었다.
- `memset(priv->ep, ...)` 가 `rsrc_id` 를 지우므로 **그보다 먼저** 끊어야
  한다.

### 남은 사소한 것 — 기능 문제 아님

```
<err> usbd_ch9: HWINFO not implemented or enabled
setup: 80 06 03 03 09 04 ...      GET_DESCRIPTOR(String index 3 = 직렬번호)
<inf> usbd_ch9: not supported
```

호스트가 **직렬번호 문자열 디스크립터**를 요청하는데, Zephyr 의 usbd 는
그것을 HWINFO(하드웨어 고유 ID)로 만든다. 우리 빌드에 `CONFIG_HWINFO` 가
없어 스택이 올바르게 STALL 하고 호스트는 넘어간다. 열거와 동작에는 영향이
없다. 필요하면 `CONFIG_HWINFO=y` 로 없앨 수 있다.

### 되돌아보면

시작: "USB 포트가 아예 나타나지 않는다"
끝:   macOS 가 `PiZZa dwc3 bringup` 을 열거하고 CDC-ACM 포트를 만든다

중간에 배제한 가설이 많았다 (PHY 설정, VBUS/ID 감지, 전기적 불안정,
U-Boot 유령 TRB, TRB 슬롯·캐시·write-back 경합, 물리 주소 불일치,
`SET_ADDRESS` 의 `DEVADDR` 시점). **실제 원인은 두 가지였고 그중 하나는
우리가 켜 둔 진단 설정이었다.**

## 🎯 USB 콘솔까지 동작 — 끝까지 확인 (2026-11-05)

`apps/usbled` 는 콘솔을 CDC-ACM 으로 내보낸다
(`chosen zephyr,console = &cdc_acm_uart0`). 즉 **드라이버가 자기가 올린
USB 로 자기 로그를 보낸다.**

```
DT_CHOSEN_zephyr_console = DT_N_S_soc_S_usb_fcc00000_S_cdc_acm_uart0
```

Mac 쪽:

```
+-o PiZZa dwc3 console@01100000  <IOUSBHostDevice, active>
/dev/cu.usbmodem1101   (전원 투입 후 14초)
```

`cat /dev/cu.usbmodem1101` 로 18초간 **4,524 바이트**를 받았다:

```
*** Booting Zephyr OS build v4.4.0-3902-gfb0fa3dd488d ***
<inf> udc_dwc3: DWC3 core 0x5533 release 0x300a
<inf> udc_dwc3: PHY STATUS[init]=0x00384644 vbusvalid=1 bvalid=1 iddig=1 ...
<inf> usbled: usbled alive 5
<inf> udc_dwc3: ep 0x81 (phys 3) enabled: ...
```

**지연 로그 버퍼가 초기 메시지를 담아 두었다가 호스트가 포트를 열자
쏟아냈다** — 그래서 init 구간까지 보인다. 콘솔이 USB 라서 호스트가 열기
전의 출력은 원래 버려지는데, 지연 모드가 그것을 살렸다. 즉시 모드를 버린
것이 열거를 고쳤고 덤으로 초기 로그까지 살렸다.

### 이것이 증명하는 것

- 열거 (SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION)
- 제어 엔드포인트 양방향
- **벌크 IN 전송** (로그가 EP 0x82 로 흐른다)
- 인터럽트 엔드포인트 설정 (EP 0x81)
- 호스트 드라이버 바인딩 (macOS 가 `/dev/cu.*`, `/dev/tty.*` 를 만들었다)

### apps/usbled 설정에서 중요한 점

`apps/usbuart` 와 **같은 설정**을 써야 한다. 콘솔 목적지만 다르다.
처음에는 `USBD_CDC_ACM_CLASS`, 지연 로그, 스택 4096 세 개,
`MAX_XLAT_TABLES` 가 모두 빠져 있었다 — 그대로 올리면 실패한다.

### 정리한 임시 계측

- `XNR raw=...` 로그를 WRN -> DBG (해독이 끝났고 호스트 폴링마다 찍힌다)
- `dwc3_ep0_purge_stale()` 삭제 (실기에서 역효과였다. 근거와 실패 기록은
  이 문서와 git 이력에 남는다)

남겨 둔 것:
- `TRB still HW-owned` 경로의 TRB/버퍼 덤프 — 이제 발생하지 않으므로
  다시 나타나면 경고로 쓸모가 있다
- `PHY STATUS` 해독 로그 — PHY 문제를 다시 의심할 때 첫 번째 창

## 직렬번호 디스크립터 — 오류 제거 (2026-11-05)

### 왜 오류가 났는가

Zephyr 의 usbd 는 직렬번호 문자열 디스크립터를 `hwinfo_get_device_id()`
로 만든다 (`usbd_ch9.c` 의 `get_sn_from_hwid`). 구현이 없으면:

```
<err> usbd_ch9: HWINFO not implemented or enabled
<inf> usbd_ch9: protocol error:
<inf> usbd_ch9: setup: 80 06 03 03 09 04 02 00   GET_DESCRIPTOR(String, index 3)
<inf> usbd_ch9: not supported
```

`CONFIG_HWINFO` 는 꺼져 있었는데도 났다 — 디바이스 디스크립터가
`iSerialNumber` 를 광고하면서 그 문자열은 등록하지 않는 구조 때문이다.
열거·동작에는 영향이 없지만 로그가 지저분하다.

### 고친 방법

`CONFIG_HWINFO=y` + `drivers/hwinfo/hwinfo_rk3566.c` 에 구현 제공.
Zephyr 의 `z_impl_hwinfo_get_device_id()` 는 `__weak`
(`drivers/hwinfo/hwinfo_weak_impl.c`) 이므로 강한 심볼로 정의하면 링크 때
우리 것이 쓰인다. `CONFIG_HWINFO_RK3566_PLACEHOLDER` 로 켜고 끈다.

결과 (실기):

```
USB Serial Number = "524B333536360001"   = "RK3566" + 0x00 0x01
Device Speed      = 2                     = High Speed
<err> 0건 · protocol error 0건 · not supported 0건
```

### (아래 자리표 내용은 2026-11-05 OTP 읽기 구현으로 대체됐다)

### ⚠ (옛 기록) 자리표였다

보드마다 같은 값이 나온다. RK3566 의 실제 칩 고유 ID 는
**OTP(`OTP_NS` @0xFE38C000)** 에 있고, 읽으려면:

- OTP 컨트롤러 시퀀스 (레지스터 설명이 RK3568 TRM **Part1 에 없다** —
  Part2 또는 미공개 문서가 필요)
- `clk_otpc_ns` / `pclk_otpc_ns` 게이팅 (CRU 쪽 비트는 Part1 에 있다)
- 칩 ID 의 OTP 내 오프셋

한 대를 붙여 쓰는 bring-up 에서는 문제가 없다. 여러 대를 동시에 붙이거나
제품으로 쓰려면 OTP 읽기를 구현해 이 파일을 대체해야 한다. 값에 의미를
담아(`'R','K','3','5','6','6',0,1`) 로그에서 자리표임을 알아보기 쉽게 했다.

## OTP 칩 고유 ID 읽기 — 구현 완료 (2026-11-05)

자리표를 실제 OTP 읽기로 대체했다.

```
USB Serial Number = "4D425737333500000000000000032106"

원시: 4D 42 57 37 33 35 00 00 00 00 00 00 00 03 21 06
ASCII: M  B  W  7  3  5                       (개체 구분값)
```

앞 6바이트가 ASCII `MBW735` — Rockchip 칩 ID 의 로트/웨이퍼 코드 형식이다.
자리표(`524B333536360001` = "RK3566\x00\x01")가 아니므로 OTP 경로가 실제로
동작했음이 확정된다. 오류·시간 초과·ECC 오류 모두 0건.

### 출처와 라이선스 판단

| 필요한 사실 | 출처 |
|---|---|
| CRU_NS 0xFDD20000 · OTP_NS 0xFE38C000 | RK3568 TRM Part1 V1.3 메모리맵 |
| 클럭 게이트 `CRU_GATE_CON26`(0x0368) 비트 11·10·9, `CON34`(0x0388) 비트 13 | TRM Part1 — "When high, disable clock" |
| 소프트리셋 `CRU_SOFTRST_CON22`(0x0458) 비트 14·13·12, `CON28`(0x0470) 비트 15·14 | TRM Part1 — "When high, reset relative logic" |
| **OTP 컨트롤러 레지스터·읽기 절차** | ⚠ **공개 TRM 양쪽에 없다.** Part2 의 33개 장을 모두 확인했고 OTP 장이 없다. Linux `drivers/nvmem/rockchip-otp.c` (GPL-2.0) 에서 사실로 확인 |
| 칩 ID 위치 (바이트 0x0A, 16바이트 → 워드 5번지 8워드) | Linux `arch/arm64/boot/dts/rockchip/rk356x-base.dtsi` 의 `otp_id: id@a { reg = <0x0a 0x10>; }` |

**레지스터 오프셋·비트 위치·절차만 사실로 확인하고 코드는 직접 작성했다.**
하드웨어 레지스터 번호는 저작 표현이 아니라 사실이라는 판단이다. 이
프로젝트는 그 외에는 BSD-2/Apache 와 벤더 문서만 인용한다. 판단이 바뀌면
`CONFIG_HWINFO_RK3566_OTP=n` 으로 끄거나 자리표 판으로 되돌릴 수 있다
(git 이력에 있다).

### 동작 방식

```
클럭 게이트 해제 (CRU)  →  소프트리셋 펄스 (CRU)  →  SBPI 로 ECC 켜기
→  USER 모드 전환  →  워드 5~12 를 하나씩 읽고 USER_QP 로 ECC 검사
→  USER 모드 해제
```

OTP 는 한 번만 읽고 캐시한다 — 값이 변하지 않는다.

읽기에 실패하면 자리표 ID 를 쓰고 `LOG_WRN` 을 남긴다. USB 열거를 깨끗하게
유지하되 실패를 숨기지는 않기 위한 선택이다.

### 참고 — 측정 중 주의

콘솔이 USB 인 빌드에서 `cat /dev/cu.usbmodem*` 가 `Resource busy` 로 실패할
수 있다 (다른 프로세스가 포트를 쥔 경우). 그때도 `ioreg` 의
`USB Serial Number` 는 hwinfo 결과를 그대로 반영하므로 판정에 쓸 수 있다.

---

## ✅ 2026-11-05 — UART2 셸 동작 (apps/moru 1단계)

`Moru > ` 프롬프트가 UART2 에서 동작하고, 동시에 USB CDC-ACM 이 열거되며
OTP 칩 ID 가 USB 직렬번호로 나간다.

```
[00:00:00.053,000] <inf> main: MoruLab Zephyr RTOS is up
[00:00:00.223,000] <inf> hwinfo_rk3566: OTP 칩 ID: 4d425737333500000000000000032106
[00:00:00.228,000] <inf> usbd_cdc_acm: Configuration enabled
Moru > help
Available commands:
  app · clear · device · help · history · kernel · rem · resize · retval · shell
```

호스트 쪽: `PiZZa dwc3 bringup@02131000`, idVendor 0x2FE3, idProduct 4,
Device Speed 2(HS), Serial Number = OTP 칩 ID. `<err>` 0줄.

### 어떻게 찾았나 — 방법이 결론을 좌우했다

셸을 켠 첫 빌드부터 보드가 **완전히 무음**이었다 (LED 도 안 깜박임).
왕복 여섯 번을 썼고 그중 네 번은 내가 기준점 없이 추측으로 움직여서
낭비했다. 결정적이었던 세 가지:

1. **기준점(control) 확인.** 검증된 `apps/usbuart` 를 같은 카드·같은
   `boot.scr` 로 구워 LED 가 깜박이는 것을 확인했다. 이것으로 보드·카드·
   배선·전원·부팅 사슬이 전부 정상임이 증명되고 탐색 범위가 절반이 됐다.
   **이걸 처음에 하지 않은 것이 가장 큰 실수였다.**

2. **U-Boot 를 1.5 Mbps 로 읽기.** 지금까지 쓰레기로만 보여서 보드가
   무엇을 하는지 전혀 몰랐다. 열어 보니 U-Boot 는 할 일을 정확히 하고
   있었다 — `boot.scr` 실행, `fatload` 성공, 진입점 0x40001084 일치.
   덤으로 BL31 v2.3 이 EL3 에서 GICv3 를 초기화한다는 것과 U-Boot 가
   EL2(SPSR 0x3c9)에서 돈다는 것도 확인했다.

3. **`.config` 를 직접 비교하기 (왕복 0회).** prj.conf 에 2줄만 더했는데
   실제 차이는 **70개**였다:
   - `CONFIG_SHELL=y` → `KERNEL_SHELL`·`DEVICE_SHELL`·`DEVMEM_SHELL`·
     `DATE_SHELL` 이 기본 y
   - `KERNEL_SHELL` → `INIT_STACKS`·`THREAD_MONITOR` 를 **imply**
   - `SHELL_BACKEND_SERIAL=y` → `LOG_BACKEND_UART` 가 자동으로 **사라진다**
     (`Kconfig.uart:7`)

   이 넷을 명시적으로 `n` 으로 끄자 부팅했다. imply 는 강제가 아니라
   끌 수 있다. **prj.conf 에 쓴 줄 수가 실제 변경량이 아니다 — 반드시
   `.config` 를 비교해야 한다.**

### ⛔ 범인 확정: `CONFIG_INIT_STACKS` (2026-11-05, 실기 3회)

| 설정 조합 | 결과 |
|---|---|
| 넷 다 끔 (`777091b`) | 부팅 ✅ `Moru >` · `help` 동작 |
| `INIT_STACKS`+`THREAD_MONITOR` 켬 (`464a4ce`) | **무음** ❌ |
| `INIT_STACKS` 만 끔, `THREAD_MONITOR` 켬 (`fc0d8eb`) | 부팅 ✅ |

- `DEVMEM_SHELL`·`DATE_SHELL` — 무죄 (성공·실패 두 판 모두 꺼져 있었다)
- `THREAD_MONITOR` — 무죄 (성공한 판에 켜져 있다)
- **`INIT_STACKS` — 유죄.** 켜면 무음, 끄면 부팅.

**증상.** U-Boot 는 정상 진행하고 `go 0x40001084` 까지 간다. 그 뒤 Zephyr 가
읽을 수 있는 출력을 **한 글자도** 내지 않는다. `uart_poll_out` 으로 직접 찍는
동기 마커조차 안 나온다. 즉 `PRE_KERNEL_2` 보다 앞에서 죽는다.

**기전은 설명되지 않았다.** `arch/arm64/core/reset.S:204-215` 가 MMU·콘솔보다
먼저 ISR 스택을 0xAA 로 칠하는데, 이 보드 수치로는 그 루프가 정상이다:
`CONFIG_ISR_STACK_SIZE=4096` = `__z_interrupt_stack_SIZEOF` 0x1000,
`z_interrupt_stacks` 가 0x…200 정렬, 시작·끝 차이 0xFF8(8의 배수)이므로
`beq` 종료 조건이 정확히 맞는다. 영역을 벗어나 쓰지도 않는다.
**왜 죽는지는 모른다** — 정적 분석으로는 결함을 찾지 못했다. 캐시·MMU 가
꺼진 상태에서의 4 KiB 쓰기가 이 SoC 에서 다른 영향을 주는지는 미확인이다.

**영향 — 잃는 기능.** `kernel thread list` 와 `kernel thread stacks` 가 둘 다
`INIT_STACKS` 를 요구한다 (`thread/Kconfig:16-18,26-28`). 그래서 이 보드에서는
쓸 수 없다. `THREAD_MONITOR` 는 쓸 수 있으므로 `kernel thread suspend`·
`resume`·`kill` 은 남는다. 스택 사용량을 보려면 `CONFIG_STACK_SENTINEL`
(넘침 감지)과 `west build` 의 정적 보고로 대신한다.

**장래 조사 거리 → 2026-11-06 에 유력한 가설이 생겼다.**
`arch/arm64/core/Kconfig:446` 의 `ARM64_BOOT_DISABLE_DCACHE` 가 "Zephyr 가
EL2 에서 부팅하는데 dcache 가 켜져 있으면 켜라" 고 기술하는데, 이 보드는
U-Boot 가 EL2(SPSR 0x3c9)에서 `go` 하고 그 설정은 `ARM64_DCACHE_ALL_OPS=n`
때문에 선택조차 되지 않는다. 즉 0xAA 칠하기가 EL2 dcache 에 dirty 로 남고
EL1(MMU 꺼짐)이 DRAM 에 쓴 값을 나중에 덮는다는 가설이다.
**실기 1회로 가른다** — `CONFIG_ARM64_DCACHE_ALL_OPS=y` +
`CONFIG_ARM64_BOOT_DISABLE_DCACHE=y` + `CONFIG_INIT_STACKS=y`.
근거·기전·실험 순서는 [trm-grounding.md](trm-grounding.md) §7.

⚠ `reset.S` 의 루프만 지우는 실험은 **"루프 결함"과 "캐시 상태"를 가르지
못한다** (둘 다 삭제로 고쳐진다). 위 실험 뒤에만 의미가 있다.

### 넷 중 어느 것인지 좁히는 과정에서 쓴 방법

 상관관계로는 `INIT_STACKS`
가 가장 유력하다 — 동작하는 `usbuart` 에 없고 실패한 모든 빌드에 있었으며,
`usbuart/prj.conf:61-62` 가 "기동 실패 원인 가리려고 껐다" 고 적어 둔 바로
그 설정이다. arm64 리셋 경로(`reset.S:204-215`)에서 MMU·콘솔보다 먼저 ISR
스택을 0xAA 로 칠한다. 다만 이 보드 수치로는 그 루프가 정상 종료한다
(`ISR_STACK_SIZE=4096` = `__z_interrupt_stack_SIZEOF` 0x1000,
`z_interrupt_stacks` 가 0x…200 정렬, 시작·끝 차이 0xFF8 = 8의 배수).
즉 **입증된 결함이 아니다.**

이것이 중요한 이유: `INIT_STACKS` 는 `kernel thread stacks` 명령에
필요하다. 범인이 그것이면 그 명령을 포기해야 한다.

### 진단 장치가 피검체를 죽인 사례 (이 세션에서 두 번)

- `moru_stage.c` 가 `SYS_INIT(..., PRE_KERNEL_1, 1)` 에서 `device_map()` 을
  불렀다. 불법이다 — `z_mem_manage_init()`(`kernel/init.c:293`)이
  `PRE_KERNEL_1`(`:560`)·`PRE_KERNEL_2`(`:564`)보다 뒤, `POST_KERNEL`(`:300`)
  바로 앞에서 돈다. `device_map` 은 스레드 문맥에서, 또는 `POST_KERNEL`
  이후에만 부른다.
- 앞서 LED 스레드 스택을 1024 로 두어 크래시한 것과 같은 종류다.

### 측정 방법에 대한 교훈

- **`tio --log-file` 은 실시간이 아니다** (버퍼링). 판단은 터미널 화면으로.
  `--log-strip` 은 출력 불가 문자를 버려서 U-Boot 쓰레기가 로그에 안 남는다.
- **보율이 틀린 구간의 쓰레기에서 문자열을 grep 하면 안 된다.** 무작위
  바이트에 찾는 글자가 섞여 거짓 양성·음성이 둘 다 난다.
- U-Boot 는 **1.5 Mbps**, Zephyr 는 **115200**. 한 번에 둘 다 읽을 수 없다.
- `diskutil list` 에 카드가 안 보이는 것은 **복사 성공의 증거가 아니다**
  (손으로 빼도 같다). `mdir` 출력을 실제로 확인해야 한다.
- `sudo` 비밀번호 오류로 `&&` 사슬이 끊기면 복사가 조용히 안 된다.
  `sudo -v` 로 먼저 받아 두면 피할 수 있다.


## 🎯 INIT_STACKS 무음 해결 — EL2 데이터 캐시 (2026-11-06)

**실기 1회로 확정했다.** 빌드 `ef6780f`, `sdcard/zephyr.bin` 150152 B.

```conf
CONFIG_INIT_STACKS=y
CONFIG_ARM64_DCACHE_ALL_OPS=y
CONFIG_ARM64_BOOT_DISABLE_DCACHE=y
```

### 결과

```
[2][<S][S>][<U][U>][M]
*** Booting Zephyr OS build v4.4.0-3902-gfb0fa3dd488d ***
...
[00:00:00.356,000] <inf> usbd_cdc_acm: Configuration enabled
Moru > kernel thread stacks
0x40036340 shell_uart   (real size 3072):  unused  876  usage 2196 / 3072 (71 %)
...
0x400401c0 IRQ 00       (real size 4096):  unused 3596  usage  500 / 4096 (12 %)
```

`<err>` 0줄. `kernel thread list`·`stacks` 둘 다 동작하고, 후자가 `IRQ 00`
(ISR 스택)까지 보여준다 — **`reset.S` 의 0xAA 칠하기가 정상 작동한다는 직접
증거다.**

### 판정 — `INIT_STACKS` 는 무죄였다

`INIT_STACKS` 를 양쪽 다 `y` 로 두고 캐시 설정만 토글해 두 방향을 재현했다:

| 빌드 | `INIT_STACKS` | 캐시 두 설정 | 결과 |
|---|---|---|---|
| `464a4ce` | y | 없음 | 무음 ❌ |
| `ef6780f` | y | **y** | 부팅 ✅ |

앞서 "유죄 입증" 으로 판정한 것은 **방아쇠**였고 원인이 아니었다. 원인은
U-Boot 가 EL2 에서 `go` 하고 `go` 가 캐시를 정리하지 않는 것, 그리고
Zephyr 의 `ARM64_BOOT_DISABLE_DCACHE` 가 기본으로 꺼져 있어 그 상황을
처리하지 않는 것이다. 기전은 [trm-grounding.md](trm-grounding.md) §7.

"유죄 입증" 과 "기전 설명" 을 따로 적어 둔 덕에 이 수정이 가능했다 — 기전을
모르는 채 "해결" 로 닫았다면 `FRAME_POINTER` 도 영구히 포기했을 것이다
([bringup-rules.md](bringup-rules.md) 규칙 4).

### 굽기 전 확인이 왕복 하나를 아꼈다

빌드 크기가 무음 빌드(`464a4ce`)와 **똑같은 150152 B** 였다. 설정이 안 켜진
것으로 보였지만, 디스어셈블로 확인하니 `z_arm64_el2_init` 안에 `dc cisw`
(캐시 clean+invalidate by set/way)와 `tlbi alle2`(`reset.c:170`, 그 `#ifdef`
안에만 있는 명령)가 들어 있었다. 인라인 코드가 정렬 패딩에 흡수된 것이다.
**크기만 보고 "안 켜졌다" 고 판단했으면 왕복 하나를 헛되게 썼다.**

`.config` diff 도 정확히 7줄이었다 — 의도한 셋과, 자동으로 켜진
`KERNEL_THREAD_SHELL_LIST`·`_STACKS` 둘. 번짐이 없었다.

### USB CDC-ACM 에는 셸이 없다 (설계대로)

`/dev/cu.usbmodem*` 를 열고 `help` 를 치면 UART 로그에
`<inf> usbd_cdc_acm: 68 65 6c 70 0d 0a |help..` 가 찍히지만 응답은 없다.
**정상이다.** `zephyr,shell-uart = &uart2` 이므로 셸은 UART2 에만 있고,
CDC-ACM 은 데이터 포트일 뿐이다. 저 hexdump 는 오히려 호스트→보드 수신
경로가 정상이라는 증거다(소비자가 없어 로그로만 남는다).

셸을 USB 로 옮기려면 `chosen zephyr,shell-uart` 를 CDC 노드로 바꾸면 되지만
**UART 셸을 잃는다** — USB 열거 전 부팅 로그를 못 보게 되므로 bring-up 에
불리하다. 양쪽 동시 구동은 `shell_transport` 를 새로 구현하는 별개 작업이다
(`shell_uart.c:527` 의 serial 백엔드는 인스턴스가 하나다).

### 남은 관찰 — USB 속도

`usbd_core: Actual device speed 1`(FS)과 bulk `mps=64`(FS 값, HS 면 512)가
보인다. USB 코드는 이번에 건드리지 않았으므로 이 변경 탓은 아니다. 다만
이전 기록의 "High Speed 열거" 와 어긋나므로 동작판 로그와 비교할 거리다.
`CONNECTSPD=0`(= HS)과도 어긋난다. 단정하지 않고 남긴다.

## 🎯 USB main 전환 성공 (2026-11-06)

사용자 원칙 "main 은 USB, 안 되면 UART 1개" 를 적용한 첫 빌드가 **실기 1회로
동작했다.** 빌드 `3261bb1`, `sdcard/zephyr.bin` 146048 B, 진입점 0x40001084.

### 확인된 것

| 단계 | 결과 |
|---|---|
| ① UART2 동기 마커 | `[2][<S][S>][<U][U>][M]` — 전부 나왔다 |
| ② `[S>]`·`[U>]` | 셸·USB init 모두 통과 |
| ③ 열거 | `/dev/cu.usbmodem213101` · `PiZZa dwc3 bringup` · **`Device Speed = 2` (High Speed)** · 직렬번호 `4D425737333500000000000000032106` |
| ④ USB 셸 | 정상 동작 (사용자 확인) |

**UART2 에는 마커만 나오고 로그·프롬프트는 나오지 않는다 — 의도대로다.**
`chosen` 이 CDC 로 갔고 로그 백엔드는 셸 하나뿐이다
(`.config` 에서 `SHELL_LOG_BACKEND=y`, `LOG_BACKEND_UART` 는 자동 꺼짐).

### 관측 수단이 실제로 작동했다

`moru_mark.c` 를 `chosen` 대신 `uart2` 고정으로 바꾼 것이 핵심이었다.
USB 가 main 이어도 **UART 어댑터만 꽂으면 커널이 어디까지 갔는지 보인다.**
이번에는 성공했으니 ①에서 끝났지만, 실패했다면 ①~⑤(`prj.conf` 맨 끝)로
카드를 바꾸지 않고 원인을 좁힐 수 있었다.

보율 걱정은 근거 있게 배제했다 — ns16550 이 DT 의 `current-speed` 를 스스로
적용한다(`uart_ns16550.c:2079` → init → `pm_action(TURN_ON)` → `configure`
→ `set_baud_rate`:654). 콘솔로 쓰이지 않아도 115200 으로 나온다. 책상에서
1분에 확인했고, 틀렸다면 관측 수단이 죽은 채 왕복을 썼을 것이다.

### 앞서 남긴 USB 속도 관찰 — 해소

`usbd_core: Actual device speed 1`(FS)은 **USB reset 시점의 중간값**이었다.
호스트는 `Device Speed = 2`(High Speed)로 본다. 링크는 HS 다.

남은 의문 하나: bulk 엔드포인트 `mps=64` 는 FS 값이다(HS bulk 는 512).
Zephyr CDC-ACM 이 HS 전용 디스크립터를 따로 두지 않는 것으로 보인다.
**동작에는 문제가 없고 처리량 문제**이므로 지금 손대지 않는다. 대량 전송이
필요해지면 그때 본다.

### 설계 결정을 책상에서 줄인 것이 효과 있었다

실기 0회로 셋을 확인해 묶음을 줄였다:
- CDC `poll_out` 이 버린다(블록 안 함) → **폴링 모드 유지**, `CHECK_DTR`
  불필요 → 셸 실행 모델을 바꾸지 않았다 (변수 1개 제거)
- ns16550 이 보율을 스스로 설정 → 마커를 uart2 로 고정해도 안전
- `.config` 로 로그 백엔드가 0개가 아닌지 확인 → 무음 함정 회피

`boot.cmd` 분기(`zephyr-uart.bin` 우선 읽기)는 **넣지 않았다**. `boot.scr`
재생성이 따르고 실패 시 후보가 하나 늘기 때문이다. 진단 전환은 `mcopy -o`
덮어쓰기로 한다 — `sdcard/zephyr-diag-uart.bin` (150152 B) 을 준비해 뒀다.

## 🎯 역추적 복구 — EL2 캐시가 막고 있던 두 번째 기능 (2026-11-06)

`CONFIG_FRAME_POINTER=y` 한 줄로 **부팅하고 역추적이 동작한다.**
빌드 `48b8c20`, 154240 B. 실기 1회.

```
Moru > kernel thread unwind
Unwinding 0x40037340 shell_uart
ra: 0x40005b28   cmd_kernel_thread_unwind      unwind.c:50
ra: 0x40006a30   exec_cmd                      shell.c:983
ra: 0x40007650   state_collect                 shell.c:1526
ra: 0x40007948   z_flag_processing_set         shell_ops.h:127
ra: 0x40007b14   shell_thread                  shell.c:1851
ra: 0x4000af58   k_sched_current_thread_query  kernel.h:265
```

주소 → 함수 변환:

```bash
~/zephyr-sdk-1.0.1/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-addr2line \
  -e /tmp/claude-501/rz3e-build/zephyr/zephyr.elf -f -C -s <주소>
```

### 과거 기록이 틀렸다 — 그리고 왜 틀렸는지도 안다

README 함정 ④는 "`FRAME_POINTER`/`EXCEPTION_STACK_TRACE` 를 켜면 Zephyr 가
기동조차 못 한다" 였다. **그 증상은 `INIT_STACKS` 와 똑같았고, 원인도 같았다 —
EL2 데이터 캐시다.** 캐시 두 설정이 켜진 구성에서는 그냥 동작한다.

**EL2 캐시 하나가 두 기능을 막고 있었다.** 기전을 "설명되지 않음"으로 정직하게
남긴 값이 두 번 돌아왔다 — "해결" 로 닫았다면 역추적을 영구히 포기했을 것이다
([bringup-rules.md](bringup-rules.md) 규칙 4).

### Kconfig 하나가 체인 넷을 켠다

`arch/arm64/core/Kconfig:210` (`CPU_CORTEX_A` 안):

```
ARCH_HAS_STACKWALK       default y, depends on FRAME_POINTER, imply THREAD_STACK_INFO
  → ARCH_STACKWALK       default y  (arch/Kconfig:489)
    → EXCEPTION_STACK_TRACE  default y  (subsys/debug/Kconfig:317)
```

그래서 `EXCEPTION_STACK_TRACE` 를 따로 시험할 수 없다 — `FRAME_POINTER` 없이는
**선택 자체가 보이지 않는다.** 과거 기록이 "둘 다" 로 적은 것은 사실상 하나였다.

`.config` diff 는 9줄이었고 전부 그 체인 + `KERNEL_THREAD_SHELL_UNWIND` ·
`ARCH_STACKWALK_MAX_FRAMES=8` · `OVERRIDE_FRAME_POINTER_DEFAULT` 다. 번짐 없음.
크기 146048 → 154240 B (+8 KiB, unwinder 코드).

### 고장 코드를 넣지 않고 검증했다

`CONFIG_KERNEL_THREAD_SHELL_UNWIND` 가 체인에 함께 켜져
**`kernel thread unwind [<id16진수>]`** 가 붙는다 (`unwind.c:53`, 인자를 빼면
현재 스레드). 살아 있는 스레드의 콜 스택을 찍으므로 일부러 깨뜨릴 필요가 없다 —
그것은 `moru_stage.c` 가 태어난 패턴이다(규칙 5).

### 로그 버퍼 기준선 (USB main)

같은 세션에서 USB 셸로 `shell stats show` → **`Lost logs: 0`**.
이것이 USB main 구성의 기준선이다. 앞서 본 0 은 UART 빌드 값이라 조건이
달랐다(백엔드가 `POST_KERNEL 90` 에 바로 붙어 0.1 초에 비워진다).
B1 이 부팅 메시지를 ~15줄 늘린 뒤 다시 확인한다.

## ✅ B1 — Linux dmesg 형식 부팅 메시지 (2026-11-06)

빌드 `8266c9b`, 150152 B. 실기 1회. 종료 조건 ①~④ 전부 통과.

```
[    6.699229] moru: ====================================================
[    6.699234] moru: MoruLab Zephyr RTOS 0.1.0 (build 69e1000b0df2)
[    6.699237] moru: Zephyr 4.4.99, built 2026-11-06 06:37:19 UTC
[    6.699238] moru: Machine model: Radxa ZERO 3E
[    6.699241] moru: SoC: Rockchip RK3566 (rk3568 SoC 정의 재사용)
[    6.699242] moru: CPU: Cortex-A55, MIDR 0x412fd050, EL1, MPIDR 0x81000000
[    6.699244] moru: Cache: min D-line 64 B (CTR_EL0 0x84448004)
[    6.699245] moru: Memory: 0x40000000 + 1024 KiB (zephyr,sram)
[    6.699248] moru: Timer: ARM arch timer 24.000000 MHz, 10000 ticks/s, tickless
[    6.953704] hwinfo_rk3566: OTP 칩 ID: 4d425737333500000000000000032106
```

| | 조건 | 결과 |
|---|---|---|
| ① | `[    0.123456]` Linux 형식, `<inf>`·스레드 이름 없음, `moru:` 태그 유지 | ✅ |
| ② | `shell stats show` → `Lost logs: 0` (부팅 메시지 +15줄 뒤) | ✅ |
| ③ | 호스트에 `MoruLab Zephyr RTOS`, `idProduct = 5` (ioreg) | ✅ |
| ④ | `kernel thread unwind` 6프레임 유지 | ✅ |

### µs 해상도가 실제로 나온다

`699229 → 699234 → 699237` 로 수 µs 간격이 보인다. `moru_boot.c` 의
24 MHz 사이클 카운터 override(`log_set_timestamp_func` + `k_cycle_get_64`)가
작동한다는 증거다 — 기본 타임스탬프(1 ms)였다면 전부 `699000` 으로 찍힌다.

### ⚠ 기준점이 Linux 와 다르다 (SoC 리셋 기준)

`6.699` 초는 **SoC 전원 투입 기준**이다. `CNTVCT_EL0` 가 리셋부터 돌기
때문에 U-Boot 가 쓴 ~6.3 초가 포함된다. Linux dmesg 는 **커널 시작을 0** 으로
한다. "Linux 부팅처럼" 이 원래 요구였으므로 B2 에서 오프셋을 빼 0 부터
세게 한다.

덤으로 얻는 사실: **이 보드의 U-Boot 는 Zephyr 진입까지 약 6.3 초 쓴다.**
오프셋을 뺀 뒤에도 그 값을 배너에 한 줄로 남긴다.

### CPU 식별

`MIDR 0x412fd050` → Cortex-A55 **r2p0** (0x41=ARM, 0x2=variant 2,
0xd05=A55, 0x0=revision 0). `MPIDR 0x81000000` → Aff0=0, 즉 primary core.
`CTR_EL0 0x84448004` → 최소 D-line 64 B.

## ✅ B2 + reboot — PSCI 가 응답한다 (2026-11-06)

빌드 `ec96cdd`, 166576 B. 실기 1회. **B4 의 핵심 미지수가 해소됐다.**

```
Moru > moru info
MoruLab Zephyr RTOS
  버전      0.1.0 (build 59d5f19462c1)
  빌드      2026-11-06 06:59:51 UTC
  Zephyr    4.4.99
  보드      radxa_zero_3e/rk3568  (Radxa ZERO 3E)
  SoC       Rockchip RK3566 (rk3568 정의 재사용)
  CPU       Cortex-A55 r2p0, MIDR 0x412fd050, EL1
  메모리    0x40000000 + 1024 KiB (zephyr,sram)
  칩 ID     4d425737333500000000000000032106  (OTP)
  콘솔      cdc_acm_uart0 (셸 같은 포트)
  마커 UART serial@fe660000 115200 8N1 (초기 부팅 전용)
  부트로더  6.700 s (전원 투입 → Zephyr 진입)
  가동      0d 00:00:27 (27342 ms)

Moru > moru reboot probe
probe: issuing PSCI_VERSION (SMC 0x84000000) ...
probe: PSCI 1.1 (raw 0x00010001) — 응답했다

Moru > kernel heap
free:           8192
allocated:      0
max. allocated: 0

Moru > hwinfo devid
Length: 16
ID: 0x4d425737333500000000000000032106
```

### 🎯 PSCI 1.1 이 NS-EL1 의 SMC 에 응답한다

계획 단계에서 **"실기 확인이 필요하다"고 적어 둔 유일한 미지수**였다
(`docs/roadmap.md` B4). `arm_smccc_smc(PSCI_VERSION)` 이 `0x00010001` =
PSCI 1.1 을 돌려줬다. 즉 TF-A BL31 v2.3 이 런타임 서비스를 제공하고
Zephyr 가 EL1 에서 쏜 SMC 가 거기 닿는다.

**결과 둘:**
1. `moru reboot`(`PSCI_SYSTEM_RESET`)이 동작할 근거가 생겼다
2. **TRM 을 다시 받을 사유가 하나 줄었다** — PSCI 가 실패할 때의 대안이던
   CRU 글로벌 소프트 리셋(TRM Part1 CRU 장)이 필요 없을 가능성이 크다.
   `trm-grounding.md` §5 의 1번 우선순위를 내린다.

### 칩 ID 의 지연 읽기 순서는 문제 없었다

검토가 "`rk3566_otp_id_is_valid()` 는 첫 `hwinfo_get_device_id()` 뒤에만
의미가 있다" 고 지적했는데, 실기에서 `(OTP)` 로 정상 표시됐다. USB 초기화가
셸보다 먼저 OTP 를 읽기 때문이다(B1 로그에서 OTP ID 줄이 배너 직후에 나온다).
다만 `diag-uart` 빌드에서는 순서가 다를 수 있으므로 헤더의 조건은 유지한다.

### ⚠ 버전 스탬프가 한 커밋 뒤처진다

`build 59d5f19462c1` 인데 실제 코드는 `ec96cdd` 다. `APP_BUILD_VERSION` 은
`git describe` 로 **빌드 시점의 HEAD** 를 쓰므로, "빌드 → 커밋" 순서로
작업하면 항상 한 커밋 뒤처진다. 기능에는 영향이 없지만 그 해시를 찾으면
그 코드가 없어 혼란을 준다.

→ **순서를 "커밋 → 재빌드 → 복사" 로 한다.** 굽기 전 체크리스트에 넣었다.

## 🎯 B4 완료 + DRAM 이 재부팅을 넘어 보존된다 (2026-11-06)

빌드 `f8f3c15`, 166576 B. **실기 1회에 세 가지를 가렸다.**

```
Moru > moru ota mark
0x50000000 에 표시했다 — magic 0x4d4f5255 pattern 0xa5c3f00d count 1

Moru > moru reboot
재부팅한다 (PSCI_SYSTEM_RESET).
⚠ Connection lost — auto-reconnecting...
✓ Reconnected

[    0.000090] moru: ====================================================
[    0.000097] moru: MoruLab Zephyr RTOS 0.1.0 (build f8f3c15c8585)
[    0.000098] moru: Zephyr 4.4.99, built 2026-11-06 07:15:45 UTC
[    0.000100] moru: Pre-kernel: 6.698 s (power-on → PRE_KERNEL_1)
[    0.000102] moru: Machine model: Radxa ZERO 3E
...
[    0.358092] hwinfo_rk3566: OTP 칩 ID: 4d4257373335...

Moru > moru ota check
0x50000000: magic 0x4d4f5255 pattern 0xa5c3f00d count 1
✅ 보존됐다 — 재부팅을 넘어 DRAM 이 남는다.
```

### ① `moru reboot` 이 동작한다 — B4 완료

`arm_smccc_smc(PSCI_SYSTEM_RESET)` 이 실제로 보드를 리셋한다. `CONFIG_REBOOT`
도 `CONFIG_PM_CPU_OPS` 도 켜지 않았다 — Kconfig 추가 0개로 얻었다(계획
보정 2 의 설계가 맞았다).

**USB 가 자동으로 재열거된다.** `tio` 가 `Connection lost` → `Reconnected`
로 스스로 복귀했다. 즉 재부팅 뒤 UART 로 넘어갈 필요조차 없었다 — USB 셸
하나로 재부팅과 확인이 끝난다.

### ② 타임스탬프 기준점이 맞다 — B2 ② ③ 완료

첫 로그가 `[    0.000090]` 이다. B1 의 `[    6.699229]` 에서 U-Boot 구간이
빠졌고, 그 시간은 `Pre-kernel: 6.698 s` 로 따로 남는다. 두 값이 일치한다
(6.699 ≈ 6.698).

### ③ 🎯 DRAM 이 PSCI 리셋을 넘어 보존된다

`0x50000000` 에 쓴 매직·패턴·카운터가 **재부팅 뒤에도 그대로 남았다.**
BL31 과 DDR 초기화가 이 영역을 지우지 않는다.

**이것이 `docs/ota-strategy.md` §2 의 유일한 미지수였다.** 이제 그 설계가
실현 가능하다:

```
Zephyr OTA 수신 → 0x50000000 에 쌓기 → 매직 헤더 → moru reboot
  → boot.scr 의 itest 로 확인 → fatwrite 로 .old 백업 + zephyr.bin 교체
  → go → 새 펌웨어.  카드도 전원도 건드리지 않는다
```

⚠ **보존은 "지우지 않는다" 는 관찰이고 보장은 아니다.** BL31·U-Boot 버전이
바뀌거나 DDR 재학습이 일어나면 달라질 수 있다. 그래서 OTA 는 매직 + CRC32
를 반드시 검사하고, 어긋나면 **기존 `zephyr.bin` 을 그대로 부팅**해야 한다.
SD 의 이미지가 황금 사본으로 남는 구조라 벽돌이 되지 않는다.

### 버전 스탬프가 맞았다 — 규칙 7-b 의 효과

`build f8f3c15c8585` = HEAD. 직전 왕복에서 한 커밋 뒤처진 것을 보고
체크리스트에 "커밋한 뒤 재빌드했는가" 를 넣었고, 바로 효과가 났다.

## ✅ OTA 1단계 — CDC-ACM 두 개 (2026-11-06)

빌드 `908c1bb` (진단 빌드, 171368 B). **호스트에 포트가 둘 생겼다.**

```
/dev/cu.usbmodem213101   인터페이스 0(CDC control) + 1(CDC data)  ← 셸
/dev/cu.usbmodem213103   인터페이스 2(CDC control) + 3(CDC data)  ← OTA 데이터
```

```
bInterfaceNumber 0: Class 2  SubClass 2   (CDC Communications / ACM)
bInterfaceNumber 1: Class 10 SubClass 0   (CDC Data)
bInterfaceNumber 2: Class 2  SubClass 2
bInterfaceNumber 3: Class 10 SubClass 0
```

### 1차 시도는 실패했고, 그 실패가 원인을 가리켰다

DT 에 `cdc_acm_uart1` 노드만 추가한 빌드(`367cb6c`)에서는 **포트가 하나뿐**
이었다. 그런데 `moru info` 는 `OTA 포트 cdc_acm_uart1 (준비됨)` 을 찍었다 —
**드라이버 인스턴스는 살아 있는데 USB 디스크립터에는 없다**는 뜻이다.
`ioreg` 로 인터페이스를 세어 2개(CDC 하나)임을 확인하고 Zephyr 쪽 문제로
좁혔다.

원인: `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` 의 자동 경로가 인스턴스를
**하나만** 등록한다 — `subsys/usb/device_next/app/cdc_acm_serial.c:65` 의
`usbd_register_class(..., "cdc_acm_0", ...)`, 함수 이름도
`register_cdc_acm_0` 다. 그 Kconfig help 의 "다른 USB 기능이 필요 없는 앱의
기본 시리얼 백엔드 전용" 이 정확히 이 뜻이었다.

### 해결 — 앱이 직접 구성한다

`src/moru_usbd.c` 가 usbd 컨텍스트·디스크립터·설정을 만들고
**`usbd_register_all_classes()`**(`usbd.h:892`)로 사용 가능한 인스턴스를
전부 등록한다. 이름을 하나씩 대지 않으므로 CDC 를 더 늘려도 코드를 고치지
않는다. VID·PID·문자열은 이 파일에서 정한다 —
`CDC_ACM_SERIAL_PID`·`PRODUCT_STRING` 은 `INITIALIZE_AT_BOOT` 하위 심볼이라
그것을 끄면 사라진다.

`usbd_device_set_code_triple(..., USB_BCC_MISCELLANEOUS, 0x02, 0x01)` 은
유지했다. 복합 장치(IAD)임을 알리는 코드 트리플이고, 없으면 호스트가 두
번째 CDC 를 잡지 못한다.

### 안전망이 제 역할을 했다

USB 초기화를 전면 교체하는 변경이라 깨지면 USB 셸을 잃는다. 그래서 **진단
빌드(`diag-uart.overlay`, 셸이 UART2)로 먼저 구웠다.** 이번엔 한 번에
성공했지만, 실패했다면 UART 셸에서 `moru_usbd` 의 단계별 실패 로그
(`설정 추가 실패` / `클래스 등록 실패` / `코드 트리플 설정 실패` /
`usbd_init 실패`)를 바로 읽을 수 있었다.

### 빌드 전 심볼 확인이 또 값을 했다

USB main 빌드 크기가 이전과 같은 167264 B 여서 설정 누락을 의심했는데,
`nm` 으로 우리 심볼 7개(`moru_uds`·`moru_add_speed`·`cdc_acm_1` 등)와
자동 경로 심볼 0개를 확인했다. **오늘 세 번째 "크기가 같아서 의심했다가
심볼로 확인" 사례다** — 정렬 패딩이 흡수한다.

## ⚠ OTA 2단계 — 수신이 0 바이트 (미해결, 2026-11-06)

빌드 `7d057f7`. `moru ota begin 171360 b277da07` 뒤 호스트에서 부어 넣었으나
**`moru ota status` 가 계속 `0 / 171360`** 이다. 호스트의 `write` 도 블록된다.

### 내가 가를 수 없게 만들었다

수정 때 **두 변수를 동시에** 바꿨다:
1. `USBD_CDC_ACM_LOG_LEVEL` INF → WRN (수신 바이트 hexdump 제거)
2. 전용 폴링 스레드(`uart_poll_in`) → 콜백(`uart_fifo_read`) 전환

그래서 0 바이트의 원인이 ②인지, 아니면 ① 전에도 0 이었는지(그때 `status` 를
확인하지 않았다) **가릴 수 없다.** 규칙 2("묶어야 할 때는 실패 시 어떻게
가를지 먼저 적는다")를 어긴 결과다.

### 기각된 가설 — EP 부족

`ep1 XferComplete with no queued buffer` 가 반복돼 "두 CDC 가 물리 EP 0~9 를
쓰는데 `num-bidir-endpoints=8` 이라 부족하다" 고 의심했다. **코드로 기각.**
`udc_dwc3.c:83` 이 `DWC3_NUM_PHYS_EPS = 2 * num_bidir_endpoints` = **16** 이다.
phys 9 는 범위 안이다.

그리고 그 로그의 `ep1` 은 **물리 EP 1 = EP0 IN**, 즉 제어 엔드포인트다
(`udc_dwc3.c:1451` 근처가 EP0 경로). 데이터 경로가 아니므로, 내가 진단하려고
포트를 반복해서 열고 닫은 부작용일 가능성이 크다.

### 코드로 확인된 제약 (다음 조사의 전제)

| 함수 | 제약 |
|---|---|
| `cdc_acm_fifo_read`(:807) | `check_wq_ctx()` 아니면 `__ASSERT_NO_MSG(false)` |
| `cdc_acm_irq_rx_ready`(:847) | 같음 |
| `cdc_acm_irq_update`(:880) | 같음. `IRQ_RX_ENABLED` **와** 링버퍼 비어있지 않음을 함께 봐야 `rx_ready` 가 선다 |
| `cdc_acm_poll_in`(:980) | 문맥 제약 **없음**. 단 1바이트마다 `rx_fifo_work` 제출 |
| `cdc_acm_irq_rx_enable`(:755) | `IRQ_RX_ENABLED` 설정 + `rx_fifo_work` 제출 |
| `usbd_cdc_acm_enable`(:355) | 클래스 활성화 시 `IRQ_RX_ENABLED` 면 `rx_enable` 재호출. `cb == NULL` 이면 `rx_fifo_work` 를 제출(poll_in 용) |
| `usbd_cdc_acm.c:315` | `LOG_HEXDUMP_INF(buf->data, buf->len, "")` — **수신 바이트를 전부 hexdump** |

마지막 항목은 확실한 결함이고 WRN 으로 내린 것은 유지할 가치가 있다.
Zephyr 공식 `overlay-usb.conf` 도 `USBD_CDC_ACM_LOG_LEVEL_OFF=y` 를 쓴다.

### 다음 수 — 변수를 하나로

**`poll_in` 스레드로 되돌리고 로그는 WRN 으로 둔다.** 그러면 바뀐 것이
로그 하나이므로 ①의 효과만 본다.
- 받으면 → hexdump 가 병목이었고 `poll_in` 으로 충분하다
- 0 이면 → 수신 경로 자체가 막혀 있다. 그때 계측(콜백 호출 횟수·
  `fifo_read` 반환값)을 `moru ota status` 에 넣어 어디서 끊기는지 본다

⚠ 진단하려고 호스트에서 포트를 반복해서 열고 닫지 않는다. 제어 전송이
폭주해 로그가 오염되고 원인 판단을 흐린다 (위 `ep1` 경고가 그 결과로 보인다).
