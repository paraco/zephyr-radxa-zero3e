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

⚠ **처음에 적었던 근거 ①(「호스트는 i8mm, 실기는 dotprod 로 갈린다」)은 틀렸다.**
측정으로 확인했다:

```
$ echo | clang -x c - -E -dM -Xclang -target-feature -Xclang -i8mm | grep __ARM_FEATURE_
#define __ARM_FEATURE_DOTPROD 1          ← MATMUL_INT8 없음

$ echo | aarch64-zephyr-elf-gcc -x c - -E -dM -mcpu=cortex-a55 | grep __ARM_FEATURE_
#define __ARM_FEATURE_DOTPROD 1          ← 같다
```

`tools/host_golden.sh:87` 이 이미 `-Xclang -target-feature -Xclang -i8mm` 로
호스트의 i8mm 을 끄고 있다. 그래서 **양쪽이 똑같이 dotprod 경로**다. repack 을
켜도 `arch/arm/repack.cpp` 의 `#if` 는 양쪽에서 같은 가지를 고른다.

게다가 그 갈림은 repack 전용이 아니다 — `arch/arm/quants.c:305,598,1158,2339,…`
의 평범한 `ggml_vec_dot_q4_0_q8_0` 에도 같은 `#if __ARM_FEATURE_MATMUL_INT8` 가
있다. repack 을 끄는 것으로 없앨 수 있는 위험이 아니었다. 없애는 것은
**`-mcpu` 짝맞춤**이고, 그것은 이미 돼 있다.

그리고 **그 경로는 이미 실기에서 가려졌다** — L4 가 `GGML_TYPE_Q4_0` 으로
`ggml_mul_mat` 을 돌려(`apps/llm_l4/src/l4_graph.c:78-83`) `ggml_vec_dot_q4_0_q8_0`
의 dotprod 커널을 타고 **비트 일치**했다. 즉 양자화 내적은 이미 확인된 지점이다.

**남는 진짜 근거는 둘이다:**

1. **메모리 — 이것이 결정적이다.** repack 은 가중치를 **다시 배치한 사본**을
   별도 버퍼 타입에 만든다(`ggml-cpu.cpp:64` 가 등록하는 그 버퍼). 모델 창
   512 MiB 에 모델이 507.85 MiB 라 여유가 **4.15 MiB** 다. 사본이 들어갈 자리가
   없다. 결정 3 의 제자리 적재와 정면으로 충돌한다 — 제자리 적재는 가중치를
   블롭에서 **옮기지 않는** 것이 전제다.
2. **한 왕복에 변수 하나** (`hardware-roundtrip-discipline`). L6·L7 은 모델
   그래프를 가린다. repack 은 가중치 배치와 matmul 커널을 **동시에** 바꾸므로,
   같은 왕복에 넣으면 불일치의 원인을 가릴 수 없다. L8 의 소재다.

(순수 성능 기능이라는 점은 2번에 포함된다.)

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

## 결정 3 — 가중치는 **제자리에서** 읽는다. 상류 수정 0건으로 된다.

L6 코드를 한 줄도 쓰기 전에 정해야 하는 것. 이것이 L6 의 구조다.

### 문제

`llama_model_loader` 는 `llama_file`(fopen/fread/fseek)로 파일을 열고, `use_mmap`
이면 `llama_mmap` 으로 매핑한다. **둘 다 Zephyr 에 없다.** mmap 경로가 없으면
`load_all_data` 가 텐서마다 새 버퍼를 할당해 `fread` 한다 — **또 508 MiB** 다.
여유 4.15 MiB 에 불가능하다.

### 해답 — 상류가 이미 이음매를 준다

| 필요 | 상류의 이음매 | 우리가 할 일 |
|---|---|---|
| GGUF 를 메모리에서 파싱 | `llama_model_loader` 에 **`FILE*` 입력 경로가 이미 있다** (`llama-model-loader.cpp:687` `gguf_init_from_file_ptr`, `:703` `llama_file(file)`) | `fmemopen(0x58000000, size, "rb")` — **상류 수정 0** |
| `fmemopen` 이 이 툴체인에 있나 | — | **있다.** `stdio.h:405` 선언, picolibc `libc_tinystdio_fmemopen.c.o` 에 `T fmemopen`. 컴파일·링크·동작 모두 확인했다 |
| 매핑 | `llama-mmap.h` 는 **완전한 pimpl** — `llama_file`·`llama_mmap`·`llama_mlock` 셋 다 `struct impl` 을 숨기고, `SUPPORTED` 는 `static const bool` 이다 | `llama-mmap.cpp` **대체판**을 쓴다. 헤더도 호출부도 고치지 않는다 |
| 제로 복사 | `use_mmap` 이면 `mapping->addr() + w.offs` 를 **그대로 쓴다** (`:1549`, `:1717`) | `addr()` 가 블롭 바닥을 돌려주면 끝이다 |

