# L4 — ggml 커널 관문

[docs/llm-hub-test-plan.md](../../docs/llm-hub-test-plan.md) 의 사다리 다섯째 칸:
작은 행렬곱 · RMSNorm · SiLU · RoPE, **dotprod 포함**.

## 결과 (2026-11-07, QEMU `-cpu cortex-a55`)

```
=== L4: ggml 커널 관문 ===
  [0] 기능 집합  Zephyr dotprod=1 i8mm=0 / 호스트 1 0 ... OK
  [1] mm f32    128개 ... OK — **비트 일치**
  [2] mm Q4_0   128개 ... OK — **비트 일치**
  [3] RMSNorm   128개 ... OK — **비트 일치**
  [4] SiLU      128개 ... OK — **비트 일치**
  [5] RoPE      256개 ... OK — 비트는 10/256 다르나 최대 상대오차 344 ppb ≤ 10000
=== L4 PASS (실패 0) ===
```

## 이 칸이 답한 것 — **ggml 전체가 Zephyr 로 온다**

L3 은 `ggml-quants.c` 한 파일이었다. L4 는 **ggml 코어 + CPU 백엔드 23개
파일**(C/C++ 혼합)이다. `ggml_init` → 그래프 구성 → `ggml_graph_compute` 가
전부 돈다. 상류 소스는 **한 줄도 고치지 않았다.**

### 포팅에 필요한 것은 세 가지뿐이다

| 항목 | 왜 | 어떻게 |
|---|---|---|
| `ggml-version.h` | cmake 생성 파일이다(`ggml/src/CMakeLists.txt:216`) | `cmake/llm_ggml.cmake` 가 상류 템플릿으로 만든다 |
| `std::mutex` 가 없다 | 툴체인의 libstdc++ 가 gthreads 없이 빌드됐다 — **L0 의 사실 ②가 여기서 실제로 문제가 됐다** | `ggml-threading.cpp`(12줄)를 빌드하지 않고 `apps/llm_common/ggml_port.c` 가 `k_mutex` 로 같은 두 함수를 준다 |
| `M_PI` · `_SC_PHYS_PAGES` | picolibc 가 `M_PI` 를 `__BSD_VISIBLE` 로 감춘다 · Zephyr sysconf enum 에 `_SC_PHYS_PAGES` 가 없다 | `apps/llm_common/ggml_zephyr_compat.h` 를 `-include` 로 강제 포함 |

**원칙: 상류를 고치지 않는다.** 고치면 llama.cpp 를 올릴 때마다 충돌을 손으로
풀어야 하고, 우리 수정과 상류 버그를 구분할 수 없게 된다. 그래서 전부
"빼고 다시 구현" 또는 "강제 포함되는 호환 층" 으로 처리했다.

## ⚠ 가장 중요한 발견 — QEMU 사다리가 **실기와 다른 커널**을 시험하고 있었다

처음 돌렸을 때 `[0]` 이 `Zephyr dotprod=0 / 호스트 1` 로 불일치였다.

원인: QEMU 용 보드가 `qemu_cortex_a53` 이라 Zephyr 가 **`-mcpu=cortex-a53`**
을 준다. Cortex-A53 은 ARMv8.0 이고 dotprod 가 없다. 그래서
`__ARM_FEATURE_DOTPROD` 가 꺼지고 ggml 이 `ggml_vec_dot_q4_0_q8_0` 의
**스칼라 경로**를 골랐다 — QEMU 를 `-cpu cortex-a55` 로 돌려도 그렇다.
**컴파일 타임 기능 선택이라 런타임 CPU 와 무관하다.**

실기(`radxa_zero_3e/rk3568`)는 Zephyr 가 이미 `-mcpu=cortex-a55` 를 주므로
하드웨어에서는 dotprod 가 켜진다. 즉 **QEMU 쪽만 모르게 덜 시험하고 있었다.**

```
aarch64-zephyr-elf-gcc -mcpu=cortex-a53 → (없음)
aarch64-zephyr-elf-gcc -mcpu=cortex-a55 → __ARM_FEATURE_DOTPROD
```

### 고치는 데 또 하나 걸렸다 — 플래그 **순서**

`target_compile_options(app PRIVATE -mcpu=cortex-a55)` 로는 **안 됐다.**
컴파일 명령의 순서가 `[타깃 옵션] [인터페이스 옵션] [소스 옵션]` 이고
Zephyr 의 `-mcpu=cortex-a53` 이 `zephyr_interface` 의 INTERFACE 옵션으로
**뒤에** 붙는다. gcc 는 마지막 `-mcpu` 를 쓴다.

```
위치 44: -mcpu=cortex-a53   ← Zephyr (인터페이스)
위치 74: -mcpu=cortex-a55   ← 우리 (소스 수준) — 이것이 이긴다
```

그래서 `set_property(SOURCE ... COMPILE_OPTIONS)` 로 **소스 수준**에 준다.
`cmake/llm_ggml.cmake` 의 `llm_ggml_flags()` 가 그 일을 한다.
작은 cmake 프로젝트로 순서를 먼저 확인한 뒤 적용했다 — 2분짜리 빌드를
추측으로 돌리지 않았다.

## 그래프를 호스트와 **같은 파일**로 공유한다

