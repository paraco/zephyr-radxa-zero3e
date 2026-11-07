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

소스 목록 단일화 · `-ffp-contract` 정책 · 호스트 하네스 일반화, 그리고
reviewer 지적 5건 수정 뒤 세운 기준선이다. 이후에는 연관 시험만 돌리고, 공통 부분을 또 바꾸면
이것을 다시 통과시킨다(전역 지침의 시험 순서).

## ✅ 실기 확인 — **모델 없이 되는 모든 칸이 닫혔다** (2026-11-07)

L0 · L1 · L2 · L3 · L4 · L5a · L5b — **일곱 칸 전부 실기 통과.**

`moru ota apply try` 로 올려 확인했다. 카드는 건드리지 않았다.

| 칸 | 실기 결과 | QEMU 와 비교 |
|---|---|---|
| **L0** | PASS · RAM 0.62% · 예외 전개·pthread 가 실기 EL1 에서 동작 | **네 항목 전부 같다** |
| **L2** | PASS · **캐시 잔여 위험 닫힘** — 블롭의 매직·CRC 가 온전했다 | QEMU 는 이 항목을 **구조적으로 못 가린다** |
| **L3** | PASS · **비트 일치 640개** (음의 0 포함) | **동일** |
| **L6a** | (아직) | **PASS — 실제 507.85 MiB 모델, 제자리 적재, 어휘 248,320** |
| **L1** | PASS · 창 512 MiB 진짜 RAM · **쓰기 235 MiB/s** · CRC 호스트와 일치 | 대역폭만 다르다(QEMU 1,777 MiB/s 는 무의미) |
| **L4** | PASS · RAM 27.11% · **`[0a]` sysconf 포팅이 실기에서 멈추지 않았다** | **숫자까지 전부 같다** |
| **L5a** | PASS · RAM **77.15%** · 순환 상태 1 MiB/층 | **숫자까지 전부 같다** |
| **L5b** | PASS · `dotprod=1 i8mm=0` · RAM 39.67% | **숫자까지 전부 같다** |

L4 의 `[0a]` 는 특별하다 — `ggml_backend_cpu_device_get_memory()` 를 실제로
불러 **보드를 멈출 잠복 버그**(sysconf 무한 재귀)가 고쳐졌음을 실기에서
확인했다. reviewer 가 "L6 에서 터졌을 것" 이라 지적한 자리다.

### 가장 큰 결론 — **QEMU 가 이 계층에서 실기를 충실히 대신한다**

L5a·L5b 의 **CRC·불일치 개수·최대 상대오차·전체 요약이 하나도 다르지 않다.**
즉 "QEMU 에서 개발하고 실기로 확인한다" 는 계획의 전제가 실측으로 닫혔다.

예외는 **성능**뿐이다. 그래서 L8 이 하드웨어 전용인 것도 맞았다.

### 실기에서만 알 수 있던 것 셋

| | |
|---|---|
| `dotprod` 가 진짜 Cortex-A55 에서 켜진다 | `dotprod=1 i8mm=0` — 호스트와 같은 기능 집합 |
| **쓰기 대역폭 235 MiB/s** | 507.85 MiB 모델 적재에 약 2.2초 |
| ⚠ **소프트웨어 CRC32 가 11.8 MiB/s** | 512 MiB 에 **43.3초.** QEMU 보다 **12배 느리다** — 모델 적재 검증에 전체 CRC 를 쓸 수 없다 |

### ⚠ 관측 경로를 두 번 틀렸다

보드 defconfig 의 `CONFIG_LOG=y` 가 앱의 `printk` 를 1 KB 지연 버퍼로
우회시킨다. QEMU 보드에는 `CONFIG_LOG` 이 꺼져 있어 **같은 앱이 두 보드에서
다른 출력 경로를 갖는다.**

1. 기본(`LOG_PRINTK=y`) → 통과했으나 **출력 꼬리만** 남았다
2. `LOG_PRINTK=n` → **보드가 멈췄다** (UART2 에 쓰는 주체가 둘이 되어 교착)
3. `LOG_MODE_IMMEDIATE=y` → **전체 출력** (쓰는 주체 하나, 버퍼·스레드 없음)

