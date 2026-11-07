# L6a — 모델 적재 관문 (메타데이터 + 어휘)

L6 을 셋으로 나눈 첫 칸 (`docs/l6-decisions.md` 의 "하위 사다리").
**추론은 하지 않는다** — 그래프는 L6b 다.

가리는 것:

| 점검 | 무엇을 | 왜 |
|---|---|---|
| `[0]` | `zephyr_future.h` 의 **의미** | 지금까지 컴파일만 확인했다. 조용히 틀리면 적재 실패가 묻힌다 |
| `[1]` | 블롭 매직 + **텐서 수** | boot 파티션의 시험 GGUF(3,872 B, 텐서 2개)도 매직이 'GGUF' 다 |
| `[2]` | `gguf_init_from_file_ptr` + `fmemopen` | 제자리 블롭에서 메타데이터 |
| `[3]` | `llama_mmap::SUPPORTED` · `ml->use_mmap` | **결정 3 의 핵심** — false 면 508 MiB 를 또 복사한다 |
| `[4]` | 320개 텐서가 블롭 안을 가리키나 | 적재가 잘렸는지 |
| `[5]` | `llama_vocab::load` | 248,320 토큰이 힙에 올라가나 |
| — | **이미지 크기 대 OTA 상한** | `build.sh` 가 찍는다. L6c 에서 놀라지 않기 위해 여기서 잰다 |

## ✅ QEMU 통과 — 2026-11-07, 실제 모델

```
=== L6a: 모델 적재 관문 (메타데이터 + 어휘) ===
  창: 0x58000000 + 512 MiB
  [0] std::future 대체판 ... OK — 값 · 예외 재던지기 · deferred · 인자 전달
  [1] 블롭 점검 ... OK — 매직 'GGUF' v3, 텐서 320개, KV 46개
  [2] 메타데이터 ... OK (800 ms)
      arch        qwen35
      텐서        320개 (weights_map 320)
      데이터 시작 0xa74400
  [3] 제자리 적재 ... OK — SUPPORTED=true · use_mmap=true — 복사 없음
  [4] 텐서 위치가 블롭 안인가 ... OK — 320개 전부 안쪽, 끝 0x1fbd9100 (507 MiB)
  [5] 어휘 적재 ... OK (1540 ms)
      토큰 248320개 · BOS 11 · EOS 248046
=== L6a PASS (실패 0) ===
```

모델: `Qwen3.5-0.8B-Q4_K_M.gguf` 532,517,120 bytes. 양자화 혼합 —
q4_K 다수 · q5_K 36 · q6_K 17.

### 측정

| 항목 | 값 |
|---|---|
| 이미지 | **1,146,776 bytes = OTA 상한 8 MiB 의 13%** (여유 7,241,832) |
| 정적 RAM | 104,172 KB / 128 MB = **79.48%** |
| 메타데이터 적재 | 800 ms |
| 어휘 적재 | 1,540 ms |
| token-to-piece 캐시 | 1.7581 MB |
| 텐서 데이터 끝 | 0x1fbd9100 = 507 MiB (창 512 MiB 안) |

**이미지 크기 걱정은 해소됐다.** `std::regex` 한 함수가 260 KB,
`unicode-data.cpp` 가 11,019줄이라 상한에 닿을 것을 염려했지만 13% 다.

⚠ **정적 RAM 79.48% 는 아레나(96 MiB)를 전부 세어서다.** 어휘의 실제 사용량은
아직 안 재 봤다. L6b 는 ggml 문맥과 그래프가 더 필요하므로 **실제 힙 사용량을
재야 한다** — 남은 26 MB 가 충분한지 모른다.

## 빌드·실행

```bash
ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1 \
  ./build.sh "$PWD/apps/llm_l6a" /tmp/claude-501/l6a
```

```bash
L_BLOB=$HOME/Projects/PiZZa/models/Qwen3.5-0.8B-Q4_K_M.gguf ./tools/qemu_run.sh apps/llm_l6a
```

