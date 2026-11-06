# Zephyr 함대의 AI — Edge · Hub 2계층 전략

**2026-11-07.** "tinyML 이 차별화이고 RTOS 위에서 AI 를 돌리는 것이 목표"라는
틀에, 2계층 구조(Edge = RP2350·Teensy / Hub = Cortex-A)를 **둘 다 Zephyr** 로
가져간다는 결정을 반영해 정리한다. LFM2.5-350M 작업이 이 구조의 어디에
놓이는지가 이 문서의 목적이다.

## 먼저, 중요한 세 줄

1. **"RTOS 위의 AI" 는 목표가 아니라 이미 출하된 능력이다** — Edge 에서
   XGBoost 가 Zephyr 위에서 돌고, 골든 파리티로 호스트·펌웨어 비트 일치를
   배포 관문으로 삼고 있다.
2. **LFM2.5-350M 은 tinyML 이 아니다.** 지금 Edge 모델은 15~84 KB 다 —
   **약 3,000배** 차이이고 연산 모양도 다르다. 그 단어는 Edge 에만 쓴다.
3. **그러나 350M 은 곁가지가 아니다.** 2계층 구조에서 **Hub 계층의 AI** 라는
   제자리가 있다. 처음 세운 계획은 이것을 "별개 베팅" 으로 적었는데,
   구조를 알고 보면 **틀린 틀**이었다.

## 차별화를 한 문장으로

> **Linux 없이, Edge 부터 Hub 까지 전부 Zephyr 로 돌아가는 AI 함대.**
> Edge 는 MMU 없는 MCU 에서 100 ms 실시간 인터록을, Hub 는 Cortex-A 에서
> 집계와 설명을 한다. 그 사이는 Wi-Fi HaLow 다.

이게 흔하지 않은 이유: Hub 급에서 Zephyr 를 쓰는 곳이 드물다(보통 Linux 로
간다). 그래서 **Hub 를 Zephyr 로 유지하는 것 자체가 차별화의 절반**이다.

---

## 1. 함대 재고 — 가진 것 (추측 아님)

### Edge 계층 — RP2350 · Teensy 4.1

```
3 kHz ADC → 100 ms 블록 = 300 샘플
  → 특징 24개 (펌웨어 내부, 물리 기반)
  → XGBoost (.xmb, 트리 ≤300 · 노드 ≤5120, 실제 15~84 KB)
  → V11 veto 게이트 (veto_sigma_max · veto_a60_min)
  → 판정 (interlock / multiclass / trend)
```

**특징 계약** (`MoruLab_MLOps/packages/LWCT/fw_contract.json`,
`count=24 · min=22 · featsem=3`, 펌웨어가 22·24 둘 다 수용):

```
w2s_a60  w2s_a180  w2s_a300  w2s_thd  w2s_zcr  w2s_snr_db
w2s_e60_ratio  w2s_h3_ratio  w2s_a60_cv  w2s_rms_cv
w2s_phase_coherence  w2s_harmonic_phase_consistency  w2s_a60_autocorr
w2s_spectral_flatness  w2s_even_odd_harmonic_ratio  w2s_ms_zcr_std
w2s_snr_proxy  g_a60_adc2  g_rms_adc2
ef_grad_a60  ef_decay_n  ef_grad_rms  w2s_phase_jitter  …
```

**이미 성숙하고 물리를 담은 집합이다** — 고조파(60/180/300 Hz), THD, 위상
일관성·고조파 위상 정합, 60 Hz 자기상관, 스펙트럼 평탄도, 짝·홀 고조파비,
전계 감쇠 기울기. 도메인 전문가의 산물이다.

**배포 규율 — 모델보다 값나가는 부분**

