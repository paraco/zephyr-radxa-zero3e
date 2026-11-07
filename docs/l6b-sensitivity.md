# L6b — 이 구조는 1 ulp 를 10% 로 증폭한다

2026-11-07. L6b 의 판정 기준을 바꾼 근거.

## 처음 본 것

보드의 층별 합이 호스트 골든과 이렇게 갈렸다:

```
l_out-0 ~ l_out-15 ...... 0~14 ppb      ← 사실상 비트 일치
l_out-16 ·  17 .......... 29 · 26 ppb
l_out-18 ................ 15,063 ppb
l_out-19 ~ l_out-23 ..... 324,543 ~ 1,003,158 ppb
result_output(로짓) ..... 17,152,162 ppb (1.7%)
```

"포팅이 어디서 틀렸나" 로 읽고 원인을 찾기 시작했다. 먼저 지운 것들:

| 가설 | 어떻게 지웠나 |
|---|---|
| 층마다 다른 코드를 탄다 | `qwen35.cpp` 에 층별 분기가 **없다**(마지막 층 제외). 24개 층이 같은 코드다 |
| 임계 뒤집힘(clamp) | `swiglu_clamp` 를 쓰지 않고 `f_clamp_kqv = 0.0` 이다 |
| 최적화 수준 (`-O3` vs `-O2`) | 호스트 두 빌드가 **32/32 비트 일치**. 원인이 아니다 |

## 그 다음 — 민감도를 쟀다

⚠ `-O3`/`-O2` 가 비트 일치한 것은 "민감하지 않다" 가 **아니다.** 교란이
들어가지 않았다는 뜻이다(같은 순서로 같은 계산을 했다). 처음에 그렇게
결론 냈다가 고쳤다.

그래서 **실제로 값을 흔드는** 변경으로 다시 쟀다 — `-ffp-contract=fast` 는
FMA 축약으로 곱-합마다 **~1 ulp** 를 넣는다. 다른 조건은 전부 같다.

| 지점 | FMA 교란(~1 ulp) | **우리 보드** |
|---|---|---|
| `model.input_embed` ~ `l_out-1` | 0 ppb | 0~14 ppb |
| `l_out-2` ~ `l_out-8` | 1,000 ~ 3,000 ppb | 0~3 ppb |
| **`l_out-9`** | **5,738,985 ppb (0.57%)** | 0 ppb |
| `l_out-10` | 26,506,233 ppb (2.7%) | 0 ppb |
| `l_out-11` ~ `l_out-14` | 1e6 ~ 6e7 ppb | 0~4 ppb |
| **`l_out-15`** | **508,390,575 ppb (50.8%)** | **1 ppb** |
| `l_out-16` ~ `l_out-23` | 8e6 ~ 3.4e8 ppb | 26 ~ 1,003,158 ppb |
| `result_output` | **43,026,374 ppb (4.3%)** | 17,152,162 ppb (1.7%) |

## 결론

**이 구조(Qwen3.5 DeltaNet)는 곱-합 하나의 1 ulp 를 층 9에서 0.57%,
층 15에서 50.8% 로 증폭한다.** 24층 중 18층이 DeltaNet 선형 재귀이고,
`sigmoid`·`silu`·`softplus` 가 매 층에 들어간다.

그 바닥에 비하면 **우리 보드는 2~5 자리 더 정확하다.** 층 15까지 1 ppb 이고
로짓도 FMA 빌드보다 골든에 **2.5배 가깝다**.

즉 **층 16 이후의 갈림은 포팅 결함의 증거가 되지 못한다.** 그 구간에서는
합 비교로 어떤 구현 차이도 가릴 수 없다 — 1 ulp 면 충분히 설명된다.
남은 차이의 출처는 libm 이다(`sigmoid`·`silu`·`softplus`; L5a 에서
94~392 ppb 로 재 둔 그것).

## 그래서 판정 기준을 이렇게 바꾼다

| 구간 | 기준 | 왜 |
|---|---|---|
| `l_out-0` ~ `l_out-15` | **1,000 ppb 이내** (지금 0~14 ppb, 여유 70배) | 이 구간이 **실제 정확성 관문**이다. FMA 교란도 이 구간 앞쪽은 1,000~3,000 ppb 에 머문다 |
| `l_out-16` ~ `result_norm` | **보고만** 한다 (판정에 넣지 않는다) | 1 ulp 가 1e8 ppb 를 만드는 구간이다. 여기서 FAIL 은 아무것도 뜻하지 않는다 |
| 로짓 | **argmax 토큰 일치** | 추론의 결과를 결정하는 유일한 양이다. 합이 1.7% 달라도 고르는 토큰이 같으면 **쓸 수 있는 포팅**이다 |

⚠ **허용 오차를 "맞을 때까지" 키운 것이 아니다.** 구간을 나누고 각 구간에
맞는 양을 쟀다. 층 0~15 의 기준은 **더 엄격해졌다**(20,000 → 1,000 ppb).

⚠ 이 결론은 **이 모델**에 대한 것이다. 증폭은 DeltaNet 재귀의 성질이므로
다른 구조에서는 다시 재야 한다. `tools/l6b_cmp_golden.py` 가 그 측정을
한다 — `-ffp-contract=off` 와 `=fast` 골든을 비교하면 그 구조의 민감도
바닥이 나온다.

## 재현

```bash
cmake -S upstream/llama.cpp -B upstream/llama.cpp/build-golden-fma \
  -DCMAKE_BUILD_TYPE=Release -DGGML_METAL=OFF -DGGML_BLAS=OFF \
  -DGGML_ACCELERATE=OFF -DGGML_CPU_REPACK=OFF -DGGML_LLAMAFILE=OFF \
  -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 \
  -DCMAKE_C_FLAGS=-ffp-contract=fast -DCMAKE_CXX_FLAGS=-ffp-contract=fast \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build upstream/llama.cpp/build-golden-fma -j --target llama-eval-callback
```

```bash
python3 rk3566/tools/l6b_cmp_golden.py /tmp/claude-501/golden-O3.inc /tmp/claude-501/golden-FMA.inc
```