`src/l4_graph.c` 하나다. `tools/l4_golden.sh` 가 호스트에서 **같은 파일**을
컴파일한다. 양쪽에 따로 쓰면 전사 오류가 생기고 그것을 "커널이 다르다" 로
오진한다.

입력은 전부 **첨자의 함수**다(`in_f32(i) = ((i*7919)%31 - 15) * 0.125`).
난수는 양쪽에서 같은 값을 보장할 수 없고, 0 으로 채우면 커널이 틀려도 통과한다.

호스트는 i8mm 을 **끈다**(`-Xclang -target-feature -Xclang -i8mm`). Apple M
계열에는 i8mm 이 있어 ggml 이 실기에 없는 커널 경로를 골랐을 것이다.

## 소스 목록은 단일 출처다

`cmake/ggml-sources.txt` 하나를 Zephyr cmake 와 호스트 스크립트가 **같이**
읽는다. 처음에 목록을 두 군데 두었다가 `tiled/`·`amx/` 를 한쪽에만 넣어
링크가 깨졌다. 한쪽만 바뀌면 **골든값과 결과가 다른 코드에서 나온다.**

⚠ `file(STRINGS)` 에 **`ENCODING UTF-8` 이 필수**다. 없으면 ASCII 로 읽어
한글 주석의 UTF-8 바이트를 구분자로 삼아 줄을 조각낸다 — 23개가 **108조각**이
되고 `ggml/** 를 못 찾는다` 같은 엉뚱한 오류로 나타난다. 실제로 겪었다.

## 또 하나 — 아레나가 요청과 같으면 실패한다

```
ggml_aligned_malloc: insufficient memory (attempted to allocate 4.00 MB)
ggml.c:1662: GGML_ASSERT(ctx->mem_buffer != NULL) failed
```

`ggml_init` 의 `mem_size` 를 **정렬 할당 한 덩이**로 받는다. 아레나가 요청과
같으면 정렬 여유가 없다. 요청의 여러 배로 둔다(지금 요청 4 MiB, 아레나 32 MiB).
ggml 의 진단 메시지가 정확해서 한 번에 찾았다.

## RoPE 만 비트 일치가 아닌 이유

256개 중 10개가 비트로 다르고 최대 상대오차가 **344 ppb**(= 3.4e-7)다.
계획서의 L4 허용 오차 **1e-5** 안이다. RoPE 는 `sin`/`cos` 를 쓰고 libm
구현이 호스트(Apple libm)와 Zephyr(picolibc)에서 다르다. 정수 언팩뿐인
역양자화(L3)와 달리 여기서 비트 일치를 요구하는 것은 옳지 않다 —
그래서 계획서가 L3 는 0, L4 는 1e-5 로 **층마다 다르게** 두었다.

출력에 최대 상대오차를 **ppb 로 찍는다.** 숫자를 봐야 "허용 범위 안" 인지
판단할 수 있고, `printk` 에 부동소수를 쓰지 않으려는 이유도 있다.

## 돌리는 법

```bash
./tools/l4_golden.sh                      # 호스트 골든값 (같은 l4_graph.c)
./tools/qemu_run.sh apps/llm_l4 180
```

블롭이 필요 없다 — 입력이 코드에 들어 있다.

## ✅ 실기 통과 — 2026-11-07, Radxa ZERO 3E (RK3566)

```
=== L4: ggml 커널 관문 ===
  [0a] sysconf 포팅 (장치 메모리 보고) ... OK — total 32768 KiB (멈추지 않았다)
  [0] 기능 집합  Zephyr dotprod=1 i8mm=0 / 호스트 1 0 ... OK
  [1] mm f32    128개 ... OK — **비트 일치**
  [2] mm Q4_0   128개 ... OK — **비트 일치**
  [3] RMSNorm   128개 ... OK — **비트 일치**
  [4] SiLU      128개 ... OK — **비트 일치**
  [5] RoPE      256개 ... OK — 비트는 10/256 다르나 최대 상대오차 344 ppb
=== L4 PASS (실패 0) ===
```

RAM 35,540 KB / 128 MB (27.11%) · 이미지 632,320 B.

### `[0a]` 가 실기에서 처음 돌았다 — **보드를 멈출 버그였다**

이 양성 대조는 `ggml_backend_cpu_device_get_memory()` 를 실제로 부른다. 그
함수가 `sysconf(_SC_PHYS_PAGES)` 와 `sysconf(_SC_PAGE_SIZE)` 를 연달아 부르고,
우리 포팅이 처음에는 **자기 자신을 무한 재귀**했다(objdump 로 `b <자기 주소>`
확인). L4·L5 가 그 함수를 부르지 않아 초록으로 통과하던 잠복 버그였고,
reviewer 가 "L6 에서 보드가 멈췄을 것" 이라 지적한 자리다.

**고친 코드가 실기에서 멈추지 않고 타당한 값(`total 32768 KiB` = 아레나
32 MiB)을 돌려주는 것을 확인했다.** 계기를 쓰지 않으면 잠복한다 — 그래서
양성 대조를 넣었고, 그것이 제값을 했다.

### QEMU 와 전부 같다

다섯 항목의 비트 일치 여부와 RoPE 의 **344 ppb** 까지 QEMU 와 동일하다.
`[2] mm Q4_0` 은 `ggml_vec_dot_q4_0_q8_0` 를 지나는 **진짜 추론 경로**이고
실기 dotprod 로 돌았다.
