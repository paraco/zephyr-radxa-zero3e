# 남은 작업 계획 — 2026-11-06 전면 점검

원래 계획(`~/.claude/plans/twinkly-doodling-frog.md`)은 **UART main 기준**으로
세워졌다. 그 뒤 전제가 셋 바뀌어 전면 점검했다. 이 문서가 그 결과이고,
원래 계획과 어긋나면 **이 문서가 우선한다.**

## 바뀐 전제 셋

| | 그때 | 지금 | 영향 |
|---|---|---|---|
| 1 | 콘솔·셸이 UART2 | **USB CDC-ACM** (`chosen`), UART2 는 마커 전용 | 부팅 메시지가 "순차"가 아니라 터미널 열 때 쏟아진다. **드라이버 회귀 = 셸 상실** |
| 2 | `INIT_STACKS` 를 포기 | **복구** (캐시 두 설정) | `kernel thread list`·`stacks` 사용 가능. 스택 결정을 실측으로 한다 |
| 3 | 기전 미설명 | **EL2 dcache 로 설명됨** | `FRAME_POINTER` 도 재시험 가치가 생겼다 |

## 이미 반영된 것 (다시 할 필요 없음)

- `CONFIG_USBD_SHELL=n` (보정 4) · `DEVICE_SHELL`·`KERNEL_SHELL`·
  `APP_VERSION_SHELL`=y · `STACK_SENTINEL`·`THREAD_NAME`=y
- `VERSION` 파일 → `APP_VERSION_STRING` + `app version` 명령 (보정 5)
- `psci` DT 노드는 인트리에 이미 있다 (보정 1) — DTS 작업 0
- 로그 타임스탬프는 1 ms 해상도이고 랩어라운드 없음 (보정 3 정정분)

## 낡아서 고쳐야 하는 것

| 파일 | 문제 | 조치 |
|---|---|---|
| `src/moru_boot.c:143-145` | `LOG_INF("Console: %s %u 8N1, shell on same port")` — CDC 에는 보율이 없다 | B1 에서 "USB CDC-ACM (`cdc_acm_uart0`), UART2 는 초기 마커" 로 |
| `src/moru_stage.c` | `PRE_KERNEL_1` 에서 `device_map()` — **불법, 보드를 죽인다** | **삭제한다.** 남기면 다음 사람이 빌드에 넣고 당한다 |
| `CMakeLists.txt:31` | 두 파일을 주석으로 묶어 둠 | `moru_boot.c` 만 되살린다 |
| `prj.conf` PID·제품 문자열 | `0x0004` / `"PiZZa dwc3 bringup"` | B1 에서 `0x0005` / `"MoruLab Zephyr RTOS"`. **USB main 이라 호스트에 보이는 이름이다** |

## 위험 재평가 — B3 가 올라갔다

**B3(드라이버 편집)의 위험이 "중간"에서 "높음"으로 올라갔다.**
전에는 드라이버가 깨져도 UART 셸이 살아 있었다. 지금은 셸이 USB 에 있으므로
**드라이버 회귀 = 셸 상실**이다. 남는 것은 UART2 의 동기 마커뿐이다.

→ **B3 는 진단 빌드(`diag-uart.overlay`)로 먼저 시험한다.** UART 셸에서
`moru usb` 를 검증한 뒤 USB main 빌드로 올린다. 실기 왕복이 1회 늘지만,
깨진 드라이버로 USB 셸을 잃으면 그 왕복을 어차피 쓴다.

## 순서

| | 묶음 | 왕복 | 위험 | 종료 조건 |
|---|---|---|---|---|
| ~~1~~ | ~~`FRAME_POINTER=y` 재시험~~ | — | — | **✅ 2026-11-06 완료 — 역추적 복구, `kernel thread unwind` 동작** |
| **2** | **B1** 부팅 메시지 · `moru_boot.c` 되살리기 · PID 0x0005 · 제품 문자열 | 1 | 낮 | `[    0.123456]` 형식 · `shell stats show` 의 `Lost logs: 0` · 호스트에 `MoruLab Zephyr RTOS` · 열거 유지 |
| ~~3~~ | ~~**B2** `moru info` · 커널 상태 모듈 **+ `moru reboot`**~~ | — | — | **✅ 2026-11-06 완료 — 전부 통과. `moru reboot probe` 가 PSCI 1.1 응답 확인** |
| **4** | **B3-a** `moru usb` 를 **진단 빌드**로 검증 | 1 | 높 | UART 셸에서 `moru usb status` 가 `ioreg` 와 일치 · `<err>` 0줄 |
| **5** | **B3-b** 같은 코드를 USB main 으로 | 1 | 중 | 열거 유지 · USB 셸에서 `moru usb` 동작 |
| **6** | **B4** `moru reboot` + `probe` | 1 | 중 | `probe` 가 BL31 응답 확인 · `reboot` 이 실제로 재부팅 |
| — | Zephyr 상류 보고 (`ARM64_BOOT_DISABLE_DCACHE` 기본값) | 0 | — | QEMU 로 일반 문제임을 확인한 뒤 |

