# hubllm — USB 셸에서 질문하고 답을 받는다 (HUBLLM/1)

`apps/moru`(USB CDC 셸·OTA·LED) + L7b 에서 검증한 llama 경로. 규약은 `src/hub.h`.

```bash
python3 tools/ota_test.py sdcard/hubllm.bin && python3 tools/moru_q.py "moru ota apply try"
python3 tools/hub_chat.py "질문"
python3 tools/moru_q.py "hub info --kv"
```

hubllm 도 `moru ota` 수신기를 갖고 있어 **hubllm → hubllm 갱신은 전원 재투입 없이** 된다.

## 실기 첫 결과 (2026-11-08, 1104 MHz, 4스레드)

| 항목 | 값 |
|---|---|
| USB 셸 + SMP 4 + llama 동시 기동 | ✅ (CDC 두 포트, 셸 응답) |
| 모델 적재 | 3,255 ms (부팅 1.5 s 뒤 시작) |
| 문맥 | n_ctx 1024 · n_ubatch 32 · 84 MiB · 힙 합계 약 150 MiB |
| 질문 24 토큰 | ttft 3,297 ms · prefill 7.3 토큰/초 |
| 생성 | 3.65 토큰/초 (274 ms/토큰 — L6c 와 같다) |

## 둘째 판 실기 (2026-11-08, 같은 조건)

| 항목 | 결과 |
|---|---|
| model_hash (머리 10,961,920 B) | `a0d2c7b7…10d7` — 호스트 `tools/gguf_hash.c` 와 **같다** · 보드 계산 447 ms |
| model_smp (64 × 64 KiB) | `6d307e9a…6ad6` — 호스트와 **같다** |
| 모델 크기 | GGUF 머리에서 532,517,120 B (파일 크기와 같다) |
| greedy 호스트 대조 | 24 토큰까지 같고 **25번째에서 갈렸다** — 호스트의 1·2등 차이 0.056 (13.329 대 13.273). 보드는 호스트의 2등을 골랐다. 보드·호스트 로짓 차이(약 0.1)보다 작은 동점 근처라 계산 결함으로 보지 않는다 |
| 확률 샘플링 (temp 0.7 · top_k 20 · top_p 0.95 · seed 7) | 같은 시드 두 번 → 본문이 같다 |
| 대화 이어가기 (keep=1) | n_past 60 → 117 (= 60 + 질문 17 + 답 40). 둘째 답이 첫 질문의 이름을 썼다 |
| hub ctx | 1024 → 84 MiB · 2048 → 97 MiB · 4096 → 121 MiB (힙 187 MiB / 240). 4096 에서 생성 정상 |

⚠ **greedy 토큰 열 "완전 일치" 를 기대하면 안 된다.** 긴 답에는 1·2등이 0.1 이내인 걸음이
나오고, 거기서 보드·호스트가 갈릴 수 있다. 대조는 "첫 차이까지 일치 + 그 걸음의 호스트 차이"
로 판정한다 (`HUB_MARGIN=1 hub_host_ref …`).

## 반복 억제 (88d861d, 실기)

같은 질문("대한민국의 수도는", 96 토큰 상한)으로 비교:

| 설정 | 결과 |
|---|---|
| greedy | "서울특별시의회, 서울특별시경찰청 …" 되풀이, 상한까지 |
| temp 0.7 · top_k 20 · top_p 0.8 · **pres 1.5** | 공백·구두점까지 눌려 글이 깨짐 ("대한국의", "South3") |
| 같은 설정 · pres 0.5 | 생각 모드로 빠짐 (틀에 빈 생각 블록이 없을 때) |
| greedy · **rep 1.1** | 되풀이 없음, 60 토큰에서 eos |
| temp 0.7 · top_k 20 · top_p 0.8 · **rep 1.1** + 빈 생각 블록 | 되풀이 없음, 80 토큰에서 eos |

→ 이 0.8B 에서는 **rep 1.1** 이 낫다. pres 1.5(Qwen 카드 권장)는 너무 세다.
hub_chat.py 는 기본으로 Qwen non-thinking 틀처럼 `<think>\n\n</think>\n\n` 을 답 앞에
넣는다(`--think` 로 끈다).

## 참고 자료 넣기 (hub_chat.py --ref, 2026-11-08 실기 1104 MHz · greedy · rep 1.05)

자료: tools/refs/daeheungsa.txt (다섯 줄, 약 190 토큰). 질문 "대흥사는 어디에 있고 어떤 절인가?"

