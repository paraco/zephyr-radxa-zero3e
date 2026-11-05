# Zephyr on Radxa ZERO 3E (Rockchip RK3566)

Out-of-tree Zephyr 보드 포팅과 **처음부터 작성한 DWC3 USB 디바이스 컨트롤러
드라이버**. 실기에서 USB CDC-ACM 열거까지 동작한다.

```
+-o PiZZa dwc3 console@01100000   <IOUSBHostDevice, active>
   idVendor=0x2FE3  idProduct=0x0004
   USB Serial Number = "4D4257373335..."   (OTP 에서 읽은 실제 칩 ID)
   /dev/cu.usbmodem1101
```

## 동작 확인된 것

| | 상태 | 비고 |
|---|---|---|
| 보드 부팅 | ✅ | U-Boot `go` 로 적재, MMU·GIC·ARM generic timer |
| UART2 콘솔 | ✅ | 115200 (`clock-frequency = 24 MHz`) |
| GPIO (LED) | ✅ | 레지스터 직접 접근 (Zephyr 에 Rockchip GPIO 드라이버 없음) |
| dwc3 코어 | ✅ | `GSNPSID=0x5533` release `0x300a`, HW 엔드포인트 16개 |
| USB2 PHY | ✅ | GRF 로 peripheral 고정 (iddig·bvalid) |
| USB 열거 | ✅ | SET_ADDRESS · GET_DESCRIPTOR · SET_CONFIGURATION |
| 제어 전송 | ✅ | SETUP / DATA / STATUS 3단계·2단계 모두 |
| 벌크 전송 | ✅ | CDC-ACM 콘솔이 USB 로 흐른다 |
| 인터럽트 EP | ✅ | CDC 알림 엔드포인트 설정 |
| 칩 고유 ID | ✅ | OTP 에서 읽어 USB 직렬번호로 |
| High Speed | ✅ | `CONNECTSPD=0`, `Device Speed = 2` |

**미구현**: USB3(combphy 미연결로 HS 상한) · 클럭(CRU) 드라이버 · pinctrl ·
suspend/resume · 일반 엔드포인트의 실사용 검증(설정까지만 확인).

## 준비물

- Radxa ZERO 3E (RK3566)
- microSD 카드 + Radxa 공식 OS 이미지 1회 기록 (U-Boot 와 파티션을 쓴다)
- USB-C 케이블 (보드의 **USB-C 1** = OTG 포트 ↔ 호스트)
- USB-TTL 어댑터 (선택 — UART 콘솔용)
- Zephyr 워크스페이스 + SDK

```
~/zephyrproject       Zephyr v4.4.0 계열
~/zephyr-sdk-1.0.1    aarch64-zephyr-elf 툴체인
~/.zephyr-venv        west 용 venv
```

경로가 다르면 `PIZZA_ZEPHYR_WS` · `PIZZA_ZEPHYR_SDK` · `PIZZA_ZEPHYR_VENV`
환경변수로 바꾼다.

## 빌드

```bash
./build.sh "$(pwd)/apps/usbled"              # 기본 빌드 디렉터리
./build.sh "$(pwd)/apps/usbled" /tmp/mybuild # 직접 지정
```

⚠ **앱 경로는 절대경로로 준다.** `build.sh` 가 `cd $ZEPHYR_WS` 한 뒤 `-s` 로
넘기므로 상대경로는 엉뚱한 곳을 가리킨다.

산출물은 `<빌드디렉터리>/zephyr/zephyr.bin` 이다. SD 카드에 넣기 전에
`sdcard/` 로 복사한다:

```bash
cp /tmp/claude-501/rz3e-build/zephyr/zephyr.bin sdcard/
```

보드 식별자는 **`radxa_zero_3e/rk3568`** 이다. RK3566 은 인트리 rk3568 SoC
정의를 재사용한다 (같은 RK356x 다이 계열, 이번 스코프의 블록은 주소·인터럽트가
동일하다 — Linux `rk356x-base.dtsi` 도 두 칩 공용).

## 보드에 올리기

