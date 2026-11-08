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

## 아직 없는 것
- boot_slot (boot.cmd 수정 = 카드 작업 필요). 온도 읽기(tsadc 드라이버 없음).
- 0.8B 모델이라 답의 사실성은 낮다 — 첫 답(저압 활선 확인)은 기술적으로 틀렸다.
