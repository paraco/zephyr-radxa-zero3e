# Zephyr Hub LLM — 사다리 현황 (한 장 요약)

2026-11-07 기준. 자세한 것은 각 앱의 README 와
[llm-hub-test-plan.md](llm-hub-test-plan.md) 에 있다.

## 기준선 — 2026-11-07 전체 통과

```
$ ./tools/run_ladder.sh
  llm_l0: PASS   llm_l1: PASS   llm_l2: PASS   llm_l3: PASS
  llm_l4: PASS   llm_l5a: PASS  llm_l5b: PASS
통과 7 · 실패 0
```

소스 목록 단일화 · `-ffp-contract` 정책 · 호스트 하네스 일반화를 크게 바꾼
뒤 세운 기준선이다. 이후에는 연관 시험만 돌리고, 공통 부분을 또 바꾸면
이것을 다시 통과시킨다(전역 지침의 시험 순서).

## 어디까지 왔나

| 칸 | 상태 | 한 줄 |
|---|---|---|
| **L0** C++17 런타임 | ✅ QEMU | picolibc 로 STL·예외·pthread 가 된다. `std::thread` 는 없다(ggml 에 불필요) |
| **L1** 대용량 메모리 | ✅ QEMU | 창 밖 640 MiB 를 **C 코드 0줄**로 매핑. CRC 호스트와 일치 |
| **L2** GGUF 파서 | ✅ QEMU | 헤더·KV·텐서정보·정렬 6항목. 골든값이 참조 구현에서 나와 독립 |
| **L3** 역양자화 | ✅ QEMU | **비트 일치** 640개. `ggml-quants.c` 가 그대로 빌드된다 |
| **L4** ggml 커널 | ✅ QEMU | ggml 코어+CPU 백엔드 23개 파일. 커널 5개 통과 |
| **L5a** Gated DeltaNet | ✅ QEMU | 순환 상태 262,144개가 표본 1개 빼고 비트 일치 |
| **L5b** Gated Attention | ✅ QEMU | GQA 4:1 · 부분 mRoPE 64/256 확인 |
| **L6** 전진 순전파 | ⛔ | **상류 모델 그래프 필요** (llama.cpp `src/` 105,526줄) |
| **L7** 생성 | ⛔ | 같음 |
| **L8** 성능 | ⛔ | **하드웨어 전용** — QEMU 로 못 잰다 |

**L0~L5b 는 전부 합성 입력·합성 가중치다.** 가린 것은
**"ggml 이 호스트와 똑같이 계산하는가"** 이고,
**"llama.cpp 의 `qwen35` 경로가 옳은가" 는 아직 못 가렸다.**

## 가장 큰 결과 — B안이 성립한다

계획의 B안(ggml CPU 백엔드 + llama.cpp 의 모델 그래프만 포팅)에서 최대
미지수였던 **"ggml 의 C 가 Zephyr 에서 되는가"** 에 답이 나왔다. **된다.**

- ggml 코어 + CPU 백엔드 **23개 파일**(C/C++ 혼합)이 **상류 수정 0줄**로 빌드된다.
- 포팅에 필요한 것은 **세 가지**뿐이다.

| 항목 | 왜 | 어디에 |
|---|---|---|
| `ggml-version.h` | cmake 생성 파일 | `cmake/llm_ggml.cmake` |
| `std::mutex` 없음 | 툴체인 libstdc++ 가 gthreads 없이 빌드됐다 | `apps/llm_common/ggml_port.c` (k_mutex) |
| `M_PI` · `_SC_PHYS_PAGES` | picolibc 가 감춤 · Zephyr sysconf 에 없음 | `apps/llm_common/ggml_zephyr_compat.h` (`-include`) |

## 차이의 원인은 세 종류다 — 측정·실험으로 가렸다

| 경로 | 결과 | 왜 |
|---|---|---|
| **SIMD·intrinsic** | **비트 일치** | 양쪽이 같은 intrinsic. 컴파일러 재량이 없다 |
| **평범한 C 스칼라 루프** | `-ffp-contract` 고정하면 **비트 일치** | `a*b+c` 를 `fmadd` 로 묶으면 1 ulp 갈린다 |
| **libm** (`sin`·`cos`·`exp`·`sigmoid`) | **~1 ulp (94~392 ppb)** — 줄일 수 없다 | Apple libm 과 picolibc 구현이 다르다 |

`LLM_GGML_FP_CONTRACT` 가 knob 이다. 기본 `off`(재현성). **`off` 는 생산
설정이 아니다** — L8 은 `fast` 로 재고, 그때 L4·L5 를 다시 돌려 허용 오차
안인지 확인해야 한다.

## 메모리 배치 — 실측으로 닫혔다

| 무엇 | 크기 | 출처 |
|---|---|---|
| DeltaNet 순환 상태 | 층당 1 MiB × 18층 = **18 MiB** | L5a 실측 |
| KV 캐시 (2K 토큰) | 어텐션 6층 × 토큰당 2 KiB = **24 MiB** | `gguf_dump` 실측 |
| 로짓 | 약 **1 MiB** | |
| **창 안 합계** | **약 43 MiB** / 128 MB | |
| 가중치 (읽기 전용) | **507.9 MiB** — 창 밖 `0x60000000` | L1 에서 640 MiB 창 확인 |

## 사람이 해야 하는 일 (에이전트가 못 하는 것)

1. **실기 확인 3건** — L0 부팅 · 128 MB 창 · L2 의 캐시 항목.
   QEMU TCG 는 데이터 캐시를 모형화하지 않아 캐시 위험은 **QEMU 에서 실패할 수
   없다**. 양성 결과에 정보가 없다.
   ```
   python3 tools/ota_test.py --apply-try
   ```
2. **호스트 llama.cpp 전진 빌드** — L6·L7 의 골든값(`llama-eval-callback`)에
   필요하다. 샌드박스 분류기가 이 명령을 거부했다.
   ```
   cd ~/Projects/PiZZa/upstream/llama.cpp
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF
   cmake --build build -j8
   ```
3. **0단계 Linux 기준선** — 계획서 §124 의 경고(포크에서 "결과가 쓰레기" 보고)
   를 확인한다. 둘째 SD 카드가 필요하다. 첫 줄에 `asimddp` 확인을 넣는다.

## 쓰는 법

```bash
./tools/host_golden.sh llm_l5b          # 호스트 골든값 (앱과 같은 graph.c)
./tools/qemu_run.sh apps/llm_l5b 300    # 빌드 + QEMU + PASS/FAIL 판정
```

블롭이 필요한 칸은 `L_BLOB=<파일>` 을 앞에 붙인다. 빼먹으면 하네스가
**돌리기 전에** 경고한다(전부 0 을 읽어 그럴듯한 값이 나오는 오진을 막는다).
