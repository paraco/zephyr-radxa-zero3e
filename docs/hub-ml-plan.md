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
| CMD / CMD_ACK | 배포: op · xfer · BEGIN(slot·kind·size·num·sha256) · CHUNK(off + ≤1,024 B) · END · ACTIVATE / 응답 48 B (op · status · xfer · next_off · slot · num · sha256). status 0 · −1 순서 · −2 크기 · −3 sha256 · −4 파일 검사(kind 3 만) · −5 계약 · −6 빈 슬롯. Edge 는 끝낸 전송 번호·결과를 기억해 늦은 메시지에 같은 결과를 돌려준다 |
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
**정본 규격: MoruLab_MLOps `design/MLCC_LEDGER_SPEC.md` (`mlledger/1`)** — `subsys/hubml/src/ledger.c` 가 따른다.
읽기 응답: `HUBML/1 ledger seq=<n> b64=<줄>` … `HUBML/1 ledger_done last= head= total=`.

### 5.1 정상 관문(normal_gate)으로 학습 데이터 거르기 — `ml train gate=<seq>` (2026-11-09, QEMU Lm11)

`ml train uid=<hex16> [gate=<seq> | days=<d1,…>] [q=]`. 원장의 **승인된** normal_gate(decision=accept · sig 객체 있음 · ds_id 의
`ds-<uid16>-` 가 uid 와 같음)의 include/exclude 구간으로 학습·평가 데이터를 거른다. `gate`·`days` 둘 다 없으면 그 uid 의 **가장 최근**
승인 관문을 자동으로 고른다(없으면 `err code=no_gate msg=uid=…`). `days=` 는 관문 없는 예전 경로(동작·원장 줄 그대로), 둘 다 주면 `bad_arg`.
- 검사 실패 = `HUBML/1 err code=<not_found|bad_arg> msg=gate_<이유>`(decision · kind · nosig · uid · range · time · date · reason · include_empty · too_many …).
  구간은 `[날짜 yyyymmdd, "HH:MM", "HH:MM")`(끝 제외, 끝에만 `24:00`, 시작<끝, exclude 는 사유 필수), include·exclude 각 32 개 상한.
- 벡터 측정 UTC(Studio `readDaySeries` 와 같은 규칙): 앵커 = **그 .mcs 가 있는 날짜 폴더** `anchor.jsonl` 중 time_set=1 · boot_id 같음 · |edge_ms 차| 최소(같으면 앞 줄);
  `utc0 = hub_ms + (레코드 edge_ms − 앵커 edge_ms)`, 벡터 i>0 이면 `utc += dt_ms[i]`(dt_ms[0] 은 더하지 않고, quality≠0 벡터의 dt 도 누적).
  **Studio 와 다른 점: 앵커 없는 벡터는 뺀다(unanchored)** — Studio 는 수집 순서로 시각을 추정한다.
- 남기는 조건: (UTC 날짜, 하루 안 ms) 가 어떤 include 안이고 어떤 exclude 안도 아님. 밖 = `outside`, include 안이지만 exclude = `excluded`.
- 읽는 파일 = include 날짜 D 마다 D·D+1·D+2 의 합집합(밀린 데이터가 늦은 날짜 파일에 들어온다). 앵커는 폴더마다 한 번 읽어 캐시(폴더당 1024 줄 상한).
  `hubml_train` 3 패스와 `hubml_eval_holdout` 이 같은 걸러진 흐름을 본다(`struct hubml_train_cfg.gate`; NULL 이면 예전 동작).
- 원장: `dataset` 은 `days` = include 날짜, merkle = 그 날짜들의 s1.mcs 만(넘침 파일은 해시하지 않음), `gate_seq` · `gate_ds_id` 추가, counts 에
  `excluded` · `outside` · `unanchored` 추가(counts.vectors = 거른 뒤 수). `model` 의 extra 에는 `train_ds` 뒤에 `gate_seq`. 응답 끝 ` gate=<seq> unanchored= excluded= outside=`.
