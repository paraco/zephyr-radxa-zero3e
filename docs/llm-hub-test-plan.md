# Zephyr Hub 의 LLM — 계층별 CLI 단위테스트 계획

**2026-11-07.** 목표는 **open-weight LLM 을 Zephyr Hub 에서 돌리고, 그것을
계층별로 독립 검증하는 CLI 를 만드는 것**이다.
대상 모델은 **`Qwen/Qwen3.5-0.8B`** 다(2026-11-07 교체 — 처음 계획은
LFM2.5-350M 이었다. 교체가 바꾼 것은
[ondevice-llm-plan.md](ondevice-llm-plan.md) 의 "모델 교체가 바꾼 것" 에 있다).

⚠ **교체로 이 문서의 중요도가 올라갔다.** Qwen3.5 의 Gated DeltaNet 은
llama.cpp 에서도 새 구조이고, 한 포크가 **"적재는 되는데 결과가 쓰레기"** 를
보고했다. 그 실패를 잡는 것이 이 사다리의 일이다.

## 왜 테스트가 먼저인가

이 포팅은 **Zephyr 전례가 없다**(베어메탈 전례만 있다 — `embodios`).
전례 없는 2~6주 작업을 **끝에서 한 번에** 검증하려 하면, 틀렸을 때 어디가
틀렸는지 알 수 없다. 어제 `THREAD_RUNTIME_STATS` 를 쫓으며 같은 일을 겪었다 —
관측 수단이 없어 "조용히 멈춤" 과 "fault" 를 구분하지 못해 왕복 6회를 태웠다.

그래서 **계층마다 CLI 로 독립 실행되는 단위테스트**를 만든다.

## 설계 원칙 — Edge 의 규율을 Hub 로

Edge 는 `.xmb` 에 **골든 4벡터**를 넣고, 콘솔이 자기 평가기로 계산해
**호스트·펌웨어 비트 일치**를 배포 관문으로 삼는다. 그 규율을 그대로 쓴다.

⚠ **단, 적용 지점을 정확히 잡아야 한다.** 생성된 **문장**에는 골든 파리티가
성립하지 않는다(샘플링이 있다). 그러나 그 아래의 **커널·층·logits 는 모두
결정적**이다 — 거기에는 성립한다. 전략 문서에서 제가 "생성 모델에 골든
파리티를 억지로 적용하지 않는다" 고 쓴 것은 **계층을 구분하지 않아 과했다.**

| 계층 | 결정적인가 | 골든 파리티 |
|---|---|---|
| 역양자화 · 커널 · 층 · logits | **그렇다** | **적용한다 (비트/허용오차)** |
| 샘플링 뒤 토큰 열 | 시드·그리디면 그렇다 | **적용한다 (토큰 열 일치)** |
| 자유 생성 문장 | 아니다 | 적용하지 않는다 |

## 또 하나의 원칙 — 아래 계층은 모델 없이 돈다

**L0~L4 는 230 MB 모델이 없어도 실행된다.** 골든값이 작아서 **펌웨어
이미지에 박아 넣을 수 있다.** 이것이 중요한 구조적 이점이다:

> **모델 적재 경로(2단계)가 완성되기 전에 추론 엔진(3단계)을 만들 수 있다.**

즉 두 단계가 **직렬이 아니라 병렬**이 된다. 전례 없는 작업의 일정 위험이
그만큼 줄어든다.

---

## QEMU 에서 거의 다 된다 — 실측으로 확인했다

**이것이 이 계획의 비용을 가장 많이 줄인다.** 하드웨어 왕복 없이
L0~L7(정확성 전부)을 QEMU 에서 개발할 수 있다.

### QEMU 의 CPU 가 우리 보드와 **같은 MIDR 을 보고한다**

작은 Zephyr 앱으로 `ID_AA64ISAR0/1_EL1` 을 직접 읽고 `SDOT` 를 실행시켜 쟀다
(QEMU 11.1.2):

| `-cpu` | MIDR | dotprod | i8mm | `SDOT` 실행 |
|---|---|---|---|---|
| `cortex-a53` | `0x410fd034` | ❌ | ❌ | — |
| **`cortex-a55`** | **`0x412fd050`** | ✅ | ❌ | ✅ `[20 52 84 116]` (정답) |
| `cortex-a76` | `0x414fd0b1` | ✅ | ❌ | ✅ |
| `max` | — | ✅ | **✅** | ✅ |

**`cortex-a55` 의 MIDR `0x412fd050` 은 우리 보드가 찍는 값과 같다** — 오늘
`moru info` 가 `Cortex-A55 r2p0, MIDR 0x412fd050` 이라고 찍었다. dotprod 가
있고 `SDOT` 가 **정답을 낸다.** i8mm 이 없는 것까지 실기와 같다.

⚠ **`-cpu max` 를 쓰지 않는다.** i8mm 이 있어서 llama.cpp 의 런타임 기능
탐지가 **실기에 없는 커널 경로**를 고른다. 그러면 실기에서 절대 돌지 않는
코드를 시험하고 최적화하게 된다. **반드시 `cortex-a55`.**