사다리 일곱 앱 전부에 3번을 적용했다. 자세한 것은
[bringup-rules.md](bringup-rules.md) 의 사례.

---

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

## ✅ 호스트 llama.cpp 빌드 완료 (사용자가 돌렸다) — L6 전제가 풀렸다

`llama-cli` · `llama-eval-callback` · `llama-bench` · `llama-perplexity` 생성됨.
`build : b1-6c01da6`.

### 계획서 §124 의 걱정은 **재현되지 않았다**

포크에서 "qwen35 결과가 쓰레기" 보고가 있었다는 항목이다. 호스트에서 확인했다:

```
$ llama-cli -m Qwen3.5-0.8B-Q4_K_M.gguf -p "The capital city of France is"     -n 24 --temp 0 -s 1 -t 4 -st
> The capital city of France is
[Start thinking]
Thinking Process:
1.  **Analyze the Request:**
    *   Question: "The capital city of
```

**멀쩡한 출력이고 결정적이다.** 같은 시드·greedy 로 두 번 돌려 생성 토큰이
같았다. CPU 전용(`-ngl 0 --device none`)과 Metal 실행이 24토큰에서 **같은
토큰 열**을 냈다 — 계획서의 L7 판정 기준(토큰 열 일치)이 호스트 안에서 이미
성립한다.

⚠ 처음에 "결정적이지 않다" 고 판정했는데 **제 측정 오류**였다 — 로딩
스피너 애니메이션과 t/s 줄까지 비교했다. 생성 토큰만 추려 비교하면 같다.

### 하이퍼파라미터가 내 실측과 **정확히** 맞는다

```
arch = qwen35 · n_layer 24 · n_embd 1024 · n_head 8
ssm_d_conv 4 · ssm_d_inner 2048 · ssm_d_state 128 · ssm_dt_rank 16 · ssm_n_group 16
n_ctx_train 262144 · model type 0.8B · model params 752.39 M
token_embd.weight{1024, 248320}   ← vocab 248,320 확인
```

`gguf_dump` 로 읽은 값과 구현이 보고하는 값이 전부 일치한다. 파라미터 수가
safetensors 의 873 M 보다 작은 것은 GGUF 본체에 **비전 타워가 없기** 때문이다
— 텍스트 전용이라는 또 하나의 증거다(`modalities : text` 도 찍힌다).

### 층 구성이 **구현으로** 확인됐다 — 추론이 아니다

`llama-eval-callback` 이 이름 붙은 중간 텐서를 층 번호까지 찍는다:

```
어텐션 (Qcur_full-N):  3  7  11  15  19  23          →  6층
DeltaNet (conv_input-N): 0 1 2  4 5 6  8 9 10  12 13 14  16 17 18  20 21 22  → 18층
```

**`6 × (3 DeltaNet → 1 Attention)`** 이다. 계획서의 가정이 맞았다.

### L6 골든값 장치가 작동한다

노드 **1,431개**를 덤프했고 각 노드마다 이름·연산·형태·값 표본·**`sum`** 을
찍는다(제가 `llm_fullsum.h` 에 넣은 상한과 같은 발상이다).

쓸 수 있는 골든 지점(층 번호 생략):

| 계층 | 이름 |
|---|---|
| DeltaNet | `linear_attn_qkv_mixed` · `qkv_mixed_transposed` · `conv_input` · `conv_states` · `conv_output_raw` · **`conv_output_silu`** · `q_conv`/`k_conv` · `q_conv_predelta`/`k_conv_predelta`/`v_conv_predelta` · `beta`/`beta_sigmoid` · `alpha`/`a_softplus` · `gate` · `z` · `state_predelta` · **`new_state`** · `final_output` · `linear_attn_out` |
| 어텐션 | `Qcur_full` · `Qcur_reshaped` · `Qcur_normed` · `Kcur_normed` · `gate_reshaped` · `gate_sigmoid` · `attn_pregate` · `attn_gated` · `attn_output` |
| 공통 | `attn_norm` · `attn_post_norm` · `attn_residual` · `ffn_up`/`ffn_gate`/`ffn_swiglu`/`ffn_out` · `l_out` · `norm` · `result_norm` · `result_output` |