U-Boot 콘솔은 **1,500,000 baud** 라 흔한 USB-TTL 로는 타이핑할 수 없다. 대신
U-Boot 가 부팅 대상 파티션에서 **`boot.scr` 를 `extlinux.conf` 보다 먼저
찾는다**는 점을 쓴다. 자세한 근거는 [sdcard/README.md](sdcard/README.md).

SD 카드의 **`boot` 파티션(2번, FAT32)** 에 두 파일을 넣는다:

| 파일 | 역할 |
|---|---|
| `boot.scr` | `fatload` 로 `zephyr.bin` 을 `0x40000000` 에 올리고 `go` |
| `zephyr.bin` | 빌드 산출물 |

### macOS

⚠ **macOS 26 은 FAT 파티션을 마운트하지 못한다** (구형 `msdosfs.kext` 의 코드
서명 문제). `mtools` 로 마운트 없이 쓴다:

```bash
brew install mtools

# 카드 번호 확인 — 꽂을 때마다 바뀐다. 'config' + 'boot' 파티션이 있는 디스크다.
diskutil list

sudo sh -c 'cd sdcard && \
  mcopy -i /dev/rdisk4s2 -o boot.scr zephyr.bin ::/ && \
  mdir -i /dev/rdisk4s2 ::/ && \
  diskutil eject /dev/disk4'
```

⚠ 내장 SD 슬롯은 `internal` 로 보고된다 — "external 인지"로 안전을 가릴 수
없다. **크기와 파티션 이름(`config` + `boot`)으로 확인한다.**

### Linux

```bash
sudo mount /dev/sdX2 /mnt && sudo cp sdcard/boot.scr sdcard/zephyr.bin /mnt/ && sudo umount /mnt
```

`boot.cmd` 를 고쳤으면 `boot.scr` 를 다시 만든다:

```bash
cd sdcard && mkimage -A arm64 -O linux -T script -C none -n "PiZZa Zephyr loader" -d boot.cmd boot.scr
```

## 앱

| 앱 | 콘솔 | 용도 |
|---|---|---|
| `apps/usbled` | **USB CDC-ACM** | 드라이버가 자기 USB 로 자기 로그를 보낸다. 끝까지 확인용 |
| `apps/usbuart` | **UART2** | USB 가 조용할 때도 로그가 보인다. 디버깅용 |
| `apps/ledblink` | 없음 | LED 만 깜박인다. "Zephyr 가 도는가" 를 USB·UART 없이 가린다 |

세 앱 모두 LED(2초 켜짐/2초 꺼짐)를 깜박여 **USB·UART 와 무관한 생존 신호**를
준다. 이 보드의 Linux 는 같은 LED 에 `heartbeat`(빠른 두 번 + 쉼)를 걸어 두므로
**빠른 패턴을 쓰면 눈으로 구분되지 않는다** — 느린 네모파를 쓴다.

## UART 콘솔 (선택)

| USB-TTL | 보드 40핀 |
|---|---|
| **RXD** | **8** |
| **TXD** | **10** |
| **GND** | **6** |
| VCC | 연결하지 않음 (보드는 USB-PD 로 급전) |

⚠ **핀 이름은 보드 기준이라 어댑터와 교차한다.** 어댑터 TX→8, RX→10 으로
꽂으면 TX 와 TX 가 맞붙어 영원히 0 바이트다.

⚠ **어댑터는 허브를 거치지 말고 Mac 에 직접 꽂는다.** 허브 2단을 거쳤을 때
1초 주기로 연결/끊김이 반복됐다.

```bash
tio /dev/cu.usbserial-0001 -b 115200 -L --log-file /tmp/rz3e.log --log-strip
```

U-Boot 구간은 1.5 Mbps 라 쓰레기로 보이고, `go` 이후 Zephyr 로그만 또렷하게
읽힌다.

⚠ **로그는 파일로 받아 전체를 집계하라.** 창 단위로 읽으면 빈도를 놓쳐 잘못된
방향으로 간다 (실제로 세 번 그랬다). 그리고 **배너부터 자르면 `init` 로그가
전부 잘린다** — 배너는 `main()` 이 찍고 `init` 은 그보다 먼저다:

```bash
N=$(grep -an "Radxa Zero 3E dwc3 bringup" /tmp/rz3e.log | tail -1 | cut -d: -f1)
sed -n "$((N-160)),\$p" /tmp/rz3e.log      # 배너 앞 160줄을 포함한다
```

## 저장소 구조

```
boards/radxa/radxa_zero_3e/   보드 정의 (DTS, defconfig, Kconfig)
dts/arm64/rockchip/           rk3566-usb.dtsi — dwc3 노드
dts/bindings/usb/             snps,dwc3.yaml
drivers/usb/udc/              udc_dwc3.c + 레지스터 헤더  ← 핵심
drivers/hwinfo/               OTP 에서 칩 고유 ID 읽기
apps/                         usbled · usbuart · ledblink · moru (관리 셸)
sdcard/                       boot.cmd / boot.scr 와 적재 설명
docs/bringup-rules.md         실기 왕복 규율 ← 작업 시작 전에 읽을 것
docs/trm-grounding.md         하드웨어 사실의 출처 등급과 TRM 사정권
docs/hardware-verified.md     실기 검증 기록 (시간순 · 검색해서 쓴다)
tools/board-status.sh         보드·포트 상태 한눈에
build.sh                      빌드 래퍼
```

## 반드시 알아야 할 함정

이 네 가지로 각각 여러 시간을 잃었다. 자세한 경위는
[docs/hardware-verified.md](docs/hardware-verified.md).

⚠ **실기 왕복을 쓰기 전에 [docs/bringup-rules.md](docs/bringup-rules.md) 의
체크리스트를 먼저 본다.** bring-up 에서 반복한 실수 8가지를 규칙으로 바꿔
둔 것이다 — 가장 비쌌던 둘은 "기준선 없이 변경을 쌓은 것"(왕복 4회)과
"한 왕복에 변수를 20개 넣은 것"이었다. 하드웨어 상수를 새로 넣을 때는
[docs/trm-grounding.md](docs/trm-grounding.md) 의 출처 등급을 따른다.

**① `CONFIG_LOG_MODE_IMMEDIATE` 를 쓰면 USB 열거가 실패한다.**
즉시 모드는 로그 한 줄을 그 자리에서 UART 로 밀어내며 스레드를 멈춘다. 115200
에서 리셋 처리 하나가 100 ms 를 넘겨 **USB 규격의 `SET_ADDRESS` 50 ms 예산**을
통째로 먹는다. 호스트가 타임아웃·리셋을 수십 번 반복한다. `LOG_MODE_DEFERRED`
를 쓴다.

**② arm64 에서 1024바이트 기본 스택은 거의 전부 부족하다.**
64비트 레지스터 31개의 문맥 저장만으로도 크다. `CONFIG_UDC_DWC3_STACK_SIZE`,
`CONFIG_USBD_THREAD_STACK_SIZE`, 그리고 직접 만드는 스레드를 4096 으로 올린다.
`CONFIG_STACK_SENTINEL` 과 `CONFIG_THREAD_NAME` 을 켜면 넘침이 "쓰레기 주소
점프" 가 아니라 `FATAL ERROR 2: Stack overflow` + 스레드 이름으로 나온다 —
이 둘이 없어 두 번 헤맸다.

**③ `boot.scr` 에 `usb stop` 을 넣지 마라.**
U-Boot 의 `usb stop` 은 dwc3 장치를 제거하며 클럭 묶음을 해제한다. Zephyr 에
CRU 드라이버가 없어 되살릴 수 없고, 컨트롤러 레지스터가 전부 0 으로 읽혀
`GSNPSID` 검사에서 init 이 중단된다. U-Boot 의 `preboot=usb start` 가 켜 둔
클럭을 그대로 물려받아야 한다.