## 묶음별로 바뀐 내용

### 1 — `FRAME_POINTER` 재시험 (계획에 없던 것)

`INIT_STACKS` 와 **증상이 똑같았다**(무음). 같은 EL2 캐시 원인일 수 있다.
맞으면 역추적을 되찾아 **2~6 전부의 디버깅 비용이 내려간다.** 그래서 먼저 둔다.

**`CONFIG_FRAME_POINTER=y` 한 줄이 전부다.** 체인이 자동으로 따라온다
(`arch/arm64/core/Kconfig:210`, `CPU_CORTEX_A` 안):

```
ARCH_HAS_STACKWALK  default y, depends on FRAME_POINTER, imply THREAD_STACK_INFO
  → ARCH_STACKWALK        default y, depends on ARCH_HAS_STACKWALK (arch/Kconfig:489)
    → EXCEPTION_STACK_TRACE  default y, depends on ARCH_STACKWALK (subsys/debug/Kconfig:317)
```

그래서 `EXCEPTION_STACK_TRACE` 를 따로 시험할 수 없다 — `FRAME_POINTER` 없이는
**선택 자체가 보이지 않는다**(지금 `.config` 에 그 세 심볼이 아예 없는 이유다).
README 함정 ④가 "둘 다"로 적은 것은 사실상 하나였다.

전제조건은 이미 넣었다 — `SHELL_STACK_SIZE=4096`(실측 71 % 근거). 프레임
포인터는 스택을 더 쓰므로, 3072 에서 시험하면 "기동 실패"와 "셸 스택 넘침"을
가릴 수 없었다.

**역추적을 일부러 깨뜨려 확인하지 않는다** — 고장 코드를 넣어야 하고 그것이
`moru_stage.c` 가 태어난 패턴이다(규칙 5). 그럴 필요도 없어졌다:

`CONFIG_KERNEL_THREAD_SHELL_UNWIND` 가 체인에 함께 켜져
**`kernel thread unwind [<id16진수>]`** 명령이 붙는다(`unwind.c:53`, 인자를
빼면 현재 스레드). 살아 있는 스레드의 콜 스택을 찍으므로 안전하게 검증된다.

빌드 후 확인한 것 (실기 0회):
- `.config` diff 9줄 — `FRAME_POINTER`·`ARCH_HAS_STACKWALK`·`ARCH_STACKWALK`·
  `ARCH_STACKWALK_MAX_FRAMES=8`·`EXCEPTION_STACK_TRACE`·
  `KERNEL_THREAD_SHELL_UNWIND`·`OVERRIDE_FRAME_POINTER_DEFAULT`. 번짐 없음
- `nm` 에 `arch_stack_walk`(T)와 `cmd_kernel_thread_unwind` 존재
- 크기 146048 → 154240 B (+8 KiB, unwinder 코드)

주소를 함수 이름으로 바꾸려면 `aarch64-zephyr-elf-addr2line -e <elf> -f -C <주소>`.

단독 왕복으로 돌린다(기동 실패가 입증된 설정이라 묶지 않는다). 실패해도
지금 구성은 그대로 남는다.

### 2 — B1

계획대로 가되 셋이 달라진다:
- **`BOOT_BANNER=n`** — 지금 `y` 라 `*** Booting Zephyr OS build ... ***` 가
  나온다. 우리 배너로 대체한다
- **Linux 타임스탬프의 의미가 줄었다** — CDC 백엔드는 터미널 열 때 붙으므로
  부팅 로그가 한꺼번에 쏟아진다. 그래도 `[    0.123456]` 로 **순서와 소요
  시간**을 읽을 수 있어 가치는 남는다
- **로그 버퍼를 종료 조건으로 확인한다** — 부팅 메시지가 ~15줄 늘어 총 60줄
  남짓이 터미널 열기 전까지 쌓인다. `shell stats show` 의 `Lost logs: 0` 을
  본다. 넘치면 `LOG_BUFFER_SIZE` 를 올린다.

  ⚠ **기준선을 먼저 받아야 한다.** 11-06 에 본 `Lost logs: 0` 은 **UART 셸**
  빌드에서 나온 값이다. 그때는 백엔드가 `POST_KERNEL 90` 에 바로 붙어 버퍼가
  0.1 초 만에 비워졌다. USB main 에서는 사람이 터미널을 열 때까지(수십 초)
  쌓이므로 **조건이 다르고, 그 0 은 지금 구성의 증거가 아니다**(규칙 6).
  USB 셸에서 `shell stats show` 를 한 번 쳐서 현재 기준선을 기록한 뒤 B1 로
  간다. 이 수치 없이 가면 "부팅 메시지가 안 나온다"가 버퍼 넘침인지
  `moru_boot.c` 문제인지 가를 수 없다