대체판 `apps/llm_common/llama_mmap_zephyr.cpp` 의 내용:

- `llama_mmap::SUPPORTED = true` · `addr()` = DT `model_blob` 의 `reg` 주소 ·
  `size()` = 블롭 크기 · `unmap_fragment()` · `llama_prefetch()` 는 **무동작**.
- `llama_mlock::SUPPORTED = false` (매핑이 애초에 RAM 이라 고정할 것이 없다).
- `llama_file` 은 `fmemopen` 핸들 위의 `fread`/`fseek` 래퍼로 **정직하게** 둔다 —
  커서를 따로 만들면 오프셋 정본이 둘이 된다. `use_direct_io` 는 `false` 고정,
  `has_direct_io()` 는 `false`, `read_alignment()` 는 1.
- 쓰기(`write_raw`/`write_u32`)와 `file_id()` 는 **무동작이 아니라 `k_panic()`**.
  조용히 실패하면 어디서 틀렸는지 알 수 없다 — 검토에서 `ggml_zephyr_sysconf` 의
  무한 재귀를 잡아낸 그 규율이다.
- 로더 쪽은 `LLAMA_LOAD_MODE_MMAP` 로 고정한다 (`:564`). `AUTO` 에 맡기지 않는다.

**주소의 정본은 DT 다.** `build.sh` 가 이미 `model_blob.overlay` 의 `reg` 와
`sdcard/boot.cmd` 의 `fatload` 주소를 대조해 어긋나면 종료한다. 이 파일도 DT 에서
읽어 세 번째 사본을 만들지 않는다.

### 덤 — 적재 검증 문제가 사라진다

"소프트웨어 CRC32 11.8 MiB/s → 512 MiB 에 43.3 s" 는 **적재 경로의 비용이 아니게
된다.** 제자리 적재는 복사가 없으므로 검증할 전송이 없다. CRC 는 원하면 부팅 때
한 번 고르는 선택지일 뿐이고, 평소에는 L2 가 하는 **매직 + 앞뒤 표본**으로 족하다.

---

## L6 는 한 번에 가지 않는다 — 하위 사다리

`llama.cpp/src` 가 105,526 줄이다. 셋으로 나눈다.

| 칸 | 가리는 것 | 비고 |
|---|---|---|
| **L6a** | 제자리 블롭에서 **메타데이터 + 어휘** 적재 | `fmemopen`·대체 mmap 이음매를 가린다. 어휘 248,320 개가 힙에 올라간다. **그리고 이미지 크기 대 OTA 상한 8 MiB** — `unicode-data.cpp` 표까지 들어가면 L5 보다 훨씬 크다. `build.sh` 가 이미 상한을 보고하므로 **여기서 재고 L6c 에서 놀라지 않는다** |
| **L6b** | **한 토큰** 순전파, 층별 텐서 대조 | 호스트 골든은 `llama-eval-callback -ngl 0 --device none` |
| **L6c** | 여러 토큰 + KV 캐시 + DeltaNet 상태 | |

토크나이저·샘플러는 L6 에서 **뺀다** — 토큰 id 를 호스트에서 먹인다.
모델 파일은 Qwen3.5 하나만 필요하다. 나머지 `src/models/*.cpp` 약 100개는
`llama-model.cpp` 의 switch 에서 참조되므로 **전부 컴파일해 크기를 재거나
switch 를 잘라낸다** — 어느 쪽이든 L6a 의 크기 측정에 달렸다.

L6 전체는 **QEMU 에서 돈다** (`tools/qemu_run.sh` 의 `L_BLOB=`). 실기 모델 적재를
기다릴 필요가 없다.

## 다음 관문

| 항목 | 상태 |
|---|---|
| L6a·L6b·L6c | **QEMU 에서 전부 된다.** 실기를 기다리지 않는다 |
| 실제 모델(507.85 MiB) 실기 적재 | **사람 왕복 1회 필요.** 부트 파티션 314.6 MB 에 안 들어간다. 카드 `disk4s3`(15.6 GB)는 Radxa 루트라 **거의 확실히 ext4 지 FAT 가 아니다**. 그래도 Rockchip U-Boot 대부분은 `ext4load` 또는 포괄 `load`(`CONFIG_CMD_FS_GENERIC`)를 갖는다 — 있으면 **다시 포맷할 필요가 없다**:<br>`=> help` 로 `ext4load`/`load` 유무 확인 → `ext4load mmc 1:3 0x58000000 /blob.bin`<br>⚠ 508 MiB 를 SD 에서 10~20 MB/s 로 읽으면 **부팅이 30~50 s 늦어진다**. 감당할 만하지만 예상해 둔다 |
| 모델 적재 검증 | **해소됐다** — 결정 3 의 제자리 적재는 복사가 없다 (위 참고) |
| L2 캐시 결과 | 3,872 B 블롭으로만 확인했다. 실제 모델로 다시 가려야 한다 |
