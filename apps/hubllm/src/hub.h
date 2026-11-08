/*
 * hubllm — 셸(C) 과 LLM 스레드(C++) 사이의 약속.
 *
 * 규약 HUBLLM/1 (보드 → 호스트, 셸 CDC 로 한 줄씩):
 *   HUBLLM/1 prompt id=<n> n=<k> ids=<a,b,…>
 *   HUBLLM/1 tok    id=<n> i=<k> tid=<token> t=<ms> <base64 조각>
 *   HUBLLM/1 done   id=<n> n_prompt=<n> n_gen=<n> ttft_ms=<n> pp_tps=<f>
 *                   tg_tps=<f> n_ctx=<n> ctx_kib=<n> heap_kib=<n> threads=<n> stop=<eos|max|abort>
 *   HUBLLM/1 err    id=<n> code=<…> msg=<…>
 *   HUBLLM/1 ctx    n_ctx=<n> ctx_kib=<n>                     (hub ctx 의 결과)
 * 호스트 → 보드 (셸 명령):
 *   hub gen id=<n> max=<tok> b64=<글> [special=1]     또는  ids=<a,b,…>
 *           [temp=<f>] [topk=<n>] [topp=<f>] [seed=<n>] [keep=1]
 *           [pres=<f>] [freq=<f>] [rep=<f>] [last_n=<n>]
 *       temp 0(기본) = greedy. temp>0 = top_k → (penalties) → top_p → temp → dist(seed).
 *       pres/freq/rep 를 주면 penalties 를 낀다 (pres = presence_penalty, Qwen 카드 권장
 *       1.5). 0/0/1 이면 없다. last_n 기본 64. 벌점은 **생성한 토큰**에만 매긴다
 *       (프롬프트 토큰은 넣지 않는다). greedy 에 주면 penalties → greedy.
 *       keep=1 = 앞 대화의 KV 에 **이어서** 넣는다(새 부분만 보낸다).
 *       keep 이면 마지막 생성 토큰도 KV 에 넣어 둔다 — 다음 프롬프트는 그 뒤부터.
 *   hub reset            KV 를 비운다
 *   hub ctx n=<n>        문맥을 n_ctx 로 다시 만든다 (KV 도 비워진다)
 *   hub clk 1416|1104    코어 클럭 (clk1416.c — L9b·L6d 에서 실기 검증한 경로)
 *                        1416 은 vdd_cpu 1.025 V 를 먼저 쓰고, 감시 스레드가 0.5 s 마다
 *                        VSEL 을 읽어 어긋나면 즉시 1104 로 내린다.
 *                        → HUBLLM/1 clk mhz=<실측> state=<1416|1104> guard_reads= guard_fail=
 *   hub abort
 *   hub info [--kv]
 *
 * 둘째 판(2026-11-08): 샘플러 · keep · ctx · model_hash(F2).
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

enum hub_kind { HUB_GEN = 0, HUB_RESET, HUB_CTX, HUB_CLK };

struct hub_req {
	int kind;
	int ctx_n;		/* HUB_CTX */
	float temp, topp;
	int topk;
	float pres, freq, rep;
	int last_n;
	uint32_t seed;
	bool keep;
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
int hub_llm_n_past(void);
/** sha256(GGUF 머리 = 텐서 데이터 앞까지) · sha256(데이터 표본 64 × 64 KiB) — 16진 64자, 없으면 "-" */
const char *hub_llm_model_hash(void);
const char *hub_llm_model_smp(void);

#ifdef __cplusplus
}
#endif