### 3 — B2

계획의 보정 3 그대로 — `kernel heap` 은 `HEAP_MEM_POOL_SIZE > 0` 이 없으면
**컴파일조차 안 된다**(`heap.c:8`). 현재 `0` 이다.

```conf
CONFIG_HEAP_MEM_POOL_SIZE=8192
CONFIG_SYS_HEAP_RUNTIME_STATS=y
CONFIG_HWINFO_SHELL=y
CONFIG_LOG_CMDS=y
```

`moru info` 의 콘솔 항목은 "USB CDC-ACM" 으로 쓴다. `kernel thread list`·
`stacks` 가 살아 있으니 중복 구현하지 않는다.

`HWINFO_SHELL` 은 링크 걱정이 없다 — `drivers/hwinfo/CMakeLists.txt:8` 이
`CONFIG_HWINFO` 면 `hwinfo_weak_impl.c` 를 **항상** 링크한다. 우리가 구현하지
않은 `hwinfo reset_cause` 는 `-ENOSYS` 를 돌려주고 끝난다(확인 완료).

### 4·5 — B3 을 둘로 쪼갠다

위 "위험 재평가" 참고. 드라이버 편집을 USB 셸에 의존한 채로 하지 않는다.

진단 API 설계(`udc_dwc3_diag.h` 스냅샷 구조체 + `udc_dwc3_diag_get()` +
`struct udc_dwc3_stats` 카운터)는 계획대로 유효하다. 카운터의
`cmd_timeout`·`trb_hw_owned` 는 bringup 에서 쫓던 두 증상이다.

### 6 — B4

계획의 보정 2 그대로 — **`CONFIG_PM_CPU_OPS` 를 켜지 않는다.** `psci_init` 이
`PRE_KERNEL_1` 에서 `PSCI_VERSION` SMC 를 쏘는데 그건 콘솔보다 먼저다.
BL31 이 응답하지 않으면 무음으로 죽어 원인 구분이 불가능하다.
`moru reboot` 이 `arm_smccc_smc()` 를 셸 핸들러에서 직접 부른다
(`ARM64` 가 `HAS_ARM_SMCCC` 를 무조건 select → 추가 Kconfig 0개).

**USB main 때문에 새로 생긴 것:** 재부팅하면 USB 가 끊겨 셸도 끊긴다.
재부팅 성공 여부는 **UART2 의 동기 마커**로 판정한다 — 마커가 다시 나오면
재부팅된 것이다.

핸들러 안의 순서를 지킨다 — **CDC 가 끊긴 뒤 비우는 것은 의미가 없다**:

```
  ① shell_print 로 안내 한 줄
  ② log flush (log_panic() 또는 동등한 것)
  ③ k_msleep(50)  — CDC 의 tx_fifo_work 가 K_MSEC(1) 로 예약되고 USB
                    프레임 몇 개가 더 나가야 한다
  ④ arm_smccc_smc(PSCI_SYSTEM_RESET, ...)
```

`probe` 를 먼저 둔다 — `PSCI_VERSION` 만 쏘아 BL31 응답을 확인한다. 거기서
보드가 얼면 그 자체가 결론이고, 콘솔에 "issuing..." 이 남아 원인이 명확하다.

PSCI 가 실패하면 대안은 CRU 글로벌 소프트 리셋이고, 그 레지스터는
**TRM Part1 CRU 장**에 있다 — [trm-grounding.md](trm-grounding.md) §5 의 1번
항목. TRM 을 다시 받을 유일한 실제 사유다.

## 하지 않는 것

- `apps/ledblink`·`usbled`·`usbuart` 는 건드리지 않는다 — 각자 특정 가설을
  가리기 위한 최소 구성이고 `hardware-verified.md` 가 그 결과를 기록하고 있다
- 보드 DTS(`radxa_zero_3e.dts`)는 건드리지 않는다 — 부팅이 확인된 파일이다
- `sdcard/boot.cmd`·`boot.scr` 은 건드리지 않는다 — 유일하게 검증된 적재 경로다
- 셸을 UART 와 USB 에 **동시에** 띄우는 것 — `shell_transport` 를 새로
  구현하는 별개 작업이다(`shell_uart.c:527` serial 백엔드는 인스턴스가 하나)
- `CONFIG_GPIO_SHELL` — 이 SoC 의 GPIO 드라이버가 없다