| | 답 | 프롬프트 | 첫 토큰 |
|---|---|---|---|
| 자료 없음 | "**경상북도 안동시** 동산면에 위치한 절" — 지어냄, 같은 말 되풀이, 120 토큰 상한 | 22 | 3.0 s |
| 자료 있음 | "전라남도 해남군 삼산면 두륜산에 있는 절이며, 대한민국 제22교구 본사이다." — 위치 맞음 ("대한불교조계종"을 "대한민국"으로 바꿈), 32 토큰에서 eos | 216 | 28.9 s |
| 자료에 없는 것 ("몇 년에 처음 지어졌나?") | "자료에 없습니다." — 지어내지 않음 | 218 | 29.2 s |
| 자료에 있는 인물 | 초의선사·일지암만 말함 (서산대사는 빠짐), 문장이 조금 뒤틀림 | 215 | 28.8 s |

→ 사실성은 크게 나아진다. 다만 **첫 토큰이 약 29 초** — 프롬프트 처리가 7.5 토큰/초라 자료가 길수록
늦다. 자료를 쓰는 쓰임새에서는 **배치 prefill 속도가 첫 번째 병목**이다. 온도는 자료 질문 하나에
66 → 73 °C 까지 올랐다.

## repack ON (6b69824, 2026-11-08 실기 1104 MHz)

| 항목 | repack OFF | **repack ON** |
|---|---|---|
| 모델 적재 | 약 4.2 s | 8.5 s |
| 메모리 | 힙 약 150 MiB | 힙 합계 약 650 MiB (llm_heap 558 MiB — 가중치 사본 포함) |
| 참고 자료 질문 (216 토큰) 첫 토큰 | 28.9 s (prefill 7.5 토큰/초) | **14.8 s (14.6 토큰/초)** |
| 생성 | 3.65 토큰/초 | **4.01 토큰/초** |
| 같은 답 ("해남군 삼산면 두륜산 … 22 교구 본사") | | 같음 |
| greedy 호스트 대조 (호스트는 repack OFF) | 24 토큰 뒤 동점 근처(0.056)에서 갈림 | **같은 걸음에서 같은 토큰으로 갈림** (20/64 — 빈 생각 블록 4 토큰이 이제 프롬프트에 들어가 위치가 4 당겨졌다) |

## 부팅 슬롯 (F6, 6873d29 · 새 boot.scr 19,815 B 를 카드에 복사한 뒤 실기)

- apply try 로 뜬 hubllm: `hub info` → "부팅 슬롯 try" · FW_INFO `boot_slot=try` ✓
- main(카드의 zephyr.bin): **hubllm 영구 적용(2026-11-08 22시, build c6b1cc157c7e) 뒤 boot_slot=main ✓**

## 카드에 영구 적용 (2026-11-08)

- `moru ota apply` 로 hubllm(c6b1cc157c7e, 4,838,352 B, md5 c3a6cad4…d53a)을 카드의 zephyr.bin 으로.
  이전 moru(a265d15)는 **zephyr.bin.old** 로 백업됐다.
- 이제 전원만 넣으면 hubllm 이 뜬다 (부팅 약 50 s + LLM 적재 8.5 s). OTA 수신·apply try 도 그대로 된다.
- **되돌리기**: hubllm 이 부팅하지 못하면 카드를 Mac 에 꽂고 zephyr.bin.old 를 zephyr.bin 으로 되돌린다:
  `sudo sh -c 'mcopy -i /dev/rdiskNs2 ::/zephyr.bin.old /tmp/zb && mcopy -i /dev/rdiskNs2 -o /tmp/zb ::/zephyr.bin'`
  (N 은 diskutil list 로). 부팅은 하는데 이상하면 OTA 로 moru 를 다시 apply 해도 된다.

## SD 카드 — 모델 고르기·직접 읽기 (2026-11-09)

| 명령 | 하는 일 |
|---|---|
| `hub model` | 지금 모델 파일 · 해시 · 로그 번호 |
| `hub model <파일> [save]` | 재부팅해서 U-Boot 가 `models/<파일>` 을 올린다 (save = hub.conf 기본값도 바꿈) |
| `hub load <파일>` | **재부팅 없이** Zephyr 가 SD 에서 `models/<파일>` 을 읽어 다시 적재한다. `hub abort` 로 중단 |
| `sd ls [<디렉터리>]` | BLOB 파티션 목록 (기본 models) |
| `sd cmp [<MiB>]` | 지금 모델을 SD 에서 읽어 RAM 과 바이트 비교 + 속도 |

`hub load` 실기 (apply try, 1104 MHz):
- `nothere.gguf` → `err code=load_open msg=-2_…` · 지금 모델은 그대로.
- q8out → q4km: 507 MiB 읽기 58.7 s (8.66 MB/s, PIO · 25 MHz) + 적재 8.5 s. 해시 a0d2c7b7… = 1단계 실측과 같다.
  내리면 llm_heap 616 → 0 MiB 로 다 돌아온다. 생성 정상 (tg 4.01 tok/s).