**④ `CONFIG_FRAME_POINTER` / `CONFIG_EXCEPTION_STACK_TRACE` 는 쓸 수 없다.**
켜면 Zephyr 가 기동조차 못 한다 (LED·콘솔 모두 무음). 역추적 대신 ②의
`STACK_SENTINEL` + `THREAD_NAME` 을 쓴다.

## 출처와 라이선스

드라이버 본체는 **Apache-2.0** (Zephyr 와 동일). 하드웨어 사실의 출처는 각
파일 머리말에 적어 두었다.

| 사실 | 출처 | 라이선스 |
|---|---|---|
| DWC3 레지스터·명령 | Fuchsia `dwc3` 드라이버 | BSD-2 ([LICENSE.fuchsia](drivers/usb/udc/LICENSE.fuchsia)) |
| GPIO v2 레지스터 배치 | FreeBSD `sys/arm64/rockchip/rk_gpio.c` | BSD-2 |
| USB2PHY GRF · CRU · 메모리맵 | Rockchip RK3568 TRM Part1 V1.3 | 벤더 문서 |
| **OTP 컨트롤러 레지스터·절차** | Linux `drivers/nvmem/rockchip-otp.c` | **GPL-2.0** |
| 칩 ID 위치 | Linux `rk356x-base.dtsi` | GPL-2.0 / MIT |

⚠ **OTP 컨트롤러 레지스터는 공개 TRM 양쪽(Part1/Part2)에 없다.** Part2 의 33개
장을 모두 확인했고 OTP 장이 없다. 그래서 레지스터 오프셋·비트 위치·절차만
Linux 에서 **사실로서** 확인하고 코드는 직접 작성했다 — 하드웨어 레지스터
번호는 저작 표현이 아니라 사실이라는 판단이다. 이 판단이 맞지 않다고 보면
`CONFIG_HWINFO_RK3566_OTP=n` 으로 끄거나 자리표 판으로 되돌릴 수 있다
(git 이력에 있다).

## 현재 작업 상태 — 관리 셸 (2026-11-05)

`apps/moru` 가 **UART2 에 `Moru > ` 셸을 띄우는 단계까지 동작한다.** 계획은
`~/.claude/plans/twinkly-doodling-frog.md`, 실기 기록은
[docs/hardware-verified.md](docs/hardware-verified.md) 에 있다.

### 동작 확인된 것

```
[00:00:00.457,000] <inf> hwinfo_rk3566: OTP 칩 ID: 4d4257373335...
[00:00:00.465,000] <inf> usbd_cdc_acm: Configuration enabled
Moru > help
Available commands:
  app · clear · device · help · history · kernel · rem · resize · retval · shell
```

UART2 셸 + USB CDC-ACM 열거(HS) + OTP 직렬번호가 동시에 살아 있고 `<err>` 0줄.

### ⛔ 이 보드에서 쓸 수 없는 것

**`CONFIG_INIT_STACKS=y` 를 켜면 보드가 완전히 무음이 된다** (실기 3회 확인).
U-Boot 는 정상이고 `go` 까지 가지만 Zephyr 가 한 글자도 못 찍는다.
기전은 아직 설명되지 않았다 — 자세한 내용과 수치는
`docs/hardware-verified.md`.

그래서 지금은 **`kernel thread list` 와 `kernel thread stacks` 를 쓸 수 없다**
(둘 다 `INIT_STACKS` 를 요구한다). `kernel thread suspend`/`resume`/`kill` 과
`uptime`/`cycles`/`version` 은 쓴다.

**다만 이 포기는 조건부다.** EL2 데이터 캐시 가설이 서 있고, 그것이 맞으면
`CONFIG_ARM64_DCACHE_ALL_OPS=y` + `CONFIG_ARM64_BOOT_DISABLE_DCACHE=y` 로
두 명령을 되찾는다 — **실기 1회**로 가린다. 근거와 실험 설계는
[docs/trm-grounding.md](docs/trm-grounding.md) §7.

⚠ `CONFIG_SHELL=y` 는 `KERNEL_SHELL` 을 통해 `INIT_STACKS` 를 **imply** 한다.
그래서 `apps/moru/prj.conf` 가 명시적으로 `CONFIG_INIT_STACKS=n` 을 쓴다.
**이 줄을 지우면 보드가 부팅하지 않는다.**