- 학습 최소 1000 벡터(거른 뒤 기준) — 관문 경로에서 모자라면 `err code=train msg=gate_too_few n=<수> min=1000`(days= 경로는 `msg=-61` 그대로).
- 관문 경로에서는 특징에 비유한(NaN/Inf) 값이 있는 벡터도 뺀다(dt 는 시각에 누적). dataset `counts.bad_quality` = quality≠0 수 + 비유한 수(days= 경로는 상수 0). 앵커 상한 초과분이 있으면 응답 끝에 ` anchor_over=<n>`(원장에는 싣지 않음).
- ⚠ merkle 은 include 날짜의 s1.mcs 만 해시한다 — 실제로 읽은 D+1·D+2 넘침 파일은 빠지므로 그 파일이 나중에 바뀌면 같은 dataset 이 재현되지 않는다(재현성 한계).
- ⚠ 자동 선택은 가장 최근 승인 관문을 고르고, 그 줄이 깨져 있으면(모양 위반 등) 이전 관문으로 내려가지 않고 실패한다.
- 시험: `apps/hubml_q11`(`=== Lm11 PASS`) — 기대값은 `tools/hubml_testdata.py --gate-inc`(Studio 규칙을 옮긴 두 번째 구현 `gate_ref`)가 만든 `gate_scenario.inc`.

### 5.2 시범 운영(shadow) 점수 보고 — MCOL SCORE · `ml shadow ls` (2026-11-23, QEMU Lm12 · Lm3)

Edge 는 슬롯 2 개(0 champion = 경보, 1 challenger = 시범, 점수만)를 갖는다. FEAT 묶음마다 두 슬롯 점수를 허브로 보내고, 허브는 저장하고 날별 비교 요약을 준다. Studio 와 합의한 형식(`hubml/mcol.h` 가 정본):

- **전송:** `MCOL_T_SCORE = 11`, 흐름 `MCOL_S_EVT`(3). 데이터그램 머리 `edge_ms`·`boot_id` 는 그 FEAT 묶음과 같다. FEAT 형식·흐름 수·상태 파일(MLS2)은 그대로. 수집 엔진은 흐름 안의 type 을 가리지 않고 저장하므로(`ingest.c` 변경 없음) EVT 흐름의 기존 동작이 유지된다.
- **본문(LE):** 머리 16 B = `feat_seq u32 · n u8 · flags u8(0) · rsv u16 · champ_num u32 · chal_num u32`, 뒤에 `n × 9 B { champ f32 · chal f32 · vflags u8 }`. vflags bit0/1 = champ/chal valid, bit2/3 = champ/chal alarm. 빈 슬롯 = valid 0 · num 0 · 점수 0.0f. quality≠0 벡터 = 두 valid 0.
- **읽기 규칙:** n 은 1~8, 본문 길이는 정확히 16+9n — 아니면 깨짐(`mcol_score_get` 거부, 요약은 `bad=` 로만 센다). 점수가 NaN/inf 면 valid 비트가 켜져 있어도 무효(경보 비트도 무시). flags·rsv·vflags bit4~7 은 거부하지 않고 무시. `feat_seq` 는 짝 확인용 — 허브 요약은 짝을 따지지 않는다.
- **Edge 참조 구현:** `hubml_edge_score_batch()` (`edge.c`) — 경보 = valid && score ≥ 1.0f (kind 3 점수 = d²/thr). FEAT n 이 1~8 밖이면 오류.
- **허브 명령:** `ml shadow ls uid=<hex16> days=<d1,…> [--kv]` (`--kv` 는 있어도 없어도 출력이 같다). 날마다 먼저 `HUBML/1 shadow_day uid= day= pairs=<그날 쌍 수> pairs_over=<0|1> bad=<깨진 SCORE 수>`, 이어서 그날 쌍(champ_num, chal_num)마다 `HUBML/1 shadow uid= day= champ=<uid:num|-> chal=<uid:num|-> n= valid_both= alarm_champ= alarm_chal= both= champ_only= chal_only= agree= mean_champ= mean_chal= corr=` 한 줄(벡터 수 많은 순, 같으면 먼저 나온 쌍). 통계는 쌍별로 따로 누적한다. 끝에 `HUBML/1 shadow_done n=<일수>`.
  - 받은 날 파일의 SCORE 만 읽는다. 중복은 수집 엔진 `win_check` 가 먼저 거르고(같은 흐름·seq 재전송은 저장되지 않는다), 전원 차단 등으로 파일에 물리적으로 두 번 남은 것은 `hubml_store_scan` 이 거른다.
  - `both`·`champ_only`·`chal_only`·`agree` 는 두 슬롯이 모두 유효한 벡터 안에서 센다. `alarm_*` 는 상대 슬롯이 무효여도 센다.
  - **쌍 상한 = 하루 32 쌍(`SH_MAX_PAIRS`).** 더 세지 못하므로 넘으면 `pairs=32 pairs_over=1` 이고, 넘은 쌍(33 번째부터)의 벡터는 어느 shadow 줄에도 넣지 않는다(이미 있는 쌍은 계속 누적). Python 기준도 같게 정했다.
  - 저장소 읽기 오류(`hubml_store_scan` rc<0 이고 -ENOENT 가 아님. 예: 파일 끝 레코드가 잘림)는 그날 `shadow_day` 끝에 ` err=<rc>` 를 붙인다. 부분 합계는 그대로 낸다. 장비·날 파일이 없으면 `shadow_day ... pairs=0` 한 줄뿐이다.