### 우리 보드의 창을 QEMU 와 같은 128 MB 로 맞출 수 있다

`qemu_cortex_a53` 의 `zephyr,sram` 은 **128 MB** 다. 우리 보드는 1 MB 인데,
**DTS 한 줄**로 맞춰진다 — `CONFIG_SRAM_SIZE` 는 보드 파일에 없고 DT 에서
파생된다(`arch/Kconfig:245-247`, 그 Kconfig 는 **[DEPRECATED]** 로 표시돼
있다 — DT 가 정본이다).

```dts
&sram0 { reg = <0x40000000 DT_SIZE_M(128)>; };
```

빌드로 확인한 결과:

| | 창 1 MB | **창 128 MB** |
|---|---|---|
| `CONFIG_SRAM_SIZE` | 1024 | **131072** (자동으로 따라온다) |
| `__kernel_ram_end` | `0x40100000` | `0x48000000` |
| `_end` | `0x4005c000` | `0x4009b000` (**+252 KB**) |
| **바이너리 크기** | 187768 B | **187768 B — 변화 없음** |

`+252 KB` 는 **페이지 프레임 장부**다(128 MB ÷ 4 KB = 32768 × 8 B ≈ 256 KB).
`.bss` 라서 **이미지가 커지지 않고 OTA 전송량도 그대로다.**

주소도 안전하다 — 창이 `0x48000000` 에서 끝나고 **OTA 버퍼가 `0x50000000`**
이라 128 MB 여유다. **256 MB 로 하면 끝이 정확히 OTA 버퍼에 닿으므로
128 MB 가 맞는 선택이다.**

⚠ **빌드가 된다는 것이 부팅한다는 뜻은 아니다.** 메모리 배치를 바꾸는
변경이고 이 보드는 EL2 캐시 전력이 있다 → **`moru ota apply try` 로 올린다.**

### 그래서 같은 설정이 양쪽에서 돈다

창 크기가 같으니 **L0~L5 는 QEMU 와 실기에서 같은 구성으로 빌드된다.**
QEMU 에서 개발하고, 통과한 것을 그대로 실기에 올려 확인한다.

### 메모리 배치 — 모델은 창 밖에 둔다

Qwen3.5-0.8B Q4 는 **약 0.55 GB** 라 128 MB 창에 안 들어간다. 그래서 나눈다 —
**가중치는 읽기 전용이므로 따로 매핑하는 것이 자연스럽다.**

| 무엇 | 어디 | 크기 |
|---|---|---|
| Zephyr + 힙 + 연산 버퍼 + KV + 로짓 | **창 안** (128 MB) | KV 2K≈25 MB · 로짓 1 MB · 나머지 연산 |
| **모델 가중치 (읽기 전용)** | 창 밖, `device_map()` | 약 0.55 GB |

실기에서는 U-Boot `fatload`, **QEMU 에서는 `-device loader`** 로 같은 주소에
넣는다 — **두 환경의 적재 경로가 같은 모양이 된다.**

```bash
qemu-system-aarch64 -cpu cortex-a55 -machine virt,secure=on,gic-version=3 \
  -m 2G -nographic -no-reboot -kernel zephyr.elf \
  -device loader,file=model.gguf,addr=0x60000000,force-raw=on
```

### QEMU 로 **안 되는 것** — 측정했다

| 안 되는 것 | 근거 |
|---|---|
| **EL2 진입 경로** | 위 앱이 `CurrentEL = EL1`, `ID_AA64PFR0.EL2 = 0` 을 찍었다 — **EL2 가 아예 없다**. 우리 보드는 EL2 로 들어오고 `ARM64_BOOT_DISABLE_DCACHE` 가 거기서만 돈다(`reset.c:158`). (`virt,virtualization=on` 으로 EL2 를 켤 수는 있다 — 시험해 본 적 없다) |
| RK3566 주변장치 | dwc3 USB · uart2(DW_apb_uart, 어제의 busy-detect) · RK GIC 사정 |
| **성능 수치** | TCG 는 실기보다 수십 배 느리다. **L8 은 하드웨어 전용이다** |
| OTA · 실험 슬롯 · 셸 | 보드 전용 |

**즉 QEMU 는 "정확성", 하드웨어는 "성능과 보드 고유 경로" 를 맡는다.**

## ✅ L0 통과 — 2026-11-07, QEMU `-cpu cortex-a55`

사다리 첫 칸을 **실제로 돌렸다.** 앱은 [`apps/llm_l0`](../apps/llm_l0/) 에 있다.

```
=== L0: C++17 런타임 관문 ===
  [1] std::vector<float>(1024)  size=1024  ... OK
  [2] throw/catch std::runtime_error ... OK
  [3] pthread_create + join (ggml 이 쓰는 것) ... OK
  [4] std::thread (참고) ... 쓸 수 없다 — libstdc++ 가 gthreads 없이 빌드됐다
=== L0 PASS (실패 0) ===
```

실기용 빌드도 통과한다(144,528 B) — 단, 아래 ④의 툴체인 조건이 붙는다.
**부팅은 아직 확인하지 않았다**(`apply try` 로 올려 확인한다).