| 규율 | 무엇 |
|---|---|
| **골든 파리티** | `.xmb` 에 입력 4벡터 + 기대 출력이 들어 있고, 콘솔이 **자기 평가기로 계산해** 대조 → 호스트·펌웨어 **비트 일치**를 배포 전에 증명 |
| **같은 이유로 거부** | 콘솔 파서가 펌웨어 `model_store.cpp` 의 `parse_header`/`load_file_locked` 와 **1:1 같은 순서** |
| **상한 두 종류 분리** | 형식 상한(노드 int16 → 32767) ≠ 장비 상한(응용 팩 `target.limits`) |
| **물리 veto** | 모델 위의 관문. 모델이 틀려도 물리가 막는다 |
| **응용은 선언으로** | 활선·모터 예지·유해가스가 `pack.json` 으로. 콘솔은 응용을 모른다 |

### 연결 — Wi-Fi HaLow (802.11ah)

`NGT_AP_ZephyrRTOS` = Teensy 4.1 Zephyr 펌웨어, **HaLow AP 의 정책 엔진 + UI**.
NRC7394(`nrc7394_sdk/sdk/apps/NGT_bypass`)와 **BridgeRPC(UART)** 로 짝.
현재 **P4(scaffold)** — hello-world 스레드 + USB-CDC 콘솔 검증 단계.

→ **함대의 뼈대가 아직 이르다.** Hub 의 AI 가 Edge 의 판정을 받아야 의미가
있으므로, 이 계층의 성숙도가 Hub AI 의 **선행 조건**이다.

### Hub 계층 — Cortex-A, 전부 Zephyr 보드가 있다

| 보드 | SoC / 코어 | ISA · dotprod | RAM | Zephyr 보드 | Zephyr 가 잡는 창 |
|---|---|---|---|---|---|
| **Radxa Zero 3E** | RK3566 / A55 ×4 | ARMv8.2 · **있음** (i8mm 없음) | **2 GB**(실측) | **우리 아웃오브트리** | `DT_SIZE_M(1)` — 우리가 정한 값 |
| Pi Zero 2 W | BCM2710 / A53 ×4 | ARMv8.0 · **없음** | **512 MB** | 인트리 `rpi_zero_2w` | — |
| Pi 4B | BCM2711 / A72 ×4 | ARMv8.0 · **없음** | 1~8 GB | 인트리 `rpi_4b` | **`DT_SIZE_M(8)`** |
| Pi 5 | BCM2712 / A76 ×4 | ARMv8.2 · **있음** | 4~16 GB | 인트리 `rpi_5` | — |

**두 가지가 여기서 결정된다.**

① **dotprod 가 Hub 를 둘로 가른다.** A55·A76 은 있고, **A53·A72 는 없다**.
   양자화 행렬곱의 ARM 가속 경로가 A53/A72 에서는 안 걸린다. 즉
   **Pi Zero 2 W 와 Pi 4B 는 LLM 급 Hub 로는 불리하다** — 성능뿐 아니라
   Pi Zero 2 W 는 512 MB 라 0.25 GB 모델이 절반을 먹는다.
   (전달받은 메모의 "A55/A53 계열은 dotprod" 는 **A53 에 대해 틀렸다.**)

② **창 크기는 보드 DTS 의 숫자일 뿐이다.** 인트리 `rpi_4b` 가 이미 **8 MB** 를
   잡는다. 우리가 1 MB 인 것은 우리가 그렇게 정한 것이고, 수백 MB 로 키우는
   것은 이례적인 일이 아니다. **처음 계획에서 1단계 위험을 과대평가했다.**
   (단, 이 보드는 EL2 데이터 캐시 전력이 있어 배치 변경은 `apply try`
   슬롯으로 돌린다.)

**결론: 350M 급 Hub 는 Radxa Zero 3E(A55·2 GB) 와 Pi 5(A76·4 GB+) 다.**
우리가 포팅을 소유한 것은 Zero 3E 이므로 **거기서 먼저 한다.**

> 오늘까지의 rk3566 Zephyr 작업(셸·OTA·uart2 로그·fault 덤프·실험 슬롯)은
> 곁가지가 아니라 **Hub 플랫폼의 OS 브링업**이었다.

---

## 2. Edge 계층의 프런티어 — 제가 정하지 않는다

