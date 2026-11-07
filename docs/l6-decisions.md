# L6 선결 결정 — repack · backend-reg

사다리 L0~L5b 가 실기에서 전부 통과했다(docs/llm-ladder-status.md). L6(모델 그래프)
으로 가기 전에 두 가지를 **명시적으로** 정한다. 지금은 둘 다 "우연히 그렇게 돼
있는" 상태라 기록이 필요하다.

---

## 결정 1 — `GGML_USE_CPU_REPACK` 는 L6·L7 에서 **끈다**. L8 에서만 켠다.

### 사실 (상류 파일에서 확인)

| 사실 | 출처 |
|---|---|
| 상류 기본값은 **ON** | `ggml/CMakeLists.txt:152` — `option(GGML_CPU_REPACK … ON)` |
| 매크로를 보는 곳은 **딱 두 군데** | `ggml-cpu/CMakeLists.txt:627` 이 정의, `ggml-cpu.cpp:64`(버퍼 타입 등록)·`:644`(기능 문자열) |
| `repack.cpp` · `arch/arm/repack.cpp` 는 **이미 우리 빌드에 들어 있다** | `cmake/ggml-sources.txt` |
| 그런데 매크로가 없어 **버퍼 타입이 등록되지 않는다** | `ggml-cpu.cpp:64` 의 `#ifdef` |
| arm 커널은 **`__ARM_FEATURE_DOTPROD` / `__ARM_FEATURE_MATMUL_INT8` 로 갈린다** | `arch/arm/repack.cpp:27,51,249,662,801,955,1114` 등 |

즉 **지금 repack 은 컴파일되지만 쓰이지 않는다.** 코드는 링크돼 있고 가중치는
재배치되지 않아 평범한 `ggml-cpu` 경로로 간다.

### 왜 끄나

1. **호스트와 실기가 서로 다른 커널로 갈린다.** 호스트는 Apple M 계열이라
   `__ARM_FEATURE_MATMUL_INT8`(i8mm)가 켜지고, 실기 Cortex-A55 는 i8mm 이 없어
   dotprod 커널로 간다. 위 `#if` 들이 **다른 코드를 고른다.** L6·L7 의 목적은
   호스트 골든값과의 수치 일치를 가리는 것인데, 비교 대상이 서로 다른 커널이면
   불일치가 나와도 그게 포팅 결함인지 커널 차이인지 가릴 수 없다.
   이것은 L3·L4·L5 에서 **세 갈래 분류**로 정리한 그 문제의 가장 나쁜 형태다
   (SIMD 경로가 양쪽에서 다르다).
2. **메모리.** repack 은 가중치를 **다시 배치한 사본**을 버퍼에 만든다. 모델 창
   512 MiB 에 모델이 507.85 MiB 라 여유가 4.15 MiB 다. 재배치 사본이 들어갈
   자리가 없다.
3. **순수 성능 기능이다.** 정확도를 위한 것이 아니다. L8(성능)의 소재다.

### 어떻게 적용하나

- `cmake/llm_ggml.cmake` 에 `LLM_GGML_REPACK` 옵션을 만들고 **기본 OFF**,
  ON 일 때만 `GGML_USE_CPU_REPACK` 를 정의한다.
- `tools/host_golden.sh` 가 **같은 변수를 읽는다.** 호스트와 실기가 갈리는 것이
  바로 위험이므로, 한쪽만 바꿀 수 없게 만든다 — `cmake/ggml-sources.txt` 를
  단일 출처로 만든 것과 같은 이유다.
- L8 에서 켤 때는 **호스트 골든값을 repack 으로 다시 만들어** 비교한다.
  끈 상태의 L6·L7 결과가 그때의 기준선이 된다.

---

## 결정 2 — `ggml-backend-reg.cpp` 는 **대체 파일을 쓴다** (상류를 고치지 않는다).

### 사실

| 사실 | 출처 |
|---|---|
| 파일이 `<filesystem>` 과 `dlopen` 에 의존한다 | `ggml-backend-reg.cpp:7,93,112` |
| 다른 백엔드 헤더 15개를 `#ifdef` 로 끌어온다 | `:30~90` |
| **밖으로 내보내는 함수는 15개뿐**이고 전부 레지스트리 vector 위의 얇은 래퍼다 | `:298,302,316,320,325,336,340,345,355,366,374,382,393,397,567,571` |
| `llama.cpp/src` 가 쓰는 것은 그 중 일부 — `dev_by_type`(11회) · `reg_get_proc_address`(10) · `dev_count`(6) · `reg_count`(5) · `dev_get`(5) 등 | `grep` 집계 |
| `ggml_backend_dev_name` 같은 **장치 래퍼는 이 파일이 아니다** (`ggml-backend.cpp`) | 정의 위치 확인 |

### 왜 대체하나

Zephyr 에는 파일 시스템도 동적 링크도 없다. `<filesystem>` 은 이 툴체인의
libstdc++ 에서 링크되지 않는다(`ggml-threading.cpp` 의 `std::mutex` 와 같은
사정이다). 그런데 **필요한 것은 CPU 백엔드 하나를 등록하는 것**뿐이다.

상류를 고치면 `git pull` 마다 충돌한다. 지금까지 **상류 수정 0건**으로 왔고
(`ggml_port.c` 가 빠진 함수를 제공하고, `ggml_zephyr_compat.h` 가 없는 선언을
채운다), 이것도 같은 방식으로 간다 — **포팅 항목 4번**이 된다.

### 어떻게 적용하나

`apps/llm_common/ggml_backend_reg_zephyr.cpp` 를 만든다. 상류의
`ggml-backend-reg.cpp` 를 `ggml-sources.txt` 에 **넣지 않고**, 이 파일을 대신
링크한다. 내용은 고정 배열 하나와 위 15개 함수다:

- `ggml_backend_load` / `unload` / `load_all` / `load_all_from_path` → 아무것도
  하지 않고 `nullptr`. **조용히 실패하지 않는다** — `printk` + `k_panic()` 로
  막는다. `ggml_zephyr_sysconf` 의 `default:` 와 같은 규율이다(검토에서
  무한 재귀를 잡아낸 그 자리).
- 나머지는 CPU 백엔드 등록 하나를 담은 vector 위의 조회다.

**호스트 쪽은 상류 파일을 그대로 쓴다** — 호스트에는 filesystem 이 있고, 레지스트리
동작은 양쪽이 같아야 하므로 대체판이 상류와 같은 결과를 내는지 L6 에서 가린다.

---

## 다음 관문

| 항목 | 상태 |
|---|---|
| 실제 모델(507.85 MiB) 적재 경로 | **미해결** — 부트 파티션 314.6 MB 에 안 들어간다. 카드의 `disk4s3`(15.6 GB)가 FAT 인지 미확인. 카드는 지금 보드에 있다 |
| 모델 적재 검증 | **재설계 필요** — 소프트웨어 CRC32 11.8 MiB/s → 512 MiB 에 43.3 s |
| L2 캐시 결과 | 3,872 B 블롭으로만 확인했다. 실제 모델로 다시 가려야 한다 |
