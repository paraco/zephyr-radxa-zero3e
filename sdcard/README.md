# SD 카드에 넣는 파일 — U-Boot 콘솔 없이 Zephyr 적재

Radxa Zero 3E 의 **`boot` 파티션(2번, FAT32)** 에 넣는다. 그 파티션은 Radxa 공식
이미지에서 **비어 있다** (루트에 볼륨 레이블뿐) — 커널과 `extlinux.conf` 는 ext4
rootfs 안에 있다. 그래서 이 FAT 파티션을 그대로 쓸 수 있다.

## 왜 이 방법인가

U-Boot 콘솔은 1,500,000 baud 인데 손에 있는 CP2102 가 최대 1 Mbps 라 타이핑을 할
수 없다. 대신 U-Boot 가 **부팅 대상 파티션마다 `boot.scr` 를 `extlinux.conf` 보다
먼저 찾는다**는 점을 쓴다.

U-Boot 바이너리에서 직접 확인한 사실 (이미지의 sector 16384 FIT 를 추출해 문자열 확인):

```
bootcmd=bootflow scan; run distro_bootcmd; ...
boot_targets=mmc1 mmc0 nvme scsi usb pxe dhcp spi
boot.scr   boot.scr.uimg   bootmeth_script
fatload   ext4load   bootm   booti   source
"start application at address 'addr'"      <- go 명령이 있다
```

`dcache` / `icache` 명령은 **없다** (`CONFIG_CMD_CACHE` 미포함). 즉 캐시를 끄고
점프할 수 없다 — 이것이 남은 불확실성이다.

## 파일

| 파일 | 내용 |
|---|---|
| `boot.cmd` | 스크립트 원본 (사람이 읽는 것) |
| `boot.scr` | `mkimage -A arm64 -O linux -T script` 로 헤더를 붙인 것. U-Boot 는 이것만 읽는다 |
| `zephyr.bin` | 적재할 Zephyr 이미지 |

`boot.cmd` 를 고치면 반드시 다시 만든다:

```bash
mkimage -A arm64 -O linux -T script -C none -n "PiZZa Zephyr loader" \
        -d boot.cmd boot.scr
```

## 넣는 법 — macOS

macOS 26 은 이 파티션을 마운트하지 못한다. 구형 `msdosfs.kext` 가 코드 서명 문제로
로드되지 않고(`Bad code signature`) `diskutil mount` 도 거부한다. 그래서 **`mtools`
로 마운트 없이** 쓴다.

```bash
brew install mtools
sudo mdir  -i /dev/rdiskNs2 ::/                 # 내용 확인
sudo mcopy -i /dev/rdiskNs2 -o boot.scr ::/     # 넣기
sudo mcopy -i /dev/rdiskNs2 -o zephyr.bin ::/
```

`N` 은 매번 바뀐다. `diskutil list` 로 `config`(16.8 MB) + `boot`(314.6 MB) 파티션이
있는 디스크를 찾아 확인한다.

## 되돌리기

```bash
sudo mdel -i /dev/rdiskNs2 ::/boot.scr
```

`boot.scr` 만 지우면 U-Boot 가 평소대로 `extlinux.conf` 로 Linux 를 띄운다.

**스크립트 자체에도 안전장치가 있다** — `zephyr.bin` 을 읽지 못하면 `go` 를 부르지
않고 그냥 끝나므로, U-Boot 가 다음 부팅 항목으로 넘어가 Linux 가 뜬다.

## 적재 주소

| 값 | 근거 |
|---|---|
| 적재 `0x40000000` | DTS 의 `sram0`. 라이브 DT 로 확인한 DRAM(`0x00200000`~`0x80000000`) 한가운데이고 U-Boot 재배치 영역과 겹치지 않는다 |
| 진입 `0x40001084` | 빌드 산출물의 entry point. 바뀌면 `boot.cmd` 도 고친다 |

## 남은 불확실성

`go` 는 캐시·MMU 를 U-Boot 상태 그대로 두고 점프한다. Zephyr arm64 가 그 상태에서
깨끗이 시작하는지는 실기에서만 알 수 있고, 이 U-Boot 에는 캐시를 끌 명령이 없다.
실패하면 `bootm` 의 standalone 경로(U-Boot 가 핸드오프를 더 제대로 한다)를 검토한다.

Zephyr 콘솔은 **115200** 이므로 (DTS 에서 바꿔 뒀다) 올라가면 지금 CP2102 로 로그를
읽을 수 있다.

## boot.scr 복구 — 되돌리는 법

`boot.scr` 은 **OTA 로 바꿀 수 없다**(OTA 는 `zephyr.bin` 만 쓴다). 그래서
`boot.scr` 이 잘못되면 **카드 리더가 유일한 복구 수단**이다. 바꾸기 전에
무엇이 되돌릴 대상인지 분명히 해 둔다.

| 파일 | 뜻 |
|---|---|
| `boot.scr` | 지금 쓰려는 판 |
| `boot.scr.known-good` | **직전에 실기에서 부팅이 확인된 판.** 새 것이 안 뜨면 이것을 카드에 쓴다 |

```bash
# 되돌리기
cd ~/Projects/PiZZa/rk3566/sdcard && \
  sudo mcopy -i /dev/rdisk4s2 -o boot.scr.known-good ::/boot.scr && \
  diskutil eject /dev/disk4
```

**어떤 과거 판이든 다시 만들 수 있다** — `boot.cmd` 가 git 에 있다:

```bash
git log --oneline -- sdcard/boot.cmd
git show <커밋>:sdcard/boot.cmd > /tmp/boot.cmd
mkimage -A arm64 -O linux -T script -C none -n "PiZZa Zephyr loader" \
  -d /tmp/boot.cmd /tmp/boot.scr
```

⚠ `boot.scr.known-good` 은 `.gitignore` 에 있어 **git 이 지켜 주지 않는다.**
덮기 전에 무엇인지 확인한다 (2026-11-07 에 확인 없이 덮은 적이 있다 —
`boot.cmd` 가 버전 관리되어 손실은 없었지만 규율로 적어 둔다).
