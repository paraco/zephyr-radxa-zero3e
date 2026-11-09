# HUB AI — 현장 ML 허브 (hubml) 계획 · 계약 · 진행

> 정본은 이 문서와 `subsys/hubml/include/hubml/*.h`(바이트 배치). Studio 쪽 짝 계획은 MoruLab_MLOps
> `design/MLCC_HUB_ML_PLAN.md`(Studio 팩 **MLCC**). 첫 적용은 공장 모터 장애 예지(MPDM, MFEAT34, advisory — 경보만).

## 1. 역할 (MLCC 계획과 합의, 2026-11-09)

| 곳 | 맡는 일 |
|---|---|
| **HUB AI (RK3566 Zephyr, 이 모듈)** | 현장 데이터·현장 원장의 정본 — 수집 · 저장 · 정상 관문 · 학습 · 평가 · 배포 관리 · 감시 · LLM 보고 |
| Studio (MLCC) | 사람 승인(서명) · 여러 현장 · 무거운 학습 · 상위 원장 · 계약. Studio 모델도 허브 관문을 거친다. 자동 승격 없음 |
| Edge | 특징 추출 · 판정(경보만) · 보낼 함 · 시범 점수(두 번째 슬롯, 펌웨어 확인 필요) |

## 2. Edge MCU 전제 (2026-11-09 사용자)

Edge 는 **Cortex-M7(Teensy 4.1) 또는 Cortex-M33(RP2350 · nRF54L15) — 32비트 Arm**.
- **float32 만 쓴다.** M33 의 FPU 는 단정도다. Edge 쪽 모델(kind 3)·평가기는 float32 로만 계산한다.
  허브가 double 로 학습해도 내보낼 때 float32 로 바꾸고 골든·문턱은 float32 평가기로 다시 잰다.
- **같은 C 코드**(`mcol.c`, `xmb.c`)를 Edge 에서 그대로 컴파일한다 — 64비트 허브와 32비트 Edge 가 비트까지 같다
  (QEMU Lm3, 아래). 펌웨어는 FMA 합치기 차이를 막으려면 `-ffp-contract=off` 를 권한다(허용 오차 1e-5 안이면 무관).
- **RAM:** kind 3 모델 11.2 KB (`-DXMB_MAX_FEAT=34`). kind 1(트리 2,400 노드 상한)은 40 KB.
- **보낼 함:** 특징만 하루 약 12 MB(140 B/s). SD 가 있는 Teensy 4.1 은 30 일 이상, SD 가 없는 M33 보드는
  플래시 크기만큼(수 시간~며칠) — 끊김이 길면 원파형 표본부터 버리고 특징은 지킨다(정책은 R0 에서).

## 3. 수집 규약 MCOL/1 (`hubml/mcol.h`)

HaLow(NRC7394 BridgeRPC 위 UDP, ≤ 1,200 B/데이터그램, TCP 없음) 한 데이터그램 = 레코드 하나.
`[머리 24 B][본문][CRC32]` — 머리: `'MC' · ver · type · edge_sid u16 · stream · flags · seq u32 · edge_ms u64 · len u16 · boot_id u16`.

| type | 본문 |
|---|---|
| HELLO | **24 B**: uid u64 · fw u32 · contract u16 · nfeat u8 · rsv · sensor_hash u32 · **schema_hash u32** (MLCC 요청으로 추가) |
| HELLO_ACK | sid u16 · rsv · hub_ms u64 · raw_period_s u32 |
| FEAT | n u8 · contract u8 · rsv u16 · n × { f32×contract · regime u8 · quality u8 · dt_ms u16 } (34 특징 8 개 = 1,124 B) |
| RAW_HDR / RAW_CHUNK | 원파형 표본 (머리 + 1,100 B 조각) — 형식은 R0 에서 확정 |
| EVT · HB | 이벤트 · 생존 신호 (MLCC: EVT 에 kind·gate·alarm_id, FEAT 에 model_id·shadow_score 제안 — IF-TLM/2 에서 확정) |
| ACK | 흐름마다 { stream u8 · next u32 · sack u32 } — **허브가 SD 에 기록(fs_close)한 뒤에만** |

