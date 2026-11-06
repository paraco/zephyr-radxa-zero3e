# 온디바이스 LLM — Zephyr RTOS 에 LFM2.5-350M 올리기

**2026-11-07 계획.** 목표는 `LiquidAI/LFM2.5-350M` 을 **Radxa Zero 3E 의
Zephyr RTOS 위에서** 추론하고 속도·메모리를 실측하는 것이다.

> ⚠ **먼저 [ai-on-rtos-strategy.md](ai-on-rtos-strategy.md) 를 읽는다.**
> 이 문서는 그 전략의 **Hub 계층** 실행 계획이다. 전략 문서가 정하는 것:
> 이 작업은 tinyML 이 **아니고**(Edge 모델은 15~84 KB, 약 3,000배 차이),
> 성공 기준은 속도가 아니라 **Edge 의 판정 흔적을 받아 설명하는가** 이며,
> dotprod 가 없는 Hub(A53·A72)는 대상이 아니다.

## 결론 먼저

**가능성은 메모리가 아니라 포팅에 달려 있다.** 이 보드는 DRAM 2 GB 인데 모델은
Q4 로 0.25 GB 다 — 메모리는 8배 여유다. 막는 것은 **추론 엔진을 Zephyr 에
올리는 일** 하나뿐이고, 그것이 전체 비용의 90 %다.