- **시험:** Python 기준 = `tools/hubml_testdata.py --score-inc subsys/hubml/testdata`(`shadow_lines`, `score_scenario.inc`·`score_edge_expected.inc` 를 만든다. numpy 는 이 모드에서만 쓴다). `apps/hubml_q12`(Lm12, A55)가 여러 날 시나리오(같은 날 두 쌍 · 동률 쌍 · 1e12 대 점수 · 34 쌍 · 잘린 꼬리 · 깨진 SCORE · NaN/inf)의 출력을 글자 그대로 대조하고, 같은 기대 바이트를 `apps/hubml_edge_q`(Lm3, M33)와 q12 가 각각 `hubml_edge_score_batch` 결과와 대조한다. `ml_shell.c` 는 Pearson 비트 일치를 위해 `-ffp-contract=off` 로 짓는다(`cmake/hubml.cmake`). 실기 1 회용 시뮬레이터: `tools/hubml_sim.py --score <champ.xmb> [--chal <chal.xmb>]`.
- **범위 밖:** v0(거리) 점수 · challenger 슬롯 배포를 쓰는 허브 명령 · 실펌웨어(Cortex-M7·M33) 이식.

## 6. 진행 (실기 없이 QEMU 로, 실기는 마지막 한 번)

| 단계 | 상태 | 시험 |
|---|---|---|
| M1 수집 엔진 | ✅ | Lm1 (A55): 가짜 Edge 20 · 손실 5 %·중복 3 %·뒤바뀜 · 재시작 2 종 · 자정 넘김 → 20,180 레코드 무손실·한 번씩 |
| M3 학습 · M4 평가 | ✅ | Lm2 (A55): 하루 86,400 벡터 학습 1.5 s · kind 1/3 왕복·변조 거부 · 파이썬 대조 kind1 6e-8 · kind3 0 |
| Edge 이식 | ✅ | **Lm3 (Cortex-M33 32비트)**: 같은 모델 파일 → 64 벡터 비트까지 같음 · MCOL 왕복 |
| M5a 원장 (`mlledger/1`) | ✅ | Lm4 (A55): 등록 관문(깨진 모델 거부) · model_id `<uid>:<n>` · 별칭 v0/challenger/champion(서명 없으면 거부)·승격·되돌리기 · 재시작 복원 · 반쪽 꼬리 잘라 내기 + `ledger_repair` · 중간 변조 감지(덧붙이기 막힘) · 호스트 `tools/ledger_verify.py` 사슬 대조 |
| M5b Edge 배포 전송 | ✅ | Lm5 (A55): MCOL CMD BEGIN·CHUNK(1 KiB, 창 4, go-back-N)·END·ACTIVATE ↔ Edge `edge.c` · 손실 10 %·중복 5 % → 11 KB 41 틱 · champion 슬롯·ACTIVATE 는 champion 별칭(서명) 없이 거부 · 변조(CRC 다시 맞춤) → Edge sha256 거부 −3 · kind 1 → −4 · 계약 다름 → −5 · 무응답 → 시간 초과 · 원장 deploy 사건 · Edge 점수 = 허브 비트 일치. Lm3(M33)에 같은 edge.c 받기·ACTIVATE 시험 추가 — `struct hubml_edge` 39 KB |
| 승인 서명 (ed25519 · `ml approve`) | ✅ | Lm8: ed25519 검증·SHA-512 = Mac cryptography/hashlib, 변조·s+L 거부 · Lm9: `ml approve`(mlledger/1 v0.2 §4) 실패 11 가지 거부·원장 무변화 · alias champion·normal_gate 기록 · stale(재사용) · 원장 줄 서명을 Mac 에서 재구성·검증 2/2 |
| M6 변화 감시 (`ml drift`) | ✅ | Lm10: 날 A 학습 → 날 B 1X 1.0→1.5 배 → 경보율 0.47 % → 81 %, 주범 ord_1x(z 6.29), verdict alarm. 보고만(재학습 계기 아님) |
| 시범 점수(champion/challenger 병행 보고) | ✅ QEMU · ✅ 실기 apply try (70e853dac6e2, Zephyr 4.5, 2026-11-23: Mac 가짜 Edge e5 15 분치 FEAT 112 + SCORE 112 → s3.mcs 112 레코드 · `ml shadow ls` shadow_day pairs=1 bad=0 · n=896 valid_both=896 alarm 5/5 agree 1.0000 corr 1.0000(같은 모델 두 슬롯) · 부팅 로그 mkdir -17 사라짐) | §5.2 — Lm12 (A55): SCORE 왕복 · 깨짐 거부 · `ml shadow ls` 날·쌍별 줄 Python 기준과 글자 일치 · Lm3 (M33): Edge SCORE 본문 Python 바이트 일치 |
| LLM 주간 보고 | 다음 | |
| M2 데이터 관리 · 정상 관문 · M6 감시 | | |
| hubllm 통합 (`ml` 읽기 명령 · USB 운반) | ✅ 빌드 | Lm6 (A55 가짜 셸): `ml time set` · `ml rx b64=`(HELLO → `HUBML/1 tx` HELLO_ACK) · FEAT 20·HB 4 → commit → tx ACK · `ml status/dev ls/ds ls --kv` · `ml ledger` b64 사슬 다시 계산 = `ledger_done head`. hubllm 빌드(root `/0:/ml`, hub_fs io_lock 공유, 칩 ID, 대기열 512 KiB×2, RAM 98.7 %) — **실기 확인 전** |
| 실기 (apply try) | ✅ 3차 통과 (cb268b0) | 1차 6e9ebf6 셸 스택 넘침 → c28b551 고침. 2차 2a2edf2: 첫 ml status 0.026 s ✓, 그러나 FEAT 1,800 중 451 (명령이 몰리면 기록이 안 돌았다) → cb268b0 고침(Lm6 1,500 연속으로 재현·검증). **3차 cb268b0: Mac 가짜 Edge 4 시간치 USB → FEAT 1,800 · HB 360 모두 기록·ACK, dup·oow·qfull 0 · 재부팅 뒤 장비 표·기록 위치 복원 · ml train 14,400 벡터 3.3 s far 0.35 % inject 1.0×3 pass · 원장 11 줄 사슬·head 일치 · ml drift 1.1 s stable · ml approve 거부 경로 · UART 오류 0.** 남은 작은 것(재시작 뒤 schema 0 · addr 0 ACK)은 d9a671c 에서 고침. **6dccf09 영구 적용(2026-11-09): 부팅 슬롯 main · LLM ready · ml status 0.011 s · 15 분치 112/112 기록 · 시각 앵커 anchor.jsonl(time_set 1, UTC hub_ms) · 셸 재부팅 뒤 e4 schema 4d463334·next_feat 112 유지(MLS2). 이전 영구판 b19d4d2 는 zephyr.bin.old** | 이전 기록: 6e9ebf6: 첫 `ml status` 에서 셸 멈춤(셸 스택 4 KiB 넘침) → c28b551 에서 ml 본체를 16 KiB 스레드로, QEMU 를 4 KiB 명령 스레드로 맞춰 재현·검증. 재시험 대기(허브 셸 포트를 MoruLab Studio 앱이 쥐고 있음) |

시험 씨앗(Studio 3자 시험용): `subsys/hubml/testdata/` — `golden_kind1.xmb` · `golden_kind3.xmb` · `pv.json`(64 벡터 + 기대값) ·
`sample_feat.mcs` + `sample_feat_expected.json` · 원장 `ledger_{sample,tampered,torn,unknown_kind}.jsonl` + `ledger_expected.json` ·
**허브 root 표본** `hubroot_sample/` (장비 2 대 ds · ml_state.bin · ledger.jsonl · models — Studio 폴더 열기 시험용, Lm4 에서 되살림:
`python3 tools/hubroot_extract.py <Lm4 로그> <폴더>`). 다시 만들기: `./tools/qemu_run.sh apps/hubml_q2` → `python3 tools/hubml_testdata.py <로그> subsys/hubml/testdata`.
실행: `./tools/qemu_run.sh apps/hubml_q` (Lm1) · `apps/hubml_q2` (Lm2, 뒤에 `python3 tools/xmb_ref.py <로그>`) · `./tools/qemu_edge_run.sh apps/hubml_edge_q` (Lm3).