후보를 증거와 함께 늘어놓는다. **고르기 전에
`MoruLab_MLOps/DATA_TRAIN_PLAN.md` 와 `V10_APPLY_PLAN.md`(드리프트 D1~D10 ·
버그 B1~B10)를 읽어야 한다** — 지금 아픈 곳이 거기 적혀 있다. 그 저장소의
로드맵은 그 저장소가 소유한다.

| 후보 | 맞는 경우 | 반대 증거 |
|---|---|---|
| **학습된 특징** (원시 300샘플 1-D CNN/GRU) | 특징이 놓친 물리가 있을 때 | **약하다.** 특징 24개가 이미 물리를 담았고, 300샘플@3 kHz 에서 60 Hz 구조를 다시 배워야 한다. 무엇보다 **veto 게이트가 기대는 해석 가능성을 잃는다** |
| **모델 교체** (트리 → 소형 MLP) | 24특징 위에서 트리가 한계일 때 | 상한 트리 300·노드 5120 인데 실제가 15~84 KB — **상한에 안 닿았다** |
| **데이터·평가** | 라벨·분포·현장 재현이 병목일 때 | **유력하다** — 특징·모델 모두 여유인데 제품이 실증 단계라면 남는 것은 데이터와 평가다 |
| **응용 확장** (`pack.json` 으로 모터·가스) | 활선이 안정됐을 때 | 구조가 이미 응용 중립 — **가장 싼 확장** |
| **특징 계약 확장** (24 → 그 이상) | 특정 오판에 쓸 물리가 더 있을 때 | 계약 버전 체계가 하위호환을 이미 다룬다 — 저렴 |

**제 읽기(결정 아님):** 특징도 모델도 상한에 안 닿았으므로 병목은
**데이터·평가** 또는 **응용 확장** 쪽일 가능성이 높다.

---

## 3. Hub 계층의 AI — 무엇을 위한 것인가

Hub 가 Edge 와 **같은 RTOS** 를 쓰는 것이 구조의 핵심이므로, Hub 의 AI 도
그 안에서 의미를 가져야 한다. 정직한 가치는 셋이다.

| 역할 | 내용 | 350M LLM 이 필요한가 |
|---|---|---|
| **집계·추세** | 여러 Edge 의 판정·특징을 모아 추세·이상을 본다 | **아니다.** 통계·소형 모델로 된다 |
| **설명** | "왜 인터록이 걸렸나" 를 현장 기사에게 자연어로 | **그렇다.** 이게 LLM 의 제자리다 |
| **지시 해석** | 음성·자연어 명령을 장치 동작으로 | 그렇다(툴 호출). LFM2.5 가 툴 호출을 지원한다 |

### 설명 역할을 구체적으로

```
현장 기사: "왜 인터록이 걸렸나?"
Hub      : "60 Hz 성분 a60 이 … 이고 위상 일관성이 … 이라 활선으로
            판정했다. veto_a60_min 관문도 넘었다. 기여가 큰 특징은
            phase_coherence · h3_ratio 다."        ← 클라우드 없이
```

**성공 기준은 속도가 아니라 이것이다 — Edge 의 판정 흔적(특징 24개 · 트리
기여 · veto 상태)을 입력으로 먹는가.** 먹지 않으면 2~6주짜리 포팅에 제품이
없는 데모다. 그래서 Hub AI 의 1차 관문에 "판정 흔적을 받아 설명한다" 를
넣는다.

### 왜 Hub 도 Zephyr 인가 (Linux 가 아니라)

| 이유 | 측정 가능한가 |
|---|---|
| 지터 — 집계·설명이 실시간 루프와 같은 장치에서 돈다 | **그렇다.** 토큰 간 지연 p50/p99/최대 |
| 부팅 → 첫 응답 | **그렇다.** 지금 U-Boot 6.8 s + Zephyr 54 ms(실측) |
| Linux 루트파일시스템·공격면이 없다 | 정성적 — 그러나 안전 제품에서는 실질적이다 |
| Edge 와 **같은 도구·같은 규율**(골든 파리티·OTA·셸) | 조직 비용이 준다 |

