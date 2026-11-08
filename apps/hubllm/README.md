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

## 첫 판의 한계
- greedy · 단발(매 질문 KV 를 비운다). 샘플러·대화 이어가기·model_hash·boot_slot 은 다음 판.
- 이 질문의 greedy 토큰 열을 호스트와 아직 대조하지 않았다 (경로는 L7a·L7b 에서 대조함).
- 0.8B 모델이라 답의 사실성은 낮다 — 첫 답(저압 활선 확인)은 기술적으로 틀렸다.