### 배운 것 넷 — 둘은 내 가정이 틀렸음을 보여 준다

**① picolibc 로 된다. newlib 로 바꿀 필요가 없다.**
계획에 "libc 를 newlib 로 바꿔야 할 가능성이 높다" 고 적었는데 **틀렸다.**
`GLIBCXX_LIBCPP` 의 의존성이 `NEWLIB_LIBC || PICOLIBC`
(`lib/cpp/Kconfig:119-125`)이고 picolibc 로 STL·예외·RTTI 가 모두 켜졌다.

**② `std::thread` 는 쓸 수 없다 — 그러나 ggml 에는 필요 없다.**
툴체인의 libstdc++ 가 gthreads 없이 빌드돼 생성자 템플릿이 막혀 있다.
**ggml 은 pthread 를 쓴다.** 그래서 L0 의 ③을 `std::thread` 에서
**`pthread_create`** 로 바꿨다 — 그것이 진짜 요구사항이다.
⚠ **잔여 위험**: llama.cpp 의 C++ 부분이 `std::thread` 를 쓰는 곳이 있으면
걸린다. 3단계에서 확인한다.

**③ `CPP_EXCEPTIONS` 는 `MINIMAL_LIBCPP` 에서 열리지 않는다.**
`lib/cpp/Kconfig` 가 `if !MINIMAL_LIBCPP` 로 감싸고 기본값이 `MINIMAL_LIBCPP`
다. **`CONFIG_GLIBCXX_LIBCPP=y` 를 명시**해야 한다 — 안 하면 "y 를 줬는데 n 이
됐다" 는 경고만 나오고 조용히 꺼진다.

**④ ⚠ C++ 앱은 `cross-compile` 툴체인으로 빌드되지 않는다.**
그 변종에서는 `TOOLCHAIN_HAS_GLIBCXX` 가 `n` 이 되어
(`cmake/modules/kconfig.cmake:134`) `GLIBCXX_LIBCPP` 가 거부되고
`MINIMAL_LIBCPP` 로 떨어져 **`<vector>` 조차 못 찾는다.**
우리 `build.sh` 의 기본값이 `cross-compile` 이라 실제로 걸렸다.
→ `build.sh` 가 호출자의 `ZEPHYR_TOOLCHAIN_VARIANT` 를 존중하게 고쳤다.
기본값은 그대로 둔다(moru 가 그것으로 부팅하는 것이 확인돼 있다).

```bash
ZEPHYR_TOOLCHAIN_VARIANT=zephyr ./build.sh "$PWD/apps/llm_l0"
```

### 함정 하나 (실제로 걸렸다)

`pthread_create(NULL attr)` 가 **EINVAL(22)** 로 실패했다. 스택을 받을 데가
없어서다 — `CONFIG_DYNAMIC_THREAD` 이 필요하고 그것은
**`CONFIG_THREAD_STACK_INFO` 에 의존**한다(`kernel/Kconfig:192-195`).
**빌드는 통과하고 런타임에만 실패한다** — L0 이 있어야 잡히는 종류다.

## ✅ L1·L2·L3 통과 — 2026-11-07, QEMU `-cpu cortex-a55`

세 칸을 같은 날 올렸다. 각 앱의 README 에 상세가 있다.

| 칸 | 결과 | 이 칸이 답한 것 |
|---|---|---|
| **L1** 대용량 메모리 | PASS. CRC `0xa8b4f938` 호스트와 일치 | 창 밖 640 MiB 를 **C 코드 0줄**로 매핑한다 |
| **L2** GGUF 파서 | PASS (6항목) | 헤더·KV·텐서정보·정렬을 정확히 읽는다 |
| **L3** 역양자화 | PASS. **비트 일치** 640개 | **`ggml-quants.c` 가 Zephyr 로 그대로 빌드된다** |

### 가장 큰 결과 — ggml 이 Zephyr 로 그대로 온다

계획 B안(ggml CPU 백엔드 + llama.cpp 의 모델 그래프만)의 최대 미지수가
"ggml 의 C 가 Zephyr 에서 되는가" 였다. **된다.**

L3 에서 `ggml-quants.c`(5천 줄)를 한 줄도 고치지 않고 `target_sources` 에
넣어 빌드되고 돌았고, 값이 호스트와 **비트 단위로 같았다**(Q4_0 128개 +
Q4_K 512개). 필요한 것은 양자화 경로만 쓰는 **스텁 네 개**뿐이다.

### 창 밖 DRAM 매핑 — `app.overlay` 한 장이다

arm64 가 DT 의 `zephyr,memory-region` 노드를 부팅 때 평면 매핑한다
(`arch/arm64/core/mmu.c:907` 의 `mmu_dt_regions`). `device_map()` 호출도
SoC 의 `mmu_regions[]` 수정도 필요 없었다.

⚠ 두 함정이 **빌드를 통과하고 런타임에만** 터진다:
- `zephyr,memory-attr` 를 빼면 MMU 항목이 아예 안 생긴다
  (`MMU_REGION_DT_FLAT_ENTRY_FROM_DT` 가 `IF_ENABLED` 로 감싸고 있다).
