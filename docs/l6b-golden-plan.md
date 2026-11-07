# L6b 호스트 골든값 — **기존 llama.cpp 빌드를 쓰면 안 된다**

2026-11-07. L6a 통과 직후 L6b 를 준비하면서 드러난 문제.

## 무엇이 문제인가

L6b 는 "한 토큰 순전파의 층별 텐서가 호스트와 맞는가" 를 가린다. 골든값
출처로 `llama-eval-callback` 을 쓰려 했는데, **지금 호스트 빌드가 보드와
전혀 다른 코드다.** 그 바이너리가 스스로 찍는다:

```
system_info: ... | MTL : EMBED_LIBRARY = 1 | CPU : NEON = 1 | ARM_FMA = 1 |
             FP16_VA = 1 | MATMUL_INT8 = 1 | DOTPROD = 1 | ACCELERATE = 1 |
             REPACK = 1 |
```

| 항목 | 호스트 기존 빌드 | 우리 보드 빌드 |
|---|---|---|
| `__ARM_FEATURE_MATMUL_INT8` (i8mm) | **ON** | 없다 (A55) |
| `GGML_USE_CPU_REPACK` | **ON** | OFF (결정 1) |
| Accelerate / BLAS | **ON** | 없다 |
| Metal | 빌드에 포함 | 없다 |
| `-ffp-contract` | 기본(fast) | `off` |

`--device none -ngl 0` 은 Metal 을 떼지만 **i8mm · repack · Accelerate 는 그대로다.**
그 다섯 가지가 전부 matmul 수치에 영향을 준다. 이대로 비교하면 불일치가
나와도 **포팅 결함인지 빌드 차이인지 가릴 수 없다** — 이 프로젝트가 L3·L4·L5
에서 피해 온 바로 그 함정이다.

(참고: `cmake/llm_ggml.cmake` 의 `-E -dM` 측정에서 "호스트에 i8mm 이 없다" 고
확인한 것은 **플래그 없는 clang** 이었다. llama.cpp 의 cmake 는
`GGML_MACHINE_SUPPORTS_i8mm` 를 검사해 `-march=…+i8mm` 를 **명시로 켠다**
(`build/CMakeCache.txt`). 그래서 `tools/host_golden.sh:87` 의 명시적
`-Xclang -target-feature -Xclang -i8mm` 가 실제로 일을 하고 있었다.)

## 해법 — 보드와 **같은 기능 집합**으로 두 번째 호스트 빌드

우리 손으로 덤퍼를 쓰지 않는다. 상류의 `llama-eval-callback` 을 **보드와 같은
플래그로 다시 빌드**해 쓴다. 상류의 덤프 코드를 그대로 쓰므로 형식도,
무엇을 어디서 찍는지도 갈리지 않는다.

```bash
cmake -S upstream/llama.cpp -B upstream/llama.cpp/build-golden \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF \
  -DGGML_CPU_REPACK=OFF \
  -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod \
  -DCMAKE_C_FLAGS=-ffp-contract=off \
  -DCMAKE_CXX_FLAGS=-ffp-contract=off \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build upstream/llama.cpp/build-golden -j --target llama-eval-callback
```

- `GGML_NATIVE=OFF` + `GGML_CPU_ARM_ARCH=armv8.2-a+dotprod` →
  `-march=armv8.2-a+dotprod` (`ggml-cpu/CMakeLists.txt:210-211`). i8mm 없이
  dotprod 만 — **A55 와 같다.**
- `GGML_CPU_REPACK=OFF` → `GGML_USE_CPU_REPACK` 가 정의되지 않는다
  (`ggml-cpu/CMakeLists.txt:626-627`). 보드와 같다.
- `-ffp-contract=off` → `LLM_GGML_FP_CONTRACT` 와 같다.

**검증 관문:** 새 바이너리의 `system_info` 줄에
`MATMUL_INT8 = 1` · `REPACK = 1` · `ACCELERATE = 1` 이 **없어야 한다.**
있으면 플래그가 안 먹은 것이고, 골든값을 믿을 수 없다. L6b 하네스가 이것을
먼저 확인하고 아니면 멈춘다.

## 출력 형식

```
common_debug_cb_eval:   model.input_embed = (f32) GET_ROWS(token_embd.weight{1024, 248320, 1, 1}, inp_tokens{1, 1, 1, 1}}) = {1024, 1, 1, 1}
    ...
    sum = -0.762861
```