### 다음에 이어서 할 일

```bash
./build.sh "$(pwd)/apps/moru" && cp /tmp/claude-501/rz3e-build/zephyr/zephyr.bin sdcard/
```

그 뒤 SD 카드 복사(위 "보드에 올리기") → 전원. `tio` 는 **115200**
(U-Boot 구간은 1.5 Mbps 라 쓰레기로 보이고, 그 뒤 Zephyr 출력이 읽힌다).

남은 묶음 — 각각 실기 1회:

| 묶음 | 내용 | 위험 |
|---|---|---|
| B1 | Linux 형식 부팅 메시지 · `moru_boot.c` 되살리기 · MoruLab 제품 문자열/PID | 낮음 |
| B2 | `moru info` · `VERSION` · 셸 모듈(`hwinfo`·`log`·`heap`) | 낮음 |
| B3 | `moru usb` · 드라이버 진단 API·카운터 | 중간 — 열거 회귀 시험 필수 |
| B4 | `moru reboot` (셸에서 직접 SMC) | 낮음 — BL31 확인됨, Kconfig 변경 0 |

B4 의 근거: U-Boot 로그에서 `BL31 v2.3` 이 EL3 에서 GICv3 를 초기화하고
런타임 서비스를 올리는 것을 확인했다. 단 **`CONFIG_PM_CPU_OPS` 는 켜지 말 것** —
`psci_init` 이 `PRE_KERNEL_1` 에서 SMC 를 쏘고 그건 콘솔보다 먼저라, BL31 이
응답하지 않으면 원인 구분이 불가능한 무음이 된다.

### 측정 방법 (비싸게 배운 것)

- **기준점을 먼저 확인한다.** 검증된 `apps/usbuart` 를 같은 카드로 구워
  LED 가 깜박이는지 본다. 이것 하나로 보드·카드·배선·전원·부팅 사슬이
  한꺼번에 검증된다. 이걸 건너뛰어 왕복 네 번을 낭비했다.
- **`prj.conf` 의 줄 수는 실제 변경량이 아니다.** 2줄을 더했는데 `.config`
  차이가 70개였다. 반드시 비교한다:
  `diff <(grep ^CONFIG_ A/.config|sort) <(grep ^CONFIG_ B/.config|sort)`
- **U-Boot 를 읽으려면 1.5 Mbps.** 그래야 `fatload` 결과와 `go` 주소가 보인다.
- `tio --log-file` 은 버퍼링돼 실시간이 아니다. 판단은 터미널 화면으로.
  `--log-strip` 은 쓰레기를 버려서 로그가 비어 보인다.
- 보율이 틀린 구간의 쓰레기에서 문자열을 grep 하면 안 된다 (거짓 양성·음성).
- `diskutil list` 에 카드가 없는 것은 복사 성공의 증거가 아니다. `mdir` 을 본다.
- `sudo -v` 를 먼저 돌린다. 비밀번호 오류로 `&&` 사슬이 끊기면 조용히 실패한다.
- **진단 장치가 피검체를 죽인 사례가 두 번 있었다** — `PRE_KERNEL_1` 에서
  `device_map()` 호출, LED 스레드 스택 1024. 계측을 추가할 때도 같은 규칙을
  지킨다.

---

## 다음에 할 만한 것

- 일반 엔드포인트 실사용 검증 (CDC 벌크는 동작하지만 대량 전송·ZLP·짧은 패킷
  경계는 아직 본격적으로 시험하지 않았다)
- CRU 클럭 드라이버 — 지금은 필요한 게이트를 레지스터로 직접 쓴다
- pinctrl 드라이버
- `DWC3_NUM_PHYS_EPS` 와 DT `num-bidir-endpoints` 불일치 정리
  (HW=16, DT=8 로 경고가 난다)
- USB suspend/resume (`DWC3_DEVT_U3L2L1_SUSPEND` / `WAKEUP`) 미처리