- 2 MB 정렬을 깨면 4 KB 페이지로 깔려 `MAX_XLAT_TABLES` 를 넘기고
  **콘솔이 올라오기 전에** 죽는다 — 과거의 "조용한 죽음" 과 증상이 같다.

### 골든값 장치가 검증됐다 — 사다리 전체가 이것에 얹힌다

Zephyr `crc32_ieee` 가 `zlib.crc32` 와 같음을 코드로 확인했고
(`subsys/crc/crc32_sw.c`: 다항식 `0xedb88320` · 초기 `~0` · 최종 반전),
640 MiB 를 64 KiB × 10,240 청크로 이어 계산한 값이 호스트와 같았다.

골든값의 **출처 등급**을 칸마다 다르게 두었다:
- L2 — 참조 `GGUFWriter` 로 만들고 참조 `GGUFReader` 로 뽑았다(내 파서와 독립).
- L3 — **ggml 의 C** 를 호스트에서 그대로 호출했다. 파이썬 역양자화기를
  쓰지 않았다. 허용 오차가 0 인데 파이썬과 C 는 결합 순서가 달라 반올림이
  달라질 수 있다.
- f32 는 **비트 패턴**으로 적는다. 10진수로는 비트 일치를 확인할 수 없고,
  실제로 골든값에 **음의 0**(`0x80000000`)이 들어 있다.

### ⚠ 통과했지만 실기에서 다시 봐야 하는 것

L2 의 [1]·[2] 는 "블롭은 Zephyr MMU·캐시 전에 적재기가 쓰므로 캐시 가능
매핑으로 읽으면 낡은 줄을 볼 수 있다" 를 겨냥했다. 그런데 **QEMU TCG 는
데이터 캐시를 모형화하지 않아 이 시험은 QEMU 에서 실패할 수 없다.**
양성 결과에 정보가 없다. 잔여 위험으로 계속 둔다.

### 새 하네스 · 새 도구

| 도구 | 하는 일 |
|---|---|
| `tools/qemu_run.sh` | 빌드 + QEMU + PASS/FAIL 판정. 판정 문장이 없으면 **종료코드 2** 로 "조용한 죽음" 을 제한시간 초과와 구분한다 |
| `tools/l1_golden.py` | L1 패턴의 CRC32 (numpy 벡터화, 640 MiB 0.17초) |
| `tools/l2_make_gguf.py` | 참조 writer 로 시험 GGUF + 골든 헤더 |
| `tools/l3_golden.sh` | **호스트 ggml** 로 역양자화 골든값 |
| `.venv/` | `gguf-py` 가 `pyyaml` 을 요구한다 |
| `upstream/llama.cpp` | `--depth 1` 클론(223 MB). `src/models/qwen35.cpp` **있다** |

### 오진 하나를 기록한다

L3 를 처음 돌릴 때 `L_BLOB=` 을 빼먹었다. 전부 0 인 메모리를 역양자화하면
Q4_0 은 `-8×0 = -0.0`, Q4_K 는 `0×0-0 = +0.0` 이 나온다. **그럴듯한 부동소수
값**이라 "포팅이 틀렸다" 로 읽혔고 `ggml_fp16_t` 가 `__fp16` 으로 잡혔다는
가설까지 세웠다(틀렸다 — `ggml_half` 는 C 경로에서 항상 `uint16_t` 다).

그래서 **양쪽에 자기 진단을 넣었다**: 하네스가 `app.overlay` 에 `model_blob`
이 있고 `L_BLOB` 이 없으면 돌리기 전에 경고하고, 앱이 매직을 먼저 본다.

---

## ✅ L4 통과 — **ggml 전체가 Zephyr 로 온다**

```
  [0] 기능 집합  Zephyr dotprod=1 i8mm=0 / 호스트 1 0 ... OK
  [1] mm f32    128개 ... OK — 비트 일치
  [2] mm Q4_0   128개 ... OK — 비트 일치
  [3] RMSNorm   128개 ... OK — 비트 일치
  [4] SiLU      128개 ... OK — 비트 일치
  [5] RoPE      256개 ... OK — 최대 상대오차 344 ppb ≤ 1e-5
=== L4 PASS ===
```

L3 은 `ggml-quants.c` 한 파일이었다. L4 는 **ggml 코어 + CPU 백엔드 23개
파일**(C/C++ 혼합)이고 `ggml_init` → 그래프 → `ggml_graph_compute` 가 전부
돈다. **상류 소스는 한 줄도 고치지 않았다.**

### 포팅 항목은 셋뿐이다 — 전부 상류를 고치지 않는 방식

| 항목 | 왜 | 어떻게 |
|---|---|---|
| `ggml-version.h` | cmake 생성 파일 | `cmake/llm_ggml.cmake` 가 상류 템플릿으로 만든다 |
| `std::mutex` 없음 | 툴체인 libstdc++ 가 gthreads 없이 빌드됐다 (**L0 의 사실 ②**) | `ggml-threading.cpp`(12줄) 제외 + `apps/llm_common/ggml_port.c` 가 `k_mutex` 로 |
| `M_PI` · `_SC_PHYS_PAGES` | picolibc 가 `__BSD_VISIBLE` 로 감춤 · Zephyr sysconf enum 에 없음 | `ggml_zephyr_compat.h` 를 `-include` 강제 포함 |