**즉 L5a·L5b 를 합성 가중치에서 실제 가중치로 올릴 수 있다.** 지금의 합성판은
충실한 부분집합이지만 빠진 것이 있다 — 목록을 보면 `conv_output_silu` 가
있는데 내 L5a 그래프에는 **conv 뒤 SiLU 가 없다.** `linear_attn_qkv_mixed`
(결합 QKVZ 투영)도 없다. 실제 가중치판에서 채워야 한다.

### ⚠ 골든값을 뽑을 때 반드시 지킬 것 둘

**① CPU 를 강제한다.** macOS 기본 빌드는 **Metal 을 켠다.** 그냥 돌리면
가중치 497 MiB 가 `MTL0` 버퍼로 가고 순환층도 `dev = MTL0` 다. 그러면
골든값이 **Metal 커널**에서 나온다 — 우리가 포팅하는 CPU 백엔드와 다른
구현이다. `-ngl 0 --device none` 을 반드시 준다.

**② repack 이 이 빌드에서 켜져 있다.** `system_info: … REPACK = 1` 이고
`CPU_REPACK` 이 선호 버퍼 타입이다. 즉 호스트 CPU 골든값은 **repack 경로**에서
나오는데 우리 Zephyr 빌드는 repack 이 꺼져 있다. **다른 코드다.**
아래 열린 결정 ①이 이론이 아니라 구체적인 문제가 됐다.

### Gate 0 은 **절반만** 끝났다

계획서의 0단계는 "보드에 Linux 를 올려 확인" 이다. 지금 답한 것은
**정확성 절반**(qwen35 가 0.8B 에서 멀쩡한가)이고, 호스트는 macOS arm64 다.
**보드 성능·보드 커널 경로는 여전히 둘째 SD 카드가 필요하다.**

---

## ⚠ L6 전에 결정해야 하는 것 둘 (reviewer 지적, 아직 안 고쳤다)

둘 다 **지금 틀린 것은 아니지만** L6 에서 드러난다. 설계 판단이 필요해
남겨 둔다.

### ① `GGML_USE_CPU_REPACK` 를 켤 것인가

지금 정의하지 않는다. 그래서 `ggml-cpu.cpp` 의 repack 버퍼 타입이 등록되지
않고 `arch/arm/repack.cpp` 는 **컴파일만 되고 죽은 코드**다.

- 상류 기본값은 **ON** 이다(`ggml/CMakeLists.txt:152`).
- 즉 llama.cpp 가 aarch64 에서 **실제로 쓰는 Q4_0 경로**를 우리는 시험하지
  않고 있다. L4 의 "진짜 추론 경로" 라는 주석은 그만큼 과장이다.
- 켜면 호스트·Zephyr 양쪽에 똑같이 켜야 한다(불변식 2).

**판단이 필요한 이유:** repack 은 가중치를 적재 시 재배치하므로 메모리 배치
(창 밖 읽기 전용 가중치)와 상호작용한다. 성능에는 유리하고 정확성 비교에는
변수가 하나 늘어난다.

### ② `ggml-backend-reg.cpp` 를 소스 목록에 넣을 것인가

지금 없다. 상류 `ggml/src/CMakeLists.txt:359-360` 의 백엔드 레지스트리다.
llama.cpp 가 `ggml_backend_dev_*` 로 쓰므로 **L6 에서 링크가 깨진다.**
소리가 나므로 위험은 낮지만, 넣을 때 `GGML_BACKEND_DL` 관련 설정을 함께
정해야 한다.

---

## ✅ 실기 확인 — sram 창 128 MiB 부팅 (2026-11-07)

`moru ota apply try` 로 올려 확인했다. 카드는 건드리지 않았다(한 번만 뛰는
슬롯이고 전원 재투입하면 기존 1 MiB moru 로 돌아온다).

