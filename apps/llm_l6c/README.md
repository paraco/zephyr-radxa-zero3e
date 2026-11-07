# L6c — 여러 토큰 (KV 캐시 · DeltaNet 상태)

L6 의 셋째 칸. L6b 는 토큰 **하나**를 넣었으므로 상태가 토큰 사이로
넘어가는지는 가리지 못했다. 상태가 안 넘어가면 **둘째 토큰부터** 틀린다.

## 기준은 측정으로 정했다 — 토큰 id 열 **전부 일치**

로짓 값은 1 ulp 교란에 4.3% 흔들린다(`docs/l6b-sensitivity.md`). 그런데
**탐욕 argmax 열은 16개까지 그 교란에 견딘다.** 호스트의 두 빌드가 똑같이
낸다:

```
off : , I am a student of the university of the north of the north of the
fma : , I am a student of the university of the north of the north of the
      ↑ 16/16 일치
```

그래서 토큰 열은 전부 일치를 요구할 수 있다. 추측이 아니라 잰 값이다.
12개로 잡았다 — 실기 23초/토큰이면 약 4.7분이고 11번째 `' north'` 까지 가
반복 구간 진입을 포함한다.

⚠ **토큰이 갈리면 그 자리에서 멈춘다.** 다음 단계의 **입력이 달라져**
  비교가 무의미해지고, 계속 돌리면 "여러 단계 실패" 로 보여 **갈린 지점을
  가린다.**

## ✅ QEMU 통과 — 2026-11-09 기준 모델 (2026-11-07)

```
[4] 토큰 12개 이어서 생성
     1단계 OK   id     11 로짓 13.078794  (호스트 13.073855 ',')          790 ms
     2단계 OK   id    353 로짓 14.422954  (호스트 14.484054 ' I')         730 ms
     3단계 OK   id   1044 로짓 18.048634  (호스트 17.968019 ' am')        720 ms
     4단계 OK   id    264 로짓 14.399913  (호스트 14.472815 ' a')         730 ms
     5단계 OK   id   5286 로짓 13.646799  (호스트 13.646438 ' student')   730 ms
     6단계 OK   id    314 로짓 16.927963  (호스트 16.856750 ' of')        730 ms
     7단계 OK   id    279 로짓 13.509903  (호스트 13.702618 ' the')       730 ms
     8단계 OK   id  11759 로짓 13.674334  (호스트 13.690474 ' university')730 ms
     9단계 OK   id    314 로짓 16.866117  (호스트 16.794298 ' of')        730 ms
    10단계 OK   id    279 로짓 11.101216  (호스트 11.174417 ' the')       730 ms
    11단계 OK   id   9897 로짓 12.701114  (호스트 12.681550 ' north')     730 ms
    12단계 OK   id    314 로짓 14.994235  (호스트 14.721532 ' of')        740 ms
[5] 생성: ", I am a student of the university of the north of"
    OK — 12/12 토큰 일치
    → KV 캐시와 DeltaNet 상태가 토큰 사이로 제대로 넘어간다
=== L6c PASS (실패 0) ===
```

### 알아낸 것 둘

**① 토큰당 시간이 늘지 않는다** (790 → 730 ms 로 평평하다). DeltaNet 은
토큰당 O(1) 이고 어텐션은 12위치뿐이라 KV 가 자라는 효과가 안 보인다.
문맥이 길어질 때의 거동은 **아직 모른다** — n_ctx 256 을 채우는 시험이
따로 필요하다.

**② 힙 최대가 L6b 보다 작다** — 114,591 KiB vs L6b 의 193,847 KiB.
L6b 는 `cb_eval` 로 노드를 관찰하므로 스케줄러가 그래프를 **쪼개** 더
많은 버퍼를 잡는다. 즉 **관찰이 메모리를 쓴다**. 평소 추론은 L6c 쪽
수치가 맞다.

| 항목 | 값 |
|---|---|
| 이미지 | 3,916,552 bytes = OTA 상한의 **46%** |
| 힙 최대 | 114,591 KiB / 240 MiB |
| 토큰당 | **730 ms** (QEMU) |

## 쓰는 법

```bash
ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1 \
  ./build.sh "$PWD/apps/llm_l6c" /tmp/claude-501/l6c
```

```bash
L_BLOB=$HOME/Projects/PiZZa/models/Qwen3.5-0.8B-Q4_K_M.gguf ./tools/qemu_run.sh apps/llm_l6c
```

기준 토큰 열을 다시 만들 때:

```bash
./tools/l6c_host_gen.sh 2>/dev/null > apps/llm_l6c/src/host_gen.inc
```