**B안(ggml 포팅)이 성립한다.** 이제 남은 위험은 ggml 이 아니라 **모델 그래프**
(L5 의 Gated DeltaNet)와 **성능**(L8)이다.

### ⚠ QEMU 사다리가 실기와 **다른 커널**을 시험하고 있었다

처음 `[0]` 이 `Zephyr dotprod=0 / 호스트 1` 로 불일치였다. `qemu_cortex_a53`
보드라 Zephyr 가 `-mcpu=cortex-a53`(ARMv8.0)을 주고 `__ARM_FEATURE_DOTPROD` 가
꺼져 ggml 이 **스칼라 경로**를 골랐다 — QEMU 를 `-cpu cortex-a55` 로 돌려도
그렇다. **컴파일 타임 선택이라 런타임 CPU 와 무관하다.**

실기(`radxa_zero_3e/rk3568`)는 Zephyr 가 이미 `-mcpu=cortex-a55` 를 주므로
하드웨어에서는 켜진다. 즉 **QEMU 쪽만 모르게 덜 시험하고 있었다.** 고쳤다.

그리고 **플래그 순서**에 또 걸렸다. `target_compile_options` 로는 안 된다 —
순서가 `[타깃][인터페이스][소스]` 이고 Zephyr 의 `-mcpu=cortex-a53` 이
인터페이스로 뒤에 붙어 이긴다. `set_property(SOURCE ...)` 로 소스 수준에
줘야 한다.

> **교훈으로 둘 것:** QEMU 가 보고하는 CPU(`-cpu`)와 **컴파일된 코드의 기능
> 집합**은 별개다. 사다리의 각 칸이 기능 매크로를 찍고 호스트와 비교하게 둔
> 것이 이것을 잡았다. 안 찍었으면 "통과" 로 넘어갔을 것이다.

### 함정 둘을 더 기록한다

**`file(STRINGS)` 에 `ENCODING UTF-8` 이 필수다.** 없으면 ASCII 로 읽어 한글
주석의 UTF-8 바이트를 구분자로 삼아 줄을 조각낸다 — 소스 23개가 **108조각**이
되고 `ggml/** 를 못 찾는다` 같은 엉뚱한 오류로 나타난다.

**아레나가 요청과 같으면 `ggml_aligned_malloc` 이 실패한다.** `ggml_init` 의
`mem_size` 를 정렬 할당 한 덩이로 받으므로 정렬 여유가 필요하다. 요청의
여러 배로 둔다. ggml 의 진단 메시지가 정확해 한 번에 찾았다.

### L5 의 전제가 정리됐다

| 미해결이었던 것 | 확인된 사실 |
|---|---|
| upstream 이 `qwen35` 를 지원하나 | **한다** — `src/models/qwen35.cpp` |
| 라이선스 | **Apache-2.0**, 게이트 없음 (`Qwen/Qwen3.5-0.8B`) |
| 실제 크기 | 873,438,784 파라미터 · Q4_K_M **약 533 MB** |
| 텍스트 전용으로 쓸 수 있나 | **된다** — `mmproj-*.gguf` 가 **별도 파일**이라 본체만 쓰면 텍스트다 |

---

## ✅ 모델 실측 — 2026-11-07, `gguf_dump` 로 읽은 값

`models/Qwen3.5-0.8B-Q4_K_M.gguf` · **532,517,120 B** (507.9 MiB)
sha256 `bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517`
Apache-2.0 · 게이트 없음 · `mmproj-*.gguf` 별도이므로 본체만 쓰면 텍스트 전용.

**계획서에 "카드에서 손으로 계산한 것" 이라 적었던 값들을 실측으로 바꾼다.**

| 키 | 값 |
|---|---|
| `general.architecture` | **`qwen35`** — upstream 의 `src/models/qwen35.cpp` 와 맞는다 |
| `block_count` | **24** |
| `embedding_length` | **1024** |
| `attention.head_count` / `head_count_kv` | **8 / 2** (GQA 4:1) |
| `attention.key_length` / `value_length` | **256 / 256** (head_dim 256) |
| `rope.dimension_count` | **64** — partial RoPE 64 가 맞았다 |
| `rope.freq_base` | **1e7** |
| `rope.dimension_sections` | **[11, 11, 10, 0]** — mRoPE 구간(VL 모델) |
| `context_length` | **262,144** |
| `ssm.conv_kernel` | **4** |
| `ssm.state_size` | **128** |
| `ssm.group_count` | **16** |
| `ssm.time_step_rank` | **16** |
| `ssm.inner_size` | **2048** |

### KV 캐시 — 손 계산이 맞았다

어텐션 층 1개 · 토큰 1개당 `2(K,V) × head_count_kv(2) × head_dim(256) × 2 B
= 2,048 B`. 24층 중 어텐션이 6층이므로 **토큰당 12 KiB**.
2,048 토큰이면 **24 MiB** — 계획서의 "KV 2K ≈ 25 MB" 와 맞는다.

