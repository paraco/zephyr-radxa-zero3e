# L2 — GGUF 파서 관문

[docs/llm-hub-test-plan.md](../../docs/llm-hub-test-plan.md) 의 사다리 셋째 칸.
**"창 밖에 적재된 GGUF 의 헤더·KV·텐서 정보를 정확히 읽는가"** 를 가린다.

## 결과 (2026-11-07, QEMU `-cpu cortex-a55`)

```
=== L2: GGUF 파서 관문 ===
  블롭: 0x60000000  기대 크기 3872 B
  [1] 매직 'GGUF' 가 보이는가 ... OK
  [2] 파일 CRC32 ... OK
  [3] 버전 3 · 텐서 6개 · KV 8개 ... OK
  [4] KV 8개 훑기 ... OK
  [5] 텐서 정보 6개 ... OK
  [6] 데이터 시작 정렬(32) ... OK
=== L2 PASS (실패 0) ===
```

## ✅ 실기 통과 — 2026-11-07 — **캐시 잔여 위험이 닫혔다**

```
=== L2: GGUF 파서 관문 ===
  블롭: 0x58000000  기대 크기 3872 B
  [1] 매직 'GGUF' 가 보이는가 ... OK
  [2] 파일 CRC32 ... OK
  [3] 버전 3 · 텐서 6개 · KV 8개 ... OK
  [4] KV 8개 훑기 ... OK
  [5] 텐서 정보 6개 ... OK
  [6] 데이터 시작 정렬(32) ... OK
=== L2 PASS (실패 0) ===
```

RAM 472 KB / 128 MB (0.36%) · 이미지 74,344 B.

**이것이 QEMU 가 구조적으로 못 가리던 유일한 항목이다.** 아래 유보가 이것으로
해소됐다 — U-Boot(`fatload blob.bin 0x58000000`)가 Zephyr 의 MMU·캐시보다
**먼저** 써 넣은 블롭을 캐시 가능(`MT_NORMAL`) 매핑으로 읽었고 **낡은 줄을
보지 않았다.** 3,872 바이트 전체 CRC32 도 맞다.

### 왜 안 깨졌나 — 추측하지 않고 아는 것만 적는다

확인된 것은 **이 조건에서 깨지지 않았다**는 사실뿐이다. 블롭이 3,872 B 로
작고, U-Boot 가 `go` 전에 캐시를 정리할 수도 있고, Zephyr 의 MMU 초기화가
해당 영역을 무효화할 수도 있다 — **어느 것이 이유인지는 모른다.**

⚠ 그래서 **507.85 MiB 실제 모델로 다시 확인해야 한다.** 작은 블롭이
우연히 통과했을 가능성을 배제할 수 없다(캐시 용량은 수 MB 수준이고, 작은
영역은 전부 캐시에 들어가 일관성 문제가 드러나지 않을 수 있다).

## ⚠ (해소됨) 통과했지만 실기에서 다시 봐야 했던 것

[1]·[2] 는 L1 이 남긴 잔여 위험 — "블롭은 Zephyr 의 MMU·캐시가 켜지기 전에
적재기가 쓰므로 캐시 가능 매핑으로 읽으면 낡은 줄을 볼 수 있다" — 을 겨냥했다.
**QEMU 에서 통과한 것은 그 위험을 푼 것이 아니다.**

QEMU TCG 는 데이터 캐시를 모형화하지 않는다. 메모리가 항상 일관돼 보이므로
**이 시험은 QEMU 에서 실패할 수 없다.** 즉 양성 결과에 정보가 없다.
실기(U-Boot `fatload` → `go`)에서 같은 두 항목을 다시 확인해야 하고,
거기서 깨지면 블롭 영역을 읽기 전에 캐시를 무효화해야 한다.

**잔여 위험으로 계속 둔다. "L2 에서 확인했다" 고 쓰지 않는다.**

## 골든값이 내 파서와 독립이다

`src/l2_golden.h` 는 생성 파일이다.

```
llama.cpp 참조 GGUFWriter  →  l2_test.gguf  →  llama.cpp 참조 GGUFReader  →  골든값
                                   ↓
                          내가 쓴 Zephyr 파서  →  비교
```

내 파서는 파일을 만드는 쪽에도, 골든값을 뽑는 쪽에도 관여하지 않는다.
손으로 기대값을 적었다면 틀렸을 것이 실제로 하나 있었다 —
`moru.arr_u32` 의 원소 형식이 **INT32(5)** 다. UINT32(4) 가 아니다.
`add_array` 가 파이썬 `int` 를 INT32 로 추론한다.

## 형식 — 참조 구현에서 확인한 것

`gguf-py/gguf/gguf_reader.py` 를 읽어 필드 순서를 확정했다. 기억으로 쓰지 않았다.

```
magic u32 'GGUF'(0x46554747) | version u32 | tensor_count u64 | kv_count u64
KV × kv_count:       key{len u64, 바이트} · type u32 · 값
텐서정보 × count:     name{len u64, 바이트} · n_dims u32 · dims u64×n_dims
                     · type u32 · offset u64
데이터 시작 = 정렬 올림(여기까지의 오프셋)   정렬 = general.alignment 또는 32
```

`GGUFValueType` (constants.py:6905): u8 0 · i8 1 · u16 2 · i16 3 · u32 4 ·
i32 5 · f32 6 · bool 7 · string 8 · array 9 · u64 10 · i64 11 · f64 12.

## 함정 둘

**① 비정렬 적재 — `memcpy` 를 쓴다.**
문자열 길이가 가변이라 뒤따르는 u32/u64 가 정렬되지 않는다. aarch64
하드웨어는 Normal 메모리의 비정렬 적재를 처리하지만, `uint64_t *` 로
역참조하면 **컴파일러가 정렬을 가정해** 잘못된 코드를 낼 수 있다.
파서의 모든 읽기가 `memcpy` 를 지난다.

**② `ReaderTensor.shape` 는 dims 를 뒤집은 값이다.**
`gguf_reader.py:359` 가 `tuple(reversed(dims))` 를 한다. 파일에 적힌 순서를
골든값으로 쓰려면 `ReaderTensor.field.parts` 에서 읽어야 한다. 그리고
텐서 정보 필드는 `r.fields` 에 **안 들어간다** — `ReaderTensor.field` 로만 온다.

## 경계 검사

실제 모델은 0.55 GB 이고 손상될 수 있다. 커서(`struct cur`)의 모든 읽기가
크기를 검사하고, 한 번 틀리면 `err` 를 세워 더 읽지 않는다. 배열 원소 수는
`n > size/elem_size` 로 곱셈 넘침 전에 막는다.

## 돌리는 법

```bash
./.venv/bin/python tools/l2_make_gguf.py          # 파일 + 골든 헤더
L_BLOB=/tmp/claude-501/l2_test.gguf ./tools/qemu_run.sh apps/llm_l2 120
```

시험 파일은 **L3 도 같이 쓴다** — `t.q4_0`·`t.q4_k` 텐서가 그래서 들어 있다.