⚠ **속도가 Zephyr 를 고르는 이유가 아니다.** 같은 실리콘·같은 대역폭이므로
Linux 보다 빠를 이유가 없다. 고르는 이유는 **지터와 통합**이다.

---

## 4. 두 계층을 잇는 것 = 설명 가능성

Edge 의 `.xmb` 는 **결정 트리**다. 이것이 Hub LLM 과 잘 맞는 드문 성질을
가진다 — **트리는 왜 그렇게 판정했는지 경로로 말할 수 있다.**

```
Edge: 특징 24개 + 트리 경로 + veto 상태  →  HaLow  →  Hub: 자연어로 풀어 준다
```

신경망으로 바꾸면 이 설명 경로가 약해진다. **즉 §2 에서 "학습된 특징" 을
고르지 않는 이유가 하나 더 생긴다** — 그것은 Hub 의 설명 역할과 상충한다.
구조 전체로 보면 **트리를 유지하는 것이 전략적으로 일관된다.**

---

## 5. 하지 말 것

1. **Edge 의 실시간 판정을 Hub 로 올리지 않는다.** 100 ms 인터록은 MMU 없는
   MCU 에서 도는 것이 그 제품의 이야기다. Hub 로 옮기면 지연·전력·인증이
   전부 달라지고 차별화가 사라진다.
   (처음 쓴 판에서는 "Hub 로 옮기지 말 것" 을 더 넓게 적었는데, 2계층
   구조에서는 **Hub 계층 자체가 정당하다** — 옮기면 안 되는 것은 **실시간
   판정**이다.)
2. **Hub 의 LLM 을 tinyML 이라 부르지 않는다.** 대외 문안에서 특히.
3. **골든 파리티를 계층 구분 없이 적용하거나 버리지 않는다.**
   자유 생성 **문장**에는 성립하지 않지만, 그 아래는 전부 결정적이라
   **성립한다** — 역양자화(비트 일치) · 커널 · 층 · logits · 시드 고정
   토큰 열. 계층별 적용은
   [llm-hub-test-plan.md](llm-hub-test-plan.md) 에 있다.
   (이 항목의 처음 판은 "생성 모델에 억지로 적용하지 않는다" 라고만 적어
   **계층을 구분하지 않아 과했다.**)
4. **Edge 와 Hub 를 한 일정에 묶지 않는다.** Hub 포팅(2~6주, 전례 없음)이
   Edge 를 멈추게 해서는 안 된다.
5. **dotprod 없는 Hub(A53·A72)를 LLM 대상으로 잡지 않는다.** Pi Zero 2 W 는
   512 MB 라 더욱 아니다. 집계·추세 역할까지만 맡긴다.

---

## 6. 일의 순서와 관문

| # | 할 일 | 기간 | 왜 이 순서 |
|---|---|---|---|
| 1 | `DATA_TRAIN_PLAN.md`·`V10_APPLY_PLAN.md` 를 읽어 **Edge 의 현재 병목** 확정 | 수 시간 | 가장 싸다. §2 의 다섯 후보 중 무엇이 맞는지 가린다 |
| 2 | **Hub 관문 0** — Linux 기준선 (tg ≥ 5 t/s?) | 1~2일 | Hub LLM 의 성립 여부. 1과 병행 |
| 3 | **Hub 관문 A** — C++17 스파이크 (아래) | 1~2일 | 포팅 비용을 가른다. 2와 병행 |
| 4 | **Hub 관문 B** — 판정 흔적 계약 설계 | 1일 | LLM 보다 먼저. 흔적이 없으면 설명할 것이 없다 |
| 5 | 1~4 로 **Edge 와 Hub 중 어디에 사람을 넣을지** 결정 | — | 나흘이면 추측 없이 결정할 수 있다 |

세부 단계(메모리 바닥 · 모델 적재 · 추론 엔진 · 측정 · 셸 통합)는
[ondevice-llm-plan.md](ondevice-llm-plan.md) 에, **계층별 CLI 단위테스트**는
[llm-hub-test-plan.md](llm-hub-test-plan.md) 에 있다.

### 관문 A 를 세밀하게 — 한 파일에 세 기능