즉 **128 MB 창 안에 KV·로짓·연산 버퍼가 들어가고 가중치만 창 밖**이라는
배치가 실측으로 확인됐다.

---

## 테스트 사다리

| # | 계층 | 무엇을 검증 | 골든 기준값 | 모델 필요? | QEMU? |
|---|---|---|---|---|---|
| **L0** | C++17 런타임 | `std::vector` · 예외 · `std::thread` | 자체(반환값) | ❌ | ✅ |
| **L1** | 대용량 메모리 | 예약 영역 매핑 · 패턴 쓰기/읽기 | CRC32 | ❌ | ✅ |
| **L2** | GGUF 파서 | 헤더 · 텐서 수·이름·형태·오프셋 | 맥의 `gguf_dump` 출력 | 헤더만 | ✅ |
| **L3** | 역양자화 | Q4_0 / Q4_K 블록 1개 → f32 | 호스트 llama.cpp 값 — **비트 일치** | ❌ (블록 내장) | ✅ |
| **L4** | ggml 커널 | 작은 행렬곱 · RMSNorm · SiLU · RoPE | 호스트 ggml 값 | ❌ (입력 내장) | ✅ **dotprod 포함** |
| **L5a** | **Gated DeltaNet 층** (SSM 순환) | 층 1개 입출력 | 호스트 중간 텐서 | ⭕ | ✅ |
| **L5b** | **Gated Attention 층** (GQA + QK-norm + partial RoPE) | 층 1개 입출력 | 호스트 중간 텐서 | ⭕ | ✅ |
| **L6** | 전체 순전파 | 고정 토큰 1개 → logits (**248,320개 ≈ 1 MB**) | 호스트 logits — **상위 k 일치** | ⭕ | ✅ (느리다) |
| **L7** | 생성 | 고정 시드 · greedy | 호스트와 **토큰 열 일치** | ⭕ | ✅ (느리다) |
| **L8** | 성능 | pp/tg t/s · 지연 꼬리 p50/p99 | 0단계 Linux 기준선 | ⭕ | ❌ **하드웨어 전용** |

**L5 를 둘로 나누는 이유 (Qwen3.5-0.8B)**: 24층 하이브리드 —
`6 × (3 × Gated DeltaNet → 1 × Gated Attention)` = **DeltaNet 18 + 어텐션 6**.
두 종류가 **각각 틀릴 수 있으므로 따로 검증한다.**

⚠ **L5a 가 이 포팅의 진짜 난관이다.** Gated DeltaNet 은 짧은 컨볼루션이 아니라
**SSM(Mamba 계열) 순환**이고, 층마다 텐서가 많다(상류 `qwen35` 텐서 목록):

```
ssm_a · ssm_alpha · ssm_beta · ssm_conv1d · ssm_dt.bias · ssm_norm · ssm_out
attn_qkv(융합) · attn_gate
```

그래서 **L5a 를 다시 쪼갠다** — 한 번에 맞히기 어렵고, 틀리면 어디가 틀렸는지
알아야 한다:

| | 하위 테스트 | 왜 |
|---|---|---|
| L5a-1 | `ssm_conv1d` 인과 컨볼루션 | 가장 단순한 조각 |
| L5a-2 | `ssm_a`/`dt`/`alpha`/`beta` 로 만든 **감쇠·게이트 계수** | 공개된 변환 함정에 **`A_log`** 가 있다 |
| L5a-3 | **델타 규칙 순환 1스텝** | 핵심. 상태가 누적되므로 여기서 틀리면 뒤가 전부 틀린다 |
| L5a-4 | `ssm_norm` + `ssm_out` | 공개된 함정에 **RMSNorm** 이 있다 |
| L5a-5 | **값 헤드 배치** `(3,16)` vs `(16,3)` | 공개된 변환 함정 그대로. 축을 뒤집어도 **형태는 맞아서 조용히 틀린다** |

L5b(어텐션)도 주의할 것이 둘 있다 — `attn_q` 에 **Q 와 gate 가 합쳐져 폭이 두
배**이고, **QK-norm 이 헤드별**이며 **RoPE 가 partial**(dim 64 of 256)이다.

**왜 이렇게까지 쪼개나.** 한 포크가 "Qwen3.5 GGUF 가 적재는 되는데 추론 결과가
쓰레기였고, 기존 DeltaNet 구현이 순전파의 **약 80 %** 만 맞았다" 고 보고했다
(`quant.cpp#94`). **"80 % 맞음" 은 통과하는 테스트가 없으면 보이지 않는다.**

---

## CLI — `moru llm`

기존 `moru` 셸에 붙인다. 셸·OTA·uart2 로그·fatal 덤프가 이미 있다.

