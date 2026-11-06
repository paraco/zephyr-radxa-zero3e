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

## 콘솔 원칙 — main 은 USB, 안 되면 UART 1개

**2026-11-06 사용자 확정.** `apps/moru` 의 콘솔·셸은 **USB CDC-ACM** 이다.
USB 가 동작하지 않으면 **UART 1개만** 써서 들여다본다.

| 모드 | chosen | 빌드 | 보이는 것 |
|---|---|---|---|
| **main (USB)** | `app.overlay` → `cdc_acm_uart0` | `./build.sh "$PWD/apps/moru"` | `/dev/cu.usbmodem*` 에 셸·로그 전부 |
| **진단 (UART)** | `diag-uart.overlay` → `uart2` | `./build.sh "$PWD/apps/moru" /tmp/claude-501/rz3e-diag "$PWD/apps/moru/diag-uart.overlay"` | UART2 115200 에 셸·로그 전부 |

왜 바뀌었나 — 11-05 에는 "둘 다 UART2" 였다. USB 열거가 안정되고(열거 성공·
OTP 직렬번호·`<err>` 0줄), **USB 가 안 보일 때 원인을 가릴 관측 수단이
생겼기 때문**이다. 그 수단이 아래 두 줄이다.

**⚠ USB main 에서도 초기 마커는 UART2 로 나온다.** `src/moru_mark.c` 가
`chosen` 을 따르지 않고 `uart2` 에 직접 찍는다 — USB 열거 전에는 CDC 로
한 글자도 안 나가므로 그래야 관측이 가능하다. 보율은 ns16550 드라이버가
DT 의 `current-speed`(115200)를 스스로 적용하므로 콘솔로 쓰이지 않아도
115200 으로 나온다.

```
[2][<S][S>][<U][U>][M]    ← USB 가 무음이어도 UART 어댑터만 꽂으면 보인다
```

**⚠ 열거 전 로그는 터미널을 열 때까지 보이지 않는다.** `LOG_MODE_DEFERRED`
라 버퍼에 쌓이고 백엔드가 붙은 뒤 쏟아진다(`LOG_BUFFER_SIZE=16384`).
CDC 의 `poll_out` 은 버퍼가 차면 **버린다**(블록하지 않는다) — 터미널을
열고 엔터를 한 번 치면 프롬프트가 나온다.

USB 가 무음일 때 가르는 순서는 `apps/moru/prj.conf` 맨 끝에 적어 두었다.
카드를 진단 빌드로 바꾸는 것은 **마지막 수단**이다(①~④를 먼저 본다).

진단 빌드로 교체:

```bash
cd /Users/paraco/Projects/PiZZa/rk3566/sdcard && sudo mcopy -i /dev/rdisk4s2 -o zephyr-diag-uart.bin ::/zephyr.bin && sudo mdir -i /dev/rdisk4s2 ::/ && diskutil eject /dev/disk4
```

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
docs/roadmap.md               남은 작업 계획 (2026-11-06 전면 점검) ← 다음에 할 일
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

**④ 역추적은 쓸 수 있다 — 과거 기록이 틀렸다 (2026-11-06 정정).**
전에는 `CONFIG_FRAME_POINTER`/`EXCEPTION_STACK_TRACE` 를 켜면 기동조차
못 했다. **그 증상은 `INIT_STACKS` 와 똑같았고 원인도 같았다 — EL2 데이터
캐시다**(위 절). 캐시 두 설정이 켜진 지금 구성에서는 그냥 동작한다.

`CONFIG_FRAME_POINTER=y` 한 줄이 체인 넷을 켠다
(`arch/arm64/core/Kconfig:210` → `arch/Kconfig:489` →
`subsys/debug/Kconfig:317`), 덤으로 `kernel thread unwind` 명령까지:

```
Moru > kernel thread unwind
Unwinding 0x40037340 shell_uart
ra: 0x40005b28   ← addr2line 으로 cmd_kernel_thread_unwind unwind.c:50
...
```

```bash
~/zephyr-sdk-1.0.1/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-addr2line -e /tmp/claude-501/rz3e-build/zephyr/zephyr.elf -f -C -s 0x40005b28
```

**EL2 캐시 하나가 두 기능(`INIT_STACKS`·역추적)을 막고 있었다.** 그래서
위 절의 캐시 두 줄은 이 보드에서 선택이 아니다.

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

## 현재 작업 상태 — 관리 셸 + OTA (2026-11-06)

`apps/moru` 가 **USB CDC-ACM 두 포트로 셸과 OTA 를 모두 제공한다.**
계획과 남은 일은 [docs/roadmap.md](docs/roadmap.md), 실기 기록은
[docs/hardware-verified.md](docs/hardware-verified.md) 에 있다.

