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

## 아직 없는 것
- boot_slot (boot.cmd 수정 = 카드 작업 필요). 온도 읽기(tsadc 드라이버 없음).
- 0.8B 모델이라 답의 사실성은 낮다 — 첫 답(저압 활선 확인)은 기술적으로 틀렸다.