`sum` 줄이 우리가 쓰는 값이다 — `apps/llm_common/llm_fullsum.h` 의
`llm_fullsum_of()` 가 보드에서 같은 양을 낸다. L5a·L5b 가 쓰는 방식 그대로다.

⚠ `sum` 은 **상쇄가 있는 합**이라 작은 값에서 상대오차가 커 보인다.
L5b 에서 그래서 `|합|`(abssum)을 함께 쓰기로 했다. eval-callback 은 `sum` 만
찍으므로, abssum 이 필요하면 **보드 쪽 수치를 호스트의 `sum` 과 맞추는 방향**
으로 쓰고 abssum 은 보드 내부의 비-NaN 확인용으로 둔다.

## 범위

L6b 는 **첫 토큰 하나**다. 토크나이저·샘플러는 쓰지 않고 토큰 id 를 고정한다.
층 24개를 전부 보지 않고 먼저 다음 지점만 본다:

1. `model.input_embed` (GET_ROWS) — 가중치가 제자리에서 제대로 읽히는가
2. 첫 층의 RMS norm 출력
3. 첫 어텐션 층(layer 3 — qwen35 는 3·7·11·15·19·23 이 어텐션이다)의 출력
4. 첫 DeltaNet 층(layer 0)의 출력
5. 마지막 층 출력
6. `result_output` (로짓)

하나라도 어긋나면 그 사이를 좁힌다.

---

## ✅ 골든 경로 완성 — 2026-11-07

`build-golden` 의 `system_info` 가 관문을 통과했다:

```
CPU : NEON = 1 | ARM_FMA = 1 | FP16_VA = 1 | DOTPROD = 1 |
```

`MATMUL_INT8` · `REPACK` · `ACCELERATE` · `LLAMAFILE` · `MTL` · `BLAS` 가
**전부 없다.** ggml 이 실제로 보는 ARM 매크로 8개가 보드와 일치한다
(`SVE`·`SVE2`·`SME`·`SME2`·`MATMUL_INT8` = 0, `DOTPROD`·`FP16_VECTOR_ARITHMETIC`
·`FMA` = 1).

처음 설정에서 **두 가지가 어긋났고 고쳤다**:

| 어긋남 | 왜 중요한가 | 고침 |
|---|---|---|
| `FP16_VECTOR_ARITHMETIC` 호스트 OFF · 보드 ON | ggml 이 **10곳**에서 이 매크로로 갈린다 (`ggml/src` 전수 조사) | `-DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16` |
| `LLAMAFILE` 호스트 ON · 보드 OFF | `ggml-cpu.c:1310,1395` 의 matmul 경로를 바꾼다 | `-DGGML_LLAMAFILE=OFF` |

### 뽑은 값 — `apps/llm_l6b/golden.inc`

토큰 **5834** (`llama-tokenize -p "hi" --ids`). 노드 전체 **1,017개** 중
대조 지점 9개:

| 노드 | 연산 | sum |
|---|---|---|
| `model.input_embed` | GET_ROWS | -0.762861 |
| `norm-0` | RMS_NORM | -35.549904 |
| `attn_norm-0` | MUL | -43.128544 |
| `l_out-0` | ADD | -0.245448 |
| `l_out-3` | ADD | 0.605366 |
| `l_out-23` | ADD | 10.362761 |
| `norm` | RMS_NORM | 34.496681 |
| `result_norm` | MUL | 109.291367 |
| **`result_output`** | **MUL_MAT** | **-565930.500000** |

`l_out-0` 은 **DeltaNet** 층이고 `l_out-3` 은 **어텐션** 층이다 — qwen35 의
어텐션은 3·7·11·15·19·23 이다(측정값, `docs/llm-hub-test-plan.md`). 두 갈래를
각각 하나씩 본다.

`result_output` 이 로짓이므로 **끝까지 한 번에 가린다.** 어긋나면 위
지점들로 좁힌다.

### 되돌릴 수 없는 함정 하나

`tools/l6b_golden.sh` 는 `system_info` 관문을 **먼저** 돌리고 아니면 멈춘다.
이것을 건너뛰면 **잘못된 골든값을 조용히** 쓴다 — 플래그가 안 먹은 빌드는
겉보기에 똑같이 동작한다. 이 프로젝트에서 가장 비싸게 배운 종류의 실수다.