```
[  0.000097] MoruLab Zephyr RTOS 0.1.0 (build 2a92f7d8e6ba)
[  0.000101] Pre-kernel: 7.336 s (power-on → PRE_KERNEL_1)
[  0.000109] Memory: 0x40000000 + 131072 KiB (zephyr,sram)   ← 이것이 핵심
...
[  0.054044] MoruLab Zephyr RTOS is up
```

**부팅 로그만으로 판정하지 않았다** — 셸이 실제로 응답하는 것까지 확인했다
(새 도구 `tools/moru_q.py`, 아무것도 전송하지 않고 상태만 읽는다):

```
moru info     → 메모리 0x40000000 + 131072 KiB · 가동 0d 00:00:51
kernel heap   → free 8192 · allocated 0
kernel uptime → 53210 ms
```

| 항목 | 창 1 MiB | **창 128 MiB** |
|---|---|---|
| Pre-kernel | 7.282 s | **7.336 s** (+54 ms) |
| USB 열거 | OK (speed 2 = HS) | **OK, 같다** |
| EP 설정 | `DALEPENA=0x000002bf` | **같다** |
| OTP 칩 ID | 읽힘 | **읽힘** |
| 폴트·조용한 멈춤 | 없음 | **없음** |

페이지 프레임 장부가 `.bss` 에서 252 KiB 늘었지만 **초기화 시간 대가는 거의
없고 이미지 크기는 그대로**(187,768 B)다.

### ✅ 카드에 영구 적용 완료 (2026-11-07)

`moru ota apply`(`try` 없이)로 썼다. 적용 뒤 확인:

```
버전   0.1.0 (build 2a92f7d8e6ba)
메모리 0x40000000 + 131072 KiB (zephyr,sram)
```

**이것이 카드에서 부팅한 것이라는 근거:** `zephyr.bin.try` 는 그 앞 부팅에서
지워졌고(`boot.cmd` 가 뛰기 전에 삭제한다) 매직도 적용 직후 지워진다. 따라서
이 부팅의 출처는 **카드의 `zephyr.bin` 뿐**이다. 그것이 `131072 KiB` 를
찍었으므로 카드가 새 이미지다.

⚠ **U-Boot 의 `applied`·`backup` 메시지는 읽을 수 없다** — U-Boot 는
1.5 Mbps 로 찍고 쓰는 CP2102 는 1 Mbps 한계라 115200 tio 에서 깨진다.
그래서 위처럼 **논리로 닫았다.**

**롤백:** 카드에 `zephyr.bin.old`(이전 1 MiB 판)가 남아 있다. 되돌리려면
카드 리더로 이름을 바꾼다 — 보드에서는 되돌릴 수 없다.

#### 전송 상태는 재부팅으로 사라진다

`apply try` 로 한 번 부팅한 뒤 `moru ota status` 가 "수신한 적이 없다" 였다.
`moru_ota` 장부가 RAM 에 있고 재부팅으로 초기화되기 때문이다 — **영구 적용
전에 다시 전송해야 한다.** 실제로 그래서 두 번 전송했다.

---

## L2·L3 실기 준비 완료 — 카드 작업 한 번 남았다 (2026-11-07)

L2(GGUF 파서)·L3(역양자화)는 블롭을 읽는다. QEMU 는 `-device loader` 로
넣지만 실기에는 그 경로가 없었다 — `boot.cmd` 에 넣었다.

```
if fatload ${devtype} ${devnum}:${distro_bootpart} 0x58000000 blob.bin; then
	echo "=== blob.bin -> 0x58000000 (${filesize} bytes) ==="
else
	echo "=== blob.bin 없음 — 블롭 없이 계속한다 ==="
fi
```

- **있으면 올리고 없으면 넘어간다** — 블롭을 안 쓰는 앱(L0·L1·L4·L5)과 moru 가
  이 줄 때문에 부팅을 못 하면 안 된다.
- `fatload` 는 크기 인자가 없으면 **파일 전체**를 읽는다. `fatwrite` 와 달리
  고정 크기가 필요 없다.

### 주소가 두 파일에 있다 — build.sh 가 대조한다