### 쓸 수 있는 것

```
/dev/cu.usbmodem…1   셸 + 로그 (chosen console/shell = cdc_acm_uart0)
/dev/cu.usbmodem…3   OTA 데이터 (cdc_acm_uart1)
/dev/cu.usbserial…   UART2 115200 — 초기 부팅 마커 전용 (USB 가 죽었을 때의 창)
```

| 명령 | 하는 일 |
|---|---|
| `moru info` | 버전·해시·보드·CPU·메모리·칩ID·콘솔·부트로더 시간·가동 |
| `moru usb` | dwc3 레지스터 + **벌크 mps** + 이벤트 카운터 9개 |
| `moru reboot [probe]` | PSCI 재부팅 (`probe` 는 BL31 응답만 확인) |
| `moru ota begin\|status\|verify\|apply\|abort` | **카드를 빼지 않는 펌웨어 교체** |
| `moru ota mark\|check` | 재부팅 뒤 DRAM 보존 확인 (진단용) |
| 내장 | `kernel thread list\|stacks\|unwind`, `kernel heap`, `device list`, `hwinfo devid`, `log status`, `app version` |

### 펌웨어를 바꾸는 법 — 카드를 빼지 않는다

```bash
./build.sh "$(pwd)/apps/moru"
python3 tools/ota_test.py /tmp/claude-501/rz3e-build/zephyr/zephyr.bin --chunk 4096
```

그다음 셸에서 `moru ota apply`. 175 KB 가 **0.37초**에 전송되고, `boot.scr` 이
`fatwrite` 로 SD 의 `zephyr.bin` 을 교체하면서 **`zephyr.bin.old` 로 백업**한다.

⚠ 카드를 빼야 하는 경우는 셋뿐이다 — `boot.cmd`/`boot.scr` 을 바꿀 때,
롤백할 때(`.old` → `zephyr.bin`), 그리고 부팅이 완전히 깨졌을 때.

### 이 보드의 필수 설정 (지우면 부팅하지 않는다)

```conf
CONFIG_ARM64_DCACHE_ALL_OPS=y
CONFIG_ARM64_BOOT_DISABLE_DCACHE=y
```

U-Boot 가 EL2 에서 `go` 하고 캐시를 정리하지 않기 때문이다. 자세한 것은
위 §"이 보드의 필수 설정 — EL2 데이터 캐시".

### 2026-11-06 에 해결한 것

| | |
|---|---|
| `INIT_STACKS` 무음 | **EL2 데이터 캐시**였다. `INIT_STACKS` 는 방아쇠였을 뿐 |
| 역추적 | 같은 원인. `kernel thread unwind` + `addr2line` 동작 |
| USB main 전환 | 셸·로그가 CDC, UART2 는 마커 창 |
| CDC-ACM 두 개 | `INITIALIZE_AT_BOOT` 이 하나만 등록한다 → `moru_usbd.c` 가 직접 구성 |
| OTA 1·2·3단계 + 롤백 | 양방향 검증 |
| HS 속도 확정 | 벌크 mps 64 → **512** (`UDC_EVT_RESET` 을 `CONNECTION_DONE` 으로) |
| 로그 잘림 | 두 단계(CDC `poll_out` + 셸 로그 큐)에서 버려지고 있었다. **드롭 0** |

## 다음에 할 만한 것

- ~~일반 엔드포인트 실사용 검증~~ — **✅ 2026-11-06.** OTA 로 175 KB 를
  수십 회 전송했고, mps(512) 배수와 ±1 경계를 CRC 로 검증했다
  (512·513·1023·1024·2048·175104). ZLP 자체는 CDC-ACM 에서 호스트
  드라이버가 다루므로 이 경로로는 직접 드러나지 않는다
- ~~`DWC3_NUM_PHYS_EPS` 와 DT `num-bidir-endpoints` 불일치~~ —
  **✅ 2026-11-06.** 불일치가 아니었다. HW 16(물리)과 DT 8(양방향 = 물리
  16)을 단위 없이 비교한 경고였다
- **CRU 클럭 드라이버** — 지금은 필요한 게이트를 레지스터로 직접 쓴다
- **pinctrl 드라이버**
- **USB suspend/resume** (`DWC3_DEVT_U3L2L1_SUSPEND` / `WAKEUP`) 미처리
- **Zephyr 상류 보고** — 문안이 [docs/upstream-report.md](docs/upstream-report.md)
  에 있다. 사용자가 GitHub 에 올리면 끝난다