저장: `<root>/ds/<uid>/<yyyymmdd>/s<흐름>.mcs` = 머리 16 B(`MCS1`·uid·stream·ver) + `[u16 길이][데이터그램 원본]…`,
상태 `<root>/ml_state.bin`. 읽을 때 (흐름, seq) 로 중복을 거른다(상태 파일 전 끊김).

## 4. 모델 종류 (`hubml/xmb.h`, IF-PKG/2 에 제안)

| kind | 무엇 | 쓰는 곳 | 근거 |
|---|---|---|---|
| 1 | 트리 (XMB1 그대로) — Isolation Forest 를 잎 값 `k·(c(ψ)−경로)/T` 로 담을 수 있다 | 규칙 대조용 · 쓰지 않음 | QEMU Lm2: **단일 특징 ×1.3~1.6 변화 검출 0.5~0.9 %(오경보율 수준)** |
| **3** | **통계 기준선** — 운전 구간(load_ratio)별 mean·sd·촐레스키 L·문턱, 점수 = d²/문턱 (>1 경보) | **Edge** | 오경보 0.45 % · ×1.3 고장 3 종 100 % · float32 = double 결과 |

⚠ MLCC 계획의 "Isolation Forest = 트리라 .xmb 에 맞음" 은 **형식상 맞지만 검출이 안 된다** — Edge 모델은 kind 3(MLCC 의 `baseline_stats`).
⚠ LWCT 펌웨어는 n_feat 22..24 만 받는다. MPDM(34)·kind 3 은 **MPDM Edge 펌웨어(R0)** 가 받아야 한다 — 펌웨어 릴리스.

## 5. 허브 명령 HUBML/1 (MLCC 문법 채택)

줄 머리 `HUBML/1 <kind> k=v …`, `--kv`. 읽기 전용부터:
`ml status --kv` · `ml dev ls --kv` (uid sid contract schema fw last_seen_ms state + 흐름별 next/sack/gap) ·
`ml ds ls uid=<hex> --kv` · `ml ledger since=<seq> max=<n>` (원장 JSONL 줄 b64, seq·prev_sha256) · 원본은 `sd ls`·`sd cat`.
쓰기(M5 이후, 사용자 승인 뒤): `ml approve …` · `ml model put …` · `ml train …` · `ml deploy …`.
원장 = 허브 SD 의 덧붙이기 JSONL + 해시 사슬 + 별칭(v0/champion/challenger/shadow) — MLflow 서버는 쓰지 않는다.

## 6. 진행 (실기 없이 QEMU 로, 실기는 마지막 한 번)

| 단계 | 상태 | 시험 |
|---|---|---|
| M1 수집 엔진 | ✅ | Lm1 (A55): 가짜 Edge 20 · 손실 5 %·중복 3 %·뒤바뀜 · 재시작 2 종 · 자정 넘김 → 20,180 레코드 무손실·한 번씩 |
| M3 학습 · M4 평가 | ✅ | Lm2 (A55): 하루 86,400 벡터 학습 1.5 s · kind 1/3 왕복·변조 거부 · 파이썬 대조 kind1 6e-8 · kind3 0 |
| Edge 이식 | ✅ | **Lm3 (Cortex-M33 32비트)**: 같은 모델 파일 → 64 벡터 비트까지 같음 · MCOL 왕복 |
| M5 원장(JSONL·해시 사슬·별칭)·배포 | 다음 | |
| M2 데이터 관리 · 정상 관문 · M6 감시 | | |
| hubllm 통합 (`ml` 명령 · USB CDC 운반) | | |
| 실기 한 번 (apply try) | | |

시험 씨앗(Studio 3자 시험용): `subsys/hubml/testdata/` — `golden_kind1.xmb` · `golden_kind3.xmb` · `pv.json`(64 벡터 + 기대값) ·
`sample_feat.mcs` + `sample_feat_expected.json`. 다시 만들기: `./tools/qemu_run.sh apps/hubml_q2` → `python3 tools/hubml_testdata.py <로그> subsys/hubml/testdata`.
실행: `./tools/qemu_run.sh apps/hubml_q` (Lm1) · `apps/hubml_q2` (Lm2, 뒤에 `python3 tools/xmb_ref.py <로그>`) · `./tools/qemu_edge_run.sh apps/hubml_edge_q` (Lm3).