`sdcard/boot.cmd` 의 `fatload` 와 `apps/llm_common/model_blob.overlay` 의
`reg` 가 같아야 한다. 어긋나면 U-Boot 가 엉뚱한 곳에 올리고 앱은 창을 읽어
**전부 0** 이 된다 — 역양자화 결과가 ±0.0 이라 그럴듯해 보여 "포팅이 틀렸다"
로 오진한다(실제로 겪었다). `build.sh` 가 빌드마다 DT 를 정본으로 대조하고
어긋나면 **빌드를 멈춘다**:

```
블롭 주소: 0x58000000 (DT = boot.cmd) ✓
```

### 넣을 파일 (카드 작업 한 번)

| 파일 | 크기 | CRC32 |
|---|---|---|
| `boot.scr` | 9,496 B | `12446d0f` |
| `blob.bin` (L2·L3 시험 GGUF) | 3,872 B | `8320cd20` |

```bash
cd ~/Projects/PiZZa/rk3566/sdcard && \
  sudo mcopy -i /dev/rdisk4s2 -o boot.scr blob.bin ::/ && \
  sudo mdir -i /dev/rdisk4s2 ::/ && diskutil eject /dev/disk4
```

되돌리기: `boot.scr.known-good`(5,478 B)이 옆에 있다.

실기 이미지도 준비됐다 — L2 **74,344 B** `023eb0bc` · L3 **74,344 B** `dfd84b66`.

### ⚠ 실제 모델은 이 파티션에 **안 들어간다**

| | 크기 |
|---|---|
| boot 파티션 (`mmc 1:2`) | **314.6 MB** |
| 시험 GGUF | 3,872 B ✅ |
| **실제 모델** (Qwen3.5-0.8B Q4_K_M) | **507.85 MiB ❌** |

카드에 15.6 GB 파티션(`disk4s3`)이 따로 있다. L6·L7 에서는 거기서 읽도록
`fatload mmc 1:3 …` 를 쓰거나 다른 수단을 정해야 한다 — **그 파티션이 FAT
인지 아직 확인하지 않았다.** 열린 항목으로 남긴다.

---

## 사람이 해야 하는 일 (에이전트가 못 하는 것)

1. **실기 확인 3건** — L0 부팅 · 128 MB 창 · L2 의 캐시 항목.
   QEMU TCG 는 데이터 캐시를 모형화하지 않아 캐시 위험은 **QEMU 에서 실패할 수
   없다**. 양성 결과에 정보가 없다.

   ⚠ `ota_test.py --apply-try` 라는 플래그는 **없다**(내가 틀리게 적었다).
   `ota_test.py` 는 **RAM 으로 전송만** 하고, 적용은 **보드 셸 명령**이다.

   **순서 (보드가 평소 `moru` 로 떠 있어야 한다):**

   ```bash
   # ⓪ 먼저 UART2 를 열어 살아 있는지 확인한다. L0~L3 은 USB 가 없어 출력이
   #    UART2 로만 나온다(보드 DTS 의 zephyr,console = &uart2). 젬퍼가 빠져
   #    있으면 "조용한 죽음" 과 구분이 안 된다 — 실제로 그렇게 오진했다.
   tio /dev/cu.usbserial-0001 -b 115200 -L --log-file /tmp/hw.log --log-strip
   ```

   ```bash
   # ① RAM 으로 전송 + CRC 검증.  ⚠ tio 로 셸 포트를 열어 두면 스크립트가
   #    포트를 못 연다 — 셸 포트 쪽 tio 는 먼저 닫는다 (ctrl-t q).
   python3 tools/ota_test.py /tmp/claude-501/q-llm_l0/zephyr/zephyr.bin
   ```

   ```
   ② 보드 셸(USB CDC)에서:  moru ota apply try
   ```

   멈춰도 전원 재투입으로 `moru` 로 돌아온다 — `boot.cmd` 가 **뛰기 전에**
   `zephyr.bin.try` 를 지운다.

### ✅ OTA 슬롯을 **8 MiB** 로 키웠다 (2026-11-07) — 사다리 전체가 들어간다

전에는 `256 KiB` 고정이라 ggml 이 들어간 L4 이후를 실기에서 돌릴 수 없었다.