이 관문은 처음 쓴 것보다 크다. picolibc→newlib 만이 아니라 **예외**와
**`std::thread`** 가 각각 미지수다. 셋을 한 파일에 넣어 한 번에 가린다.

```cpp
// 이 파일이 뜨면 "ggml + llama.cpp 그래프" 안이 성립한다.
// 하나라도 막히면 비용이 두 배가 된다.
#include <vector>
#include <stdexcept>
#include <thread>

static void worker(std::vector<float>& v) { for (auto& x : v) x += 1.0f; }

int main(void) {
    std::vector<float> v(1024, 0.0f);       // ① STL 컨테이너 + 힙
    try { throw std::runtime_error("x"); }   // ② 예외 (arm64 + newlib 에서 미지수)
    catch (const std::exception&) { }
    std::thread t(worker, std::ref(v));      // ③ std::thread → Zephyr 스레드
    t.join();
    return (v[0] == 1.0f) ? 0 : 1;
}
```

필요 설정: `CONFIG_CPP=y` `CONFIG_STD_CPP17=y` `CONFIG_CPP_EXCEPTIONS=y`
`CONFIG_LIB_CPLUSPLUS=y` `CONFIG_POSIX_API=y` + **libc 를 newlib 로**.
⚠ 이 보드에서 libc 를 바꾸는 것은 메모리 배치를 바꾸는 일이다 —
`moru ota apply try` 로 돌린다(복구는 전원 재투입, 2026-11-06 실측).

### 관문 0 의 가정을 명시한다

tg 10~15 t/s 라는 추정은 **RK3566 LPDDR4 유효 대역폭 약 3~4 GB/s** 와 모델
0.23 GB 에서 나온 것이다. **`llama-bench` 가 3 t/s 라고 하면 그것은 실리콘의
값이고 Zephyr 탓이 아니다.** 먼저 박아 두어야 나중에 포팅을 엉뚱하게
의심하지 않는다.

### 관문 B — 판정 흔적 계약

Hub 가 설명할 수 있으려면 Edge 가 **무엇을 보냈는지**가 정의돼야 한다.
최소 묶음:

| 필드 | 왜 |
|---|---|
| 특징 24개 값 + `featsem`·`contract` 버전 | 설명의 재료. 버전이 없으면 뜻이 흔들린다 |
| 판정 결과 + 임계 (`threshold`·`base_score`) | 무엇을 넘었는지 |
| veto 상태 (`veto_sigma_max`·`veto_a60_min` 통과 여부) | 물리 관문이 개입했는지 |
| 상위 기여 특징 몇 개 | 트리에서 뽑을 수 있다 — LLM 이 추측하지 않게 한다 |
| 모델 식별 (`train_ts` · body CRC32) | 어느 모델의 판정인지. 현장 재현의 전제 |

⚠ **LLM 이 숫자를 만들어내지 않게 하는 것이 이 계약의 목적이다.** 설명은
받은 값만 쓰고, 없는 값은 "모른다" 고 해야 한다. 안전 제품에서 이것은
선택이 아니다.

---

## 관련 문서

- [ondevice-llm-plan.md](ondevice-llm-plan.md) — Hub LLM 의 단계별 계획·관문
- [hardware-verified.md](hardware-verified.md) — 이 보드 실측값·Linux 부팅 기록
- [bringup-rules.md](bringup-rules.md) — 기준선·한 변수·관측 채널 규칙
- `~/Projects/NGT_Project/MoruLab_MLOps/` — Edge 계층의 본체.
  `README.md` · `DATA_TRAIN_PLAN.md` · `V10_APPLY_PLAN.md` ·
  `MODULE_ARCHITECTURE.md` · `packages/LWCT/fw_contract.json`
- `~/Projects/NGT_Project/NGT_RTOS_V3/` — Edge·AP 펌웨어.
  `NGT_AP_ZephyrRTOS`(HaLow AP 정책 엔진, P4) · `NGT_STA_ZephyrRTOS` ·
  `docs/halow-ap-system-plan.md` · `docs/rpc-spec.md`