| | |
|---|---|
| Zephyr 에 llama.cpp/ggml 를 올린 전례 | **없다** (검색으로 못 찾았다 — 없다는 증명은 아니다) |
| 베어메탈(OS 없음) 전례 | **있다** — [dddimcha/embodios](https://github.com/dddimcha/embodios). K-quant 역양자화가 llama.cpp 와 바이트 단위로 일치한다고 주장한다. 참고 구현으로 읽을 가치가 있다 |

그래서 이 계획의 핵심은 **포팅에 들어가기 전에 버릴 수 있는 지점을 만드는
것**이다. 단계마다 관문을 두고, 관문을 넘지 못하면 멈춘다.

## 확정된 사실 (추측 아님)

우리 저장소의 실측 기록에서 가져온 값이다.

| 항목 | 값 | 출처 |
|---|---|---|
| SoC | RK3566, **Cortex-A55 × 4** | `docs/hardware-verified.md` — 라이브 sysfs |
| DRAM | `0x00200000 ~ 0x80000000` = **2 GB** | `boards/radxa/radxa_zero_3e/radxa_zero_3e.dts:41` |
| L1 D 라인 | 64 B | `getconf` 실측 |
| Zephyr 가 쓰는 창 | `0x40000000 + **1 MiB**` (`CONFIG_SRAM_SIZE=1024`) | `prj.conf` |
| Zephyr 이미지 끝 | `_end = 0x4005c000` (창 안에 ~660 KB 여유) | 링크 결과 |
| Zephyr 힙 | `CONFIG_HEAP_MEM_POOL_SIZE=**8192**` (8 KB) | `prj.conf` |
| SD 카드 접근 | **없다** — Rockchip SDHC 드라이버도 `sdmmc` DT 노드도 없다 | `docs/ota-strategy.md:16-17` |
| DRAM 적재 경로 | U-Boot `fatload` → 고정 주소, Zephyr 가 `device_map()` | `sdcard/boot.cmd`, `moru_shell.c` (OTA 가 이 패턴을 쓴다) |
| 이미 쓰는 DRAM 주소 | OTA 버퍼 `0x50000000` (+256 KB) | `moru_shell.c` |
| Linux 쪽 | Debian 11 bullseye, 커널 `5.10.160-26-rk356x`, **2026-11-03 에 부팅 확인** | `docs/hardware-verified.md` |
| Linux 이미지 | `images/radxa-zero3_bullseye_xfce_b6.img.xz` — **이미 내려받혀 있다** | 로컬 |

### 모델 쪽

| 항목 | 값 |
|---|---|
| 공식 GGUF | `LiquidAI/LFM2.5-350M-GGUF` — **`Q4_K_M` 이 올라와 있다** |
| llama.cpp LFM2 지원 | PR #14620 (350M·700M·1.2B). 참고 수치: LFM2-350M **F16** 이 데스크톱 10스레드에서 pp512 **725 t/s** |
| llama.cpp LFM2.5 | Liquid 공식 문서가 "upstream llama.cpp supports LFM2/LFM2.5" 라고 적는다. 8B-A1B 는 PR #23826 에서 토크나이저 확장 |
| 권장 샘플링 | `--temp 0.1 --top-k 50 --repeat-penalty 1.05` (공식 카드) |

## ⚠ 전달받은 내용 중 정정할 것 4건

사실 확인에서 드러난 것이다. 계획은 정정된 값으로 세운다.

**① "A55/A53 계열은 dotprod 경로가 빠름" — A53 은 dotprod 가 없다.**
A55 는 ARMv8.2-A 로 dotprod 가 있고 i8mm 은 없다(맞다). 그러나 **A53 은
ARMv8.0-A 로 dotprod 도 없다.** 그래서 Pi Zero 2 W(BCM2710, A53)에서는 Q4_0
재패킹 이득이 **나타나지 않는다.** 이번 목표가 Zero 3E(A55)이므로 계획에는
영향이 없지만, Pi 쪽으로 옮길 때 그 가정이 깨진다.

**② "GGUF Q4_0 사용" — 공식 GGUF 는 `Q4_K_M` 이고, ARM 사정도 바뀌었다.**
정적 타입 `Q4_0_4_4` 는 **삭제됐고**(llama.cpp #10757) 지금은 적재 시
런타임 재패킹이다. 게다가 **Q4_K 용 AArch64 재패킹**이 dotprod 전용 변종까지
올라왔다(#17494, #16739). 즉 "ARM 은 Q4_0" 는 낡은 조언일 수 있다.
→ **Q4_0 과 Q4_K_M 을 둘 다 측정한다.** 어느 쪽이 빠른지는 이 빌드에서
재어 봐야 알고, Q4_K_M 은 품질도 낫고 공식 배포본이다.

**③ "비교 대상 Qwen3.5-0.8B" — 존재한다.** 확인됐다
(`unsloth/Qwen3.5-0.8B-GGUF`, `bartowski/Qwen_Qwen3.5-0.8B-GGUF`). Qwen3.5
Small 계열 0.8B/2B/4B/9B 중 하나다. 다만 **0.8B 는 350M 의 2.3배**라 Zephyr
단계의 비교 대상으로는 메모리·속도 모두 불리하다 → 품질 기준점으로만 쓰고,
Zephyr 에 올리는 것은 350M 하나로 한다.

**④ "실제 구현은 Linux 환경에서 먼저 검증 권장" — 방향은 Zephyr 로 정해졌다.**
사용자 결정이다. 다만 **Linux 측정을 빼지는 않는다** — 그것은 우회가 아니라
**기준선**이다. 같은 실리콘에서 몇 t/s 가 나오는지 모르면 Zephyr 포팅이
제대로 동작하는지, 5배 느린지 판단할 수가 없다. 우리 자신의 규칙
(`docs/bringup-rules.md` 규칙 1: 기준선을 먼저 굽는다)이 그렇게 적혀 있다.

## 단계 — 관문이 있는 순서

### 0단계. 기준선: 같은 실리콘의 Linux 수치 (1~2일)

**이것 없이는 어떤 Zephyr 수치도 해석할 수 없다.**

아래는 전달받은 구현 순서를 **우리가 확인한 사실로 각색한** 실행 레시피다.
각색한 곳에는 이유를 붙였다.

#### 0-a. 가장 먼저 — CPU 기능 확인 (한 줄)

```bash
grep -o 'asimddp\|i8mm' /proc/cpuinfo | sort -u
```

**이 한 줄이 측정 해석의 전제다.** `asimddp`(= dotprod)가 없으면 llama.cpp 의
ARM 재패킹 경로가 아예 안 걸리고, Q4 기대값이 전부 틀어진다. A55 는 ARM 문서
기준 dotprod 가 있고 i8mm 은 없다 — **그러나 이 보드에서 재 본 적이 없다.**
(맥에서 같은 것을 보려면 `sysctl -a | grep FEAT_DotProd`. 이 맥 M3 Pro 는
DotProd·I8MM 둘 다 1 이다 — "있는 경우" 의 예다.)

#### 0-b. 환경 — 두 번째 카드, SSH, 스왑 끄기

- **두 번째 SD 카드**에 `images/radxa-zero3_bullseye_xfce_b6.img.xz` 를 쓴다.
  ⚠ **지금 Zephyr 카드를 덮지 않는다** — 2026-11-06 에 카드 교체 비용을
  겪었다(하루에 롤백 4회).
- 접속은 **이더넷 + SSH**. ⚠ **ADB 로는 안 된다** — 가젯은 올라오지만
  `adb root` 가 거부되고 `uid=2000` 이라 `apt` 도 `--mlock` 도 막힌다
  (`docs/hardware-verified.md`). 시리얼은 1,500,000 baud 인데 우리 CP2102 가
  그 속도를 못 낸다(같은 문서).
- `sudo swapoff -a` — **켜져 있으면 측정이 오염된다.** 2 GB 에 0.25 GB
  모델이면 필요 없다.
- `--mlock` 을 쓰려면 `RLIMIT_MEMLOCK` 을 올려야 한다(`ulimit -l unlimited`,
  또는 `sudo`). 이것이 ADB 가 아니라 SSH 로 가는 이유 중 하나다.

#### 0-c. llama.cpp 빌드 + **LFM2.5 지원을 그 빌드에서 확인**

```bash
cmake -B build -DGGML_NATIVE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
```

각색: 전달 내용은 "LFM2 구조 지원 확인" 이라고만 적었다. **확인 방법을
정한다** — 빌드한 바이너리가 그 구조를 아는지 직접 묻는다:

```bash
./build/bin/llama-cli --list-devices   # 빌드가 성립했는지
strings build/bin/llama-cli | grep -i lfm2   # 구조 이름이 들어갔는지
```

상류 근거: PR #14620 이 LFM2(350M·700M·1.2B)를 넣었고, Liquid 공식 문서가
upstream llama.cpp 가 LFM2/LFM2.5 를 지원한다고 적는다. **그래도 이 빌드에서
확인한다** — 버전이 낡으면 LFM2.5 의 확장 토크나이저에서 막힌다.

#### 0-d. 모델 — **Q4_0 과 Q4_K_M 을 둘 다** 받는다

```bash
# 공식 GGUF (Q4_K_M 이 올라와 있다)
./build/bin/llama-cli -hf LiquidAI/LFM2.5-350M-GGUF --conversation \
    --temp 0.1 --top-k 50 --repeat-penalty 1.05
```

⚠ **각색한 곳**: 전달 내용은 "GGUF Q4_0 사용 (A55/A53 계열은 dotprod 경로가
빠름)" 이었다. 두 군데를 고친다.

1. **공식 배포본은 `Q4_K_M` 이다.** Q4_0 이 저장소에 없으면 F16 에서 만든다:
   `./build/bin/llama-quantize <f16.gguf> <q4_0.gguf> Q4_0`
2. **"ARM 은 Q4_0" 는 낡은 조언일 수 있다.** 정적 타입 `Q4_0_4_4` 는
   삭제됐고(#10757) 지금은 적재 시 런타임 재패킹이며, **Q4_K 용 AArch64
   재패킹**이 dotprod 전용 변종까지 올라왔다(#17494·#16739).
   → 그래서 **둘 다 재서 이 빌드·이 칩에서 어느 쪽이 빠른지 정한다.**
   Q4_K_M 이 이기면 품질까지 같이 얻는다.
3. **A53 은 dotprod 가 없다**(ARMv8.0). 전달 메모의 "A55/A53 계열" 은
   A53 에 대해 틀렸다. 이번 대상은 A55 라 영향은 없다.

#### 0-e. KV 캐시는 **추정하지 않고 계산한다** (맥에서 가능)

모델 카드 기준 16층 하이브리드(짧은 컨볼루션 10 + GQA 어텐션 6)이고 KV 가
**어텐션 6층에만** 생긴다. 그래서 32K 컨텍스트에서도 작을 것으로 보이지만,
**숫자는 GGUF 메타데이터에서 읽는다**:

```bash
python3 llama.cpp/gguf-py/gguf/scripts/gguf_dump.py --no-tensors <model.gguf>
```

`n_embd_head_k`·`n_embd_head_v`·`n_head_kv`·`block_count`·`n_ctx_train` 을
보고 KV = `2 × 층수(어텐션만) × n_ctx × n_embd_kv × 바이트` 로 계산한다.
**이 값이 1단계 메모리 설계의 입력이다.**

#### 0-f. 측정 행렬

```bash
./build/bin/llama-bench -m <model.gguf> -t 1,2,3,4 -p 128 -n 64 -r 5
```

| 축 | 값 |
|---|---|
| 양자화 | `Q4_0`, `Q4_K_M` |
| 스레드 | 1, 2, 3, 4 (A55 4코어) |
| 작업 | `pp128`(프롬프트 처리, 연산 바운드), `tg64`(생성, 대역폭 바운드) |
| 컨텍스트 | `-c 512 / 2048 / 8192 / 32768` 에서 피크 RSS |

함께 **반드시** 기록한다 — 이것 없이는 수치를 믿을 수 없다:

```bash
/usr/bin/time -v ./build/bin/llama-cli ...   # 피크 RSS
cat /sys/class/thermal/thermal_zone*/temp     # 측정 전·후 온도
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq  # 클럭
```

⚠ **수동 방열 보드다.** 발열로 클럭이 떨어지면 스레드를 늘려도 느려진다.
온도·클럭을 같이 안 적으면 그 현상을 "스레드 확장이 안 된다" 로 오독한다.

#### 0-g. 비교 기준점 — Qwen3.5-0.8B (품질만)

존재 확인됐다(`unsloth/Qwen3.5-0.8B-GGUF`·`bartowski/Qwen_Qwen3.5-0.8B-GGUF`).
⚠ **각색**: **350M 의 2.3배**다. 그래서 **품질 기준점으로만** 쓰고 Zephyr 에
올리는 대상으로는 잡지 않는다. 한국어 품질 비교에 쓴다(LFM2.5 는 한국어 포함
9개 언어를 주장한다 — 그 주장을 확인하는 것이 이 비교의 목적이다).

#### 0-h. 라이선스 — 포팅 **전에** 확인한다

LFM Open License 의 **상업 조건**. 2~6주를 쓰기 전에 봐야 한다. 제품에 넣을
수 없는 라이선스면 모델을 바꾸는 것이 포팅보다 싸다.

#### 기록 — 저장소 CSV 를 정본으로

`docs/llm-bench.csv` 에 적는다. 전달받은 양식은 `ZERO3E_LLM_벤치마크.xlsx`
'벤치마크 기록' 시트인데 **로컬에서 찾지 못했다**(Google Drive 쪽일 수 있다).
⚠ **각색한 이유**: 숫자가 git 으로 diff 되어야 "언제 무엇이 느려졌나" 를
추적할 수 있다. xlsx 는 그게 안 된다. 보고용이 필요하면 CSV 에서 내보낸다.

**관문 0:** 350M Q4 가 이 실리콘에서 **tg 5 t/s 이상** 나오는가.
못 나오면 Zephyr 포팅은 의미가 없다 — OS 를 바꿔도 메모리 대역폭은 같다.
⚠ **가정을 명시한다**: tg 10~15 t/s 라는 추정은 **RK3566 LPDDR4 유효
대역폭 약 3~4 GB/s** 와 모델 0.23 GB 에서 나온 것이다. `llama-bench` 가
3 t/s 라고 하면 **그것은 실리콘의 값이고 Zephyr 탓이 아니다.** 먼저 박아
두어야 나중에 포팅을 엉뚱하게 의심하지 않는다.

### 1단계. Zephyr 메모리 바닥 공사 (2~3일)

지금 Zephyr 는 **1 MB 창**에서 돌고 힙이 **8 KB** 다. 수백 MB 를 쓰려면:

- `CONFIG_SRAM_SIZE` 를 실제 DRAM 영역으로 확장, 또는 모델·KV·연산 버퍼를
  **창 밖의 예약 영역**에 두고 `device_map()` 으로 붙인다(지금 OTA 가 쓰는 방식).
  → 후자가 안전하다. 커널 힙을 수백 MB 로 키우는 것보다 변경이 작다.
- ✅ **창을 키우는 것은 이례적인 일이 아니다.** 인트리 `rpi_4b` 가 이미
  DRAM **8 MB** 를 잡는다(`reg = <0x200000 DT_SIZE_M(8)>`). 우리가
  `DT_SIZE_M(1)` 인 것은 우리가 그렇게 정한 것이다 — 처음 이 단계의 위험을
  과대평가했다. 남는 위험은 아래 EL2 캐시 상호작용뿐이다.
- 주소 배치를 정한다. 이미 쓰는 것: Zephyr `0x40000000`(1 MB),
  OTA `0x50000000`(256 KB), U-Boot 는 `0x80000000` 근처로 자신을 재배치한다.
- `CONFIG_MAX_XLAT_TABLES=16` 재계산. 큰 영역은 2 MB 블록 매핑으로 덮으면
  테이블이 적게 든다 — `device_map()` 이 그렇게 하는지 확인해야 한다.

**⚠ 이 단계가 이 보드에서 가장 위험하다.** 메모리 배치를 건드리는 변경이고,
이 보드는 **EL2 데이터 캐시 문제로 초기 배치에 데인 전력이 있다**
(`CONFIG_ARM64_BOOT_DISABLE_DCACHE` 로 막아 둔 상태다). 그리고
`CONFIG_UART_NS16550_DW8250_DW_APB` 사건처럼, 잠복 결함이 새 설정과 만나면
무한 루프가 된다. **그래서 `moru ota apply try` 슬롯을 쓴다** — 부팅이 깨져도
전원 재투입으로 복구된다(2026-11-06 실측).

**관문 1:** 256 MB 영역을 매핑해 쓰고(쓰기·읽기·CRC 확인) 부팅이 안정적인가.
`moru htop` 의 힙 줄과 `moru ota` 대용량 전송으로 검증한다.

### 2단계. 모델을 DRAM 에 올리는 경로 (1일)

패턴이 이미 있어 가장 싸다.

- `sdcard/boot.cmd` 에 블록 추가: `fatload` 로 GGUF 를 고정 주소(예
  `0x60000000`)에 적재 → Zephyr 가 `device_map()` 으로 **읽기 전용** 매핑
- 적재 확인은 **CRC32** 로 한다(`moru ota verify` 와 같은 방식)
- ⚠ `boot.cmd` 의 `fatwrite` 가 크기를 고정으로 쓰는 함정이 있었다
  (`0x40000`). `fatload` 는 `${filesize}` 를 쓰므로 그 함정은 없지만,
  **U-Boot 에 `setexpr` 가 없다**는 제약은 그대로다
  (`docs/ota-strategy.md`) — 크기를 변수로 계산해야 하면 막힌다.
- 대안인 USB CDC 부어넣기는 **느리다**: 실측 472 KB/s → 230 MB 에 약 8분.
  `fatload` 가 맞다.

**관문 2:** 230 MB GGUF 가 DRAM 에 적재되고 CRC32 가 맞는가.

### 3단계. 추론 엔진 — **여기가 전부다** (2~6주, 전례 없음)

| 선택 | 범위 | 판단 |
|---|---|---|
| **A. 전체 llama.cpp 포팅** | C++17 stdlib · pthread · mmap · 파일 I/O · 서버·CLI | 가장 크다. 버릴 것이 많은데 다 끌고 온다 |
| **B. ggml CPU 백엔드 + llama.cpp 의 모델 그래프·토크나이저만** | 파일 I/O 없이 메모리에서 바로. ggml 핵심은 순수 C | **권한다.** 버리는 것이 명확하다 — 파일·서버·CLI |
| **C. 처음부터 쓰기 (llama2.c 식)** | LFM2 하이브리드(짧은 컨볼루션 10 + GQA 6)를 직접 | Q4 NEON dotprod 커널을 직접 써야 해 속도·정확도 모두 불리 |

B 의 **실제 쪼개진 작업**:

1. **C++ 런타임.** 지금 우리는 **picolibc** 다. llama.cpp 의 모델 그래프와
   토크나이저는 C++17 이고 `std::string`·`std::vector`·예외를 쓴다.
   → `CONFIG_CPP=y` `CONFIG_STD_CPP17=y` `CONFIG_LIB_CPLUSPLUS=y`,
   그리고 **libc 를 newlib 로 바꿔야 할 가능성이 높다.** 이것만 따로
   확인하는 스파이크를 먼저 둔다(빈 C++ 앱이 뜨는가).
2. **스레드.** ggml 은 자체 threadpool 을 쓴다. Zephyr POSIX(pthread)로
   맞출 수 있는지, 아니면 `k_thread` 로 얇은 shim 을 쓰는지 확인.
3. **메모리 할당자.** ggml 은 큰 블록을 `malloc` 한다. 1단계에서 정한
   예약 영역 위에 `sys_heap` 을 올리고 `malloc` 을 그쪽으로 돌린다.
4. **파일 I/O 제거.** GGUF 를 `mmap` 대신 **이미 DRAM 에 있는 포인터**로
   받는다. `llama_model_load` 경로를 메모리 로더로 바꾼다.
5. **시간·난수.** `ggml_time_us()`, 샘플링용 RNG 를 Zephyr 로 연결.

**관문 3-a (스파이크, 1~2일):** C++17 이 이 보드에서 뜨는가.
**"빈 앱" 으로는 부족하다** — picolibc→newlib 만이 아니라 **예외**와
**`std::thread`** 가 각각 미지수다. 한 파일에 셋을 넣어 한 번에 가린다
(전문은 [ai-on-rtos-strategy.md](ai-on-rtos-strategy.md) §6):
`std::vector<float>` · `throw`/`catch` · `std::thread` + `join`.
하나라도 막히면 B안의 비용이 두 배가 된다. 가장 싼 확인이므로 **가장 먼저** 한다.

**관문 3-b:** ggml 단독으로 작은 행렬곱이 맞는 값을 내는가
(호스트에서 계산한 기준값과 비교).

**관문 3-c:** 토큰 1개가 나오는가. 여기까지 오면 나머지는 성능 문제다.

### 4단계. 측정과 비교 (2~3일)

0단계의 Linux 수치와 **같은 프롬프트·같은 양자화**로 비교한다.

그리고 **Zephyr 쪽의 진짜 이점을 측정한다** — 속도가 아니라 이것들이다:

| 측정 | 왜 |
|---|---|
| 토큰 간 지연의 분포 (p50/p99/최대) | OS 지터가 없는 것이 RTOS 의 값이다. Linux 와 꼬리를 비교한다 |
| 전원 투입 → 첫 토큰 | U-Boot 6.8 s + Zephyr 54 ms(실측). Linux 부팅과 비교 |
| 피크 메모리 | Zephyr 는 OS 가 안 먹는다 |

### 5단계. `moru` 셸 통합 (1일)

`moru llm "질문"` · `moru llm bench` · `moru llm status`(모델·KV·메모리).
이미 있는 셸·OTA·uart2 로그·fatal 덤프가 그대로 쓰인다.

## 아직 모르는 것 — 푸는 순서대로

| 미지수 | 어떻게 푸는가 | 언제 |
|---|---|---|
| 빈 C++17 앱이 이 보드에서 뜨는가 | 스파이크 (가장 싸고 가장 결정적) | **0단계와 병행** |
| KV 캐시가 실제로 몇 MB 인가 | GGUF 메타데이터를 맥에서 `gguf_dump` 로 읽어 계산. 16층 중 어텐션 6층에만 생기므로 추측하지 않는다 | 0단계 |
| Q4_0 vs Q4_K_M 중 A55 에서 무엇이 빠른가 | 0단계에서 둘 다 측정 | 0단계 |
| ggml 의 threadpool 을 Zephyr 로 맞출 수 있는가 | ggml 의 스레드 추상화를 읽는다 | 3단계 전 |
| 발열로 클럭이 떨어지는가 | 0단계에서 온도·클럭을 함께 기록. 수동 방열이라 지배적일 수 있다 | 0단계 |
| LFM Open License 의 상업 조건 | 라이선스 전문 확인 — 제품에 넣기 전 필수 | 3단계 전 |
| `device_map()` 이 큰 영역을 블록 매핑으로 덮는가 | 소스 확인 + 1단계 실측 | 1단계 |

## 기록

전달받은 양식은 `ZERO3E_LLM_벤치마크.xlsx` 의 '벤치마크 기록' 시트인데
**로컬에서 찾지 못했다**(Google Drive 쪽에 있을 수 있다).

제안: **측정의 정본은 이 저장소의 CSV 로 둔다**(`docs/llm-bench.csv`).
이유는 하나다 — 숫자가 git 으로 diff 되어야 "언제 무엇이 느려졌나" 를 추적할
수 있고, xlsx 는 그게 안 된다. 보고용으로 xlsx 가 필요하면 CSV 에서 내보낸다.

열: `날짜, 호스트(linux|zephyr), 커밋, 모델, 양자화, 스레드, pp t/s, tg t/s,
피크 MB, 온도 시작/끝, 클럭 평균, 비고`

## 비용 정직하게

| 단계 | 기간 | 위험 |
|---|---|---|
| 0 기준선 (Linux) | 1~2일 | 낮다. 이미지도 있고 전에 부팅했다 |
| 1 메모리 바닥 | 2~3일 | **높다** — 이 보드의 캐시 전력. try 슬롯으로 복구는 싸다 |
| 2 모델 적재 | 1일 | 낮다. 패턴이 있다 |
| 3 추론 엔진 | **2~6주** | **가장 높다. Zephyr 전례가 없다** |
| 4 측정 | 2~3일 | 낮다 |
| 5 셸 통합 | 1일 | 낮다 |

**3단계 앞에 멈출 지점을 둔 것이 이 계획의 요점이다.** 관문 0(속도가
의미 있는가)과 관문 3-a(C++ 가 뜨는가)를 **먼저, 싸게** 확인하고, 둘 중
하나가 막히면 포팅에 들어가지 않는다.

## 관련 문서

- `docs/hardware-verified.md` — 이 보드의 실측값, Linux 부팅 기록, 접속 경로
- `docs/bringup-rules.md` — 기준선·한 변수·관측 채널 규칙. 이 계획도 그걸 따른다
- `docs/ota-strategy.md` — SD 접근이 없는 이유, U-Boot 명령 조사
- `README.md` — `moru` 셸·OTA·실험 슬롯 사용법