| 앱 | 크기 | 8 MiB 슬롯 |
|---|---|---|
| `llm_l0` | 101 KiB | ✅ 여유 7.9 MiB |
| `llm_l1`·`llm_l2`·`llm_l3` | 40~44 KiB | ✅ 여유 8.0 MiB |
| `llm_l4` | 585 KiB | ✅ 여유 7.4 MiB |
| `llm_l5a` | 593 KiB | ✅ 여유 7.4 MiB |
| `llm_l5b` | 605 KiB | ✅ 여유 7.4 MiB |

**왜 8 MiB 인가:** 슬롯을 바꿀 때마다 **카드 리더 작업이 필요하다**(OTA 는
`zephyr.bin` 만 쓰고 `boot.scr` 은 못 쓴다). 그래서 한 번에 넉넉히 잡았다 —
L6 은 ggml + llama.cpp 모델 그래프라 몇 MB 가 될 수 있다. 영역은
`0x50000000 + 8 MiB → 0x50800000` 이고 모델 창(`0x58000000`)까지 112 MiB
여유다. TRM Table 1-1 로 이 구간에 아페처가 없음을 확인했다.

**왜 고정 크기인가:** 이 U-Boot 에 **`setexpr` 가 없다.** 헤더에 적힌 실제
크기를 env 변수로 읽을 수단이 없어 `fatwrite` 에 상수를 줘야 한다.
대가는 이미지가 작아도 매번 8 MiB 를 쓰는 것(SD 에 몇 초)이다.

#### ⚠ 같은 숫자가 세 군데 있었다 — 단일 출처로 묶었다

`boot.cmd`(U-Boot) · `moru_shell.c`(펌웨어 가드) · `build.sh`(빌드 보고).
어긋나면 **이미지가 조용히 잘려 부팅이 멈춘다.**

- `build.sh` 는 이제 **`boot.cmd` 에서 읽어 온다** — 하드코딩을 없앴다.
- `moru_shell.c` 는 값을 가질 수밖에 없다(펌웨어는 카드를 읽지 못한다).
  그래서 **256 KiB 를 넘는 요청에 경고**한다 — "카드의 `boot.scr` 이 8 MiB
  판인가, 확신이 없으면 `moru ota apply try` 로 올려라"(멈춰도 전원
  재투입으로 복구된다).

#### 카드에 한 번 써야 한다 (닭과 알)

`boot.scr` 은 OTA 로 바꿀 수 없다. **한 번만** 카드 리더로 넣으면 그 뒤로는
L4~L5b 를 포함해 전부 OTA 로 간다. `zephyr.bin`(새 moru)도 같이 넣으면
창이 어긋나는 구간이 없다.

```bash
diskutil list | grep -i "FAT\|boot"       # 카드 번호 찾기
cd ~/Projects/PiZZa/rk3566/sdcard && \
  sudo mcopy -i /dev/rdiskNs2 -o boot.scr zephyr.bin ::/ && \
  sudo mdir -i /dev/rdiskNs2 ::/ && diskutil eject /dev/diskN
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

---

## ⚠ 실기에서 L2·L3 를 다시 돌릴 때 (2026-11-07)

카드 파티션 3(FAT32 "BLOB", 15.6 GB)에 **실제 모델 507.85 MiB** 가
`blob.bin` 으로 들어갔다. `sdcard/boot.cmd` 는 **파티션 3 을 먼저** 보므로
그것이 0x58000000 에 올라간다.

그래서 L2·L3 를 다시 돌리면 **골든값이 안 맞아 실패한다** — 두 칸의 기대값은
boot 파티션의 시험 GGUF(3,872 B)에서 나왔다. L3 의 `[0a] 매직` 은 통과하므로
(실제 모델도 'GGUF' 다) 더 헷갈린다.

다시 돌릴 때는 파티션 3 의 블롭을 치운다:

```
sudo mdel -i /dev/rdisk4s3 ::blob.bin
```

두 칸은 이미 실기 통과를 기록했으니 평소에는 건드릴 일이 없다.