## 알아낸 것 — 상류를 고치지 않는 수단이 넷이 됐다

1. `ggml_zephyr_compat.h` — 없는 선언 (강제 포함)
2. `zephyr_future.h` — libstdc++ 가 비워 둔 `<future>` (강제 포함)
3. **대체 파일** — `llama_mmap_zephyr.cpp` · `ggml_backend_reg_zephyr.cpp`
4. **빠진 심볼 채우기** — `llama_split_zephyr.cpp`. 상류는 이 둘을
   `src/llama.cpp` 에 두었는데, 그 파일을 넣으면 `src/models/*.cpp`
   **175개 46,741줄**이 따라온다. 두 함수는 순수 문자열 함수라 그대로 옮겼다.

⚠ `-include` 를 두 번 주려면 cmake 에서 **`SHELL:` 가 필요하다.**
`target_compile_options` 가 같은 플래그를 중복 제거해
`-include a.h b.h` 가 되고 gcc 가 `b.h` 를 **입력 파일**로 읽는다
(`cannot specify '-o' with '-c' … with multiple files`). 실제로 겪었다.

## ✅ 실기 통과 — 2026-11-07, Radxa ZERO 3E (RK3566)

```
=== L6a: 모델 적재 관문 (메타데이터 + 어휘) ===
  창: 0x58000000 + 512 MiB
  [0] std::future 대체판 ... OK — 값 · 예외 재던지기 · deferred · 인자 전달
  [1] 블롭 점검 ... OK — 매직 'GGUF' v3, 텐서 320개, KV 46개
  [2] 메타데이터 ... OK (21250 ms)
      arch qwen35 · weights_map 320 · 데이터 시작 0xa74400
  [3] 제자리 적재 ... OK — SUPPORTED=true · use_mmap=true — 복사 없음
  [4] 텐서 위치 ... OK — 320개 전부 안쪽, 끝 0x1fbd9100 (507 MiB)
  [5] 어휘 적재 ... OK (28918 ms)
      토큰 248320개 · BOS 11 · EOS 248046
=== L6a PASS (실패 0) ===
```

**QEMU 와 모든 값이 같다** — 텐서 320 · KV 46 · 데이터 시작 `0xa74400` ·
텐서 끝 `0x1fbd9100` · 토큰 248,320 · BOS 11 · EOS 248046. 다른 것은
시간뿐이다.

### 성능 — 실기 대 QEMU

| 항목 | QEMU | 실기 (A55 r2p0) | 배수 |
|---|---|---|---|
| 메타데이터 적재 | 800 ms | **21,250 ms** | 26.6× |
| 어휘 적재 | 1,540 ms | **28,918 ms** | 18.8× |
| 부트로더 (SD 에서 508 MiB) | — | **50,880 ms** | — |

QEMU 는 M 계열 호스트에서 돌아 빠르다. 비교의 뜻은 "실기가 이만큼 느리다" 가
아니라 **"QEMU 시간으로 실기를 예측할 수 없다"** 는 것이다.

⚠ **부트로더 50.88 s 가 실제 모델 적재의 증거다.** moru 가 `moru info` 에
찍는다. 모델 없이 부팅할 때보다 훨씬 길다 — U-Boot 가 파티션 3 에서
508 MiB 를 읽은 것이다(U-Boot 는 1.5 Mbps 로 말해 그 echo 는 읽을 수 없다,
`docs/bringup-rules.md` 규칙 7).

### 아직 안 잰 것

**실제 힙 사용량.** 정적 RAM 79.48% 는 아레나(96 MiB)를 전부 세어서다.
어휘가 그 중 얼마를 쓰는지 모른다. L6b 는 ggml 문맥과 그래프가 더 필요하므로
**L6b 의 첫 일은 이것을 재는 것**이다 — `kernel heap` 또는 앱 안에서
`sys_heap` 통계를 찍는다.
