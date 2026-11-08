/*
 * hubllm — 셸(C) 과 LLM 스레드(C++) 사이의 약속.
 *
 * 규약 HUBLLM/1 (보드 → 호스트, 셸 CDC 로 한 줄씩):
 *   HUBLLM/1 prompt id=<n> n=<k> ids=<a,b,…>
 *   HUBLLM/1 tok    id=<n> i=<k> tid=<token> t=<ms> <base64 조각>
 *   HUBLLM/1 done   id=<n> n_prompt=<n> n_gen=<n> ttft_ms=<n> pp_tps=<f>
 *                   tg_tps=<f> n_ctx=<n> ctx_kib=<n> heap_kib=<n> threads=<n> stop=<eos|max|abort>
 *   HUBLLM/1 err    id=<n> code=<…> msg=<…>
 * 호스트 → 보드 (셸 명령):
 *   hub gen id=<n> max=<tok> b64=<글> [special=1]     또는  ids=<a,b,…>
 *   hub abort
 *   hub info [--kv]
 *
 * 첫 판: greedy · 단발(매 요청 KV 를 비운다). 샘플러·대화 이어가기는 둘째 판.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct shell;

#define HUB_MAX_PROMPT_BYTES 3072
#define HUB_MAX_PROMPT_IDS   1024

struct hub_req {
	uint32_t id;
	int max_gen;
	bool use_ids;
	bool special;		/* tokenize 의 parse_special */
	int n_ids;
	int32_t ids[HUB_MAX_PROMPT_IDS];
	int text_len;
	char text[HUB_MAX_PROMPT_BYTES];
};

/** 0 = 받음. -EBUSY = 생성 중 · -EAGAIN = 모델 준비 전. */
int hub_llm_submit(const struct shell *sh, const struct hub_req *r);
void hub_llm_abort(void);
/** 상태 한 줄 이름: "loading" "ready" "busy" "noblob" "error" */
const char *hub_llm_state(void);
int hub_llm_n_ctx(void);
int hub_llm_ctx_kib(void);
int hub_llm_load_ms(void);

#ifdef __cplusplus
}
#endif