- 읽는 중 `hub abort` → 모델 없음(state noblob, `hub gen` 은 not_ready). 그 상태에서 `hub load` 로 회복된다.
- q4km → q8out: 566 MiB 읽기 65.4 s + 적재 8.8 s, 해시 27d9b2cd… · llm_heap 616 MiB · tg 4.25 tok/s (부팅 적재와 같다).
- 바꾼 모델은 이번 부팅에만 쓴다. 다음 부팅 기본값은 `hub model <파일> save`.
- 읽는 동안 `sd ls`·`sd cmp` 는 `err code=busy` (FatFs 를 한 번에 하나만 쓴다).

## SD 에 로그 직접 쓰기 — 2b (2026-11-09)

- 부팅하면 쓰기 스레드가 `logs/hub-<N>.txt` 를 만들고 2 초마다 새 줄을 덧붙인 뒤 fs_sync 한다.
  N = max(logs/ 의 가장 큰 번호 + 1, U-Boot 가 넘긴 다음 번호). hub.conf 는 Zephyr 가 쓰지 않는다.
- 쓰는 동안 U-Boot 는 로그를 저장하지 않는다('HLOG' 를 지움). 쓰기가 실패하면 예전처럼 재부팅 때 U-Boot 가 쓴다.
- `hub reboot`·`hub model <파일>` 은 재부팅 직전에 마지막 줄을 쓴다.
- 명령: `sd log [on|off]` · `sd cat <경로> [바이트]` · `sd wtest [MiB]`. `hub model` 에 `logfile=`.

실기 (apply try):
| 시험 | 결과 |
|---|---|
| 첫 쓰기 `sd wtest 1` · `16` | bad 0 · 쓰기 2.3~3.3 MB/s · 읽기 8.6 MB/s |
| 쓴 뒤 셸 재부팅 | U-Boot 가 모델(해시 같음)·로그를 정상 처리 — FAT 이상 없음 |
| 실시간 기록 | boot·done 줄이 바로 파일에 · 생성 4.25 tok/s 그대로 |
| 질문 직후 `hub reboot` | 마지막 done 줄까지 들어감 (471 → 754 B) |
| **전원 차단** | hub-13.txt 471 B 그대로 남음 — 1단계(재부팅 때만 저장)의 빈틈을 메웠다 |

한계: 전원이 끊기면 마지막 동기화(≤ 2 s) 뒤 줄은 잃고, FAT 갱신 중이면 메타데이터가 어긋날 수 있다 ·
로그는 중계 구역 크기 때문에 부팅 한 번에 1 MiB 에서 멈춘다 · `hub load` 동안은 기록이 밀렸다가 끝난 뒤 들어간다.
⚠ 옛 판(e4d9a75 이전)이 재부팅할 때 U-Boot 가 쓰는 번호는 hub.conf + 1 이라, 새 판이 쓴 파일을 덮을 수 있다
(시험 중 hub-12.txt 가 그렇게 덮였다). 새 판을 영구 적용하면 없어진다.

## SD 읽기 속도 (2026-11-09)

- `sd info`: 카드는 이미 **50 MHz · 4비트 고속**이다 (CRU sel 3 = 100 MHz ÷ 2). 그전에 "25 MHz" 라고 적은 것은 틀렸다.
- `sd raw 16`: 파일 시스템 없이 256 KiB 씩 → 12.6 MB/s. FatFs 경유 8.6 MB/s 와의 차이는 f_read 가
  묶음(8 KiB)마다 요청을 끊는 손실이다. 남은 한계(이론 25 MB/s)는 PIO(CPU 가 FIFO 를 워드씩 읽음).
- `hub load` 는 FAT32 묶음 연결을 직접 따라가 연속 구간을 최대 1 MiB 씩 읽는다(mode=chain, 실패하면 fatfs):
  q4km 507 MiB 58.7 → **39.8 s** (12.75 MB/s) · q8out 566 MiB 65.4 → **44.4 s**. 해시 같음 ·
  `sd cmp 600`(FatFs 경로로 전체 비교) bad 0 · 생성 정상.
- 더 빠르게 하려면 IDMAC(컨트롤러 DMA)이 필요하다.

## 아직 없는 것
- boot_slot (boot.cmd 수정 = 카드 작업 필요). 온도 읽기(tsadc 드라이버 없음).
- 0.8B 모델이라 답의 사실성은 낮다 — 첫 답(저압 활선 확인)은 기술적으로 틀렸다.