```
moru llm test all              모든 계층을 순서대로. 첫 실패에서 멈춘다
moru llm test L3               한 계층만 (독립 실행)
moru llm test L3 -v            최대 절대오차 · 첫 불일치 인덱스 · 양쪽 값
moru llm test L0-L4            모델 없이 되는 것만
moru llm info                  모델 · KV · 메모리 · 양자화 · 골든 버전
moru llm bench [-t N] [-p N] [-n N]
moru llm run "프롬프트"        [--seed N] [--greedy]
```

### 출력 규약 — 숫자를 내놓는다

⚠ **"ok" 만 찍는 테스트는 만들지 않는다.** 우리 규칙 6("없음은 증거가
아니다")이 그렇게 적혀 있다. 통과해도 **무엇을 얼마나 맞혔는지** 찍는다.

```
Moru > moru llm test L3
L3 역양자화 — Q4_0 블록 1개 (32값)
  골든 버전  g4e39f76 (호스트 llama.cpp b7075)
  비교       32/32 비트 일치 ✅
  최대 오차  0.000000e+00 (허용 0 — 정수 연산이므로 비트 일치여야 한다)
  소요       41 us

Moru > moru llm test L4 -v
L4 커널 — 행렬곱 64×64×64 f32
  비교       4096/4096, 최대 절대오차 1.19e-07 (허용 1e-05) ✅
  첫 불일치  없음
  ⚠ 단일 스레드로 재었다 — 스레드 수가 바뀌면 누산 순서가 바뀌어
    부동소수 결과가 달라진다. L4t 가 그 불변성을 따로 본다
  소요       2.1 ms
```

### 허용오차 정책 — 계층마다 다르고, 이유를 적는다

| 계층 | 허용 | 이유 |
|---|---|---|
| L3 역양자화 | **0 (비트 일치)** | 정수 언팩 + 고정 스케일 곱이다. 다르면 구현이 틀린 것이다. `embodios` 도 K-quant 역양자화가 llama.cpp 와 바이트 일치라고 주장한다 |
| L4 커널 | `1e-5` 상대 | f32 누산 순서 차이. 단일 스레드 고정으로 재현성을 확보한다 |
| L5 층 | `1e-4` 상대 | 누적된다 |
| L6 logits | **상위 k 토큰 일치** | 절대값보다 순위가 중요하다 |
| L7 생성 | **토큰 열 완전 일치** | 시드·greedy 면 결정적이다. 다르면 어딘가 틀렸다 |

⚠ **L4t(스레드 불변성)를 따로 둔다.** 스레드 수를 바꾸면 부동소수 누산
순서가 바뀌어 결과가 달라진다. 이것을 "버그" 로 오독하지 않도록, 1~4 스레드
결과의 차이가 허용 안인지만 보는 테스트를 분리한다.

---

## 골든값을 어떻게 만들고 어디에 두나

### 만들기 — 맥에서 (`tools/llm_golden.py`)

Edge 의 `rom_to_xmb.py` 와 같은 패턴이다. 호스트 llama.cpp 로 기준값을
계산해 **두 가지 형태**로 낸다.

```bash
python3 tools/llm_golden.py --model <model.gguf> --out-header apps/moru/src/llm_golden.h   # L0~L4 (작다)
python3 tools/llm_golden.py --model <model.gguf> --out-blob  sdcard/llm_golden.bin         # L5~L7 (크다)
```

| 계층 | 크기 | 두는 곳 |
|---|---|---|
| L3 | 블록 1개 + f32 32개 ≈ 150 B | **펌웨어 이미지에 내장** |
| L4 | 64×64 입력 2개 + 출력 ≈ 48 KB | **내장** (OTA 상한 262144 B 안이다 — 여유 확인 필수) |
| L5 | 층 입출력 텐서 ≈ 수 MB | 블롭 — U-Boot `fatload` 로 모델과 함께 |
| L6 | logits — **248,320 × f32 ≈ 1 MB** | 블롭 (내장 불가) |
| L7 | 토큰 열 (수십 개) | **내장** (작다) |

⚠ **OTA 상한과 충돌한다.** 지금 이미지가 187 KB 이고 상한이 262 KB(`boot.cmd`
의 `fatwrite … 0x40000`)다. L4 골든 48 KB 를 내장하면 **235 KB** 가 되어
상한의 **90 %** 다. `build.sh` 가 90 % 에서 경고하도록 이미 만들어 뒀다.
→ **L4 골든은 크기를 줄이거나(32×32) 블롭으로 옮긴다.** 설계 단계에서
정해야 한다.

### 골든 버전을 찍는다

골든값은 **어느 호스트 llama.cpp 로 만든 것인지**에 달려 있다. `moru llm info`
와 각 테스트 출력에 **골든 버전(생성한 커밋 + llama.cpp 빌드 번호)** 을
찍는다. 어제 `moru info` 에 Zephyr 커밋 해시를 넣은 것과 같은 이유다 —
버전 문자열만으로는 재현이 안 된다.

---

## 단계 — 각 단계가 테스트 하나를 켠다

관문이 곧 테스트다. 테스트가 통과하면 그 단계가 끝난 것이다.

| 단계 | 하는 일 | 끝났다는 증거 |
|---|---|---|
| **S0** | Linux 기준선 (`docs/ondevice-llm-plan.md` §0) | CSV 에 pp/tg·온도·클럭·`asimddp` 가 적혔다 |
| **S1** | C++17 런타임 | `moru llm test L0` ✅ |
| **S2** | 대용량 메모리 영역 | `moru llm test L1` ✅ (256 MB CRC32) |
| **S3** | 골든 생성기 + 내장 | `tools/llm_golden.py` 가 헤더를 만들고 빌드가 통과 |
| **S4** | 역양자화 포팅 | `moru llm test L3` ✅ **비트 일치** |
| **S5** | ggml 커널 + 스레드 | `moru llm test L4` `L4t` ✅ |
| **S6** | GGUF 파서 (메모리에서) | `moru llm test L2` ✅ |
| **S7** | 모델 적재 경로 (U-Boot `fatload`) | 230 MB CRC32 일치 |
| **S8** | 층 순전파 (컨볼루션 · GQA) | `moru llm test L5` ✅ |
| **S9** | 전체 순전파 | `moru llm test L6` ✅ 상위 k 일치 |
| **S10** | 샘플링 · 생성 | `moru llm test L7` ✅ 토큰 열 일치 |
| **S11** | 성능 · 지연 꼬리 | `moru llm bench` → CSV, S0 과 비교 |

**S1~S6 은 S7(모델 적재)과 병렬이다.** 위의 "아래 계층은 모델 없이 돈다"
원칙 덕분이다.

### 모든 펌웨어 교체는 실험 슬롯으로

`moru ota apply try` 를 쓴다. 부팅이 깨져도 **전원 재투입으로 복구**된다
(2026-11-06 실측). 전례 없는 포팅에서 이것이 왕복 비용을 결정한다 — 어제
같은 실험 하나에 카드 롤백 한 번(사람 손)이 들었고 하루에 네 번 했다.

---

## 위험과 그것을 잡는 테스트

| 위험 | 어느 테스트가 잡는가 |
|---|---|
| C++ 예외가 arm64+newlib 에서 안 된다 | **L0** — 가장 먼저, 가장 싸다 |
| 큰 영역 매핑이 MMU 테이블을 넘긴다 | **L1** |
| 역양자화를 미묘하게 틀린다 (스케일·오프셋·블록 배치) | **L3 비트 일치** — 여기서 안 잡으면 L6 에서 "왜 이상한 토큰이 나오나" 로 번진다 |
| NEON 커널이 경계 조건에서 틀린다 | **L4** — 64 의 배수가 아닌 크기도 넣는다 |
| 스레드 누산 순서 차이를 버그로 오독 | **L4t** |
| 하이브리드 두 층 중 하나만 틀린다 | **L5a/L5b 를 나눈 이유** |
| **SSM 순환을 미묘하게 틀린다** (A_log · RMSNorm · 값 헤드 축) | **L5a-1~5** — 공개된 변환 함정 그대로를 각각 겨냥한다 |
| `attn_q` 의 Q+gate 합침·헤드별 QK-norm·partial RoPE 를 놓친다 | **L5b** |
| "적재는 되는데 결과가 쓰레기" | **사다리 전체.** 포크에서 실제로 보고된 실패다 |
| 토크나이저가 다르다 (어휘 248,320) | **L7** — 토큰 열이 안 맞으면 즉시 드러난다 |
| 느린데 왜 느린지 모른다 | **S0 기준선** 과 비교. 없으면 포팅을 엉뚱하게 의심한다 |

---

## 하지 말 것

1. **"ok" 만 찍는 테스트를 만들지 않는다.** 숫자를 내놓는다 — 최대 오차,
   첫 불일치 위치, 양쪽 값.
2. **L3 에 허용오차를 주지 않는다.** 정수 연산이므로 비트 일치여야 한다.
   오차를 허용하면 틀린 구현이 통과한다.
3. **모델이 있어야만 되는 테스트를 아래 계층에 두지 않는다.** L0~L4 가
   모델 없이 도는 것이 병렬 진행의 전제다.
4. **멀티스레드로 골든을 비교하지 않는다.** 단일 스레드로 고정하고,
   스레드 불변성은 L4t 에서 따로 본다.
5. **골든 버전을 안 찍고 비교하지 않는다.** 어느 호스트 빌드로 만든
   기준값인지 모르면 불일치의 원인을 가릴 수 없다.

---

## 관련 문서

- [ai-on-rtos-strategy.md](ai-on-rtos-strategy.md) — Edge·Hub 2계층 전략. 이
  문서는 그 Hub 계층의 **검증 계획**이다
- [ondevice-llm-plan.md](ondevice-llm-plan.md) — Hub LLM 의 단계·관문·0단계
  실행 레시피
- [bringup-rules.md](bringup-rules.md) — 기준선·한 변수·관측 채널·"없음은
  증거가 아니다". 이 계획의 출력 규약이 거기서 나왔다
- `llm-bench.csv` — 성능 측정 정본
- `~/Projects/NGT_Project/MoruLab_MLOps/` — Edge 의 골든 파리티 원형
  (`console/src/core/XmbFile.h` · `XgbEvaluator.cpp` · `tools/rom_to_xmb.py`)
