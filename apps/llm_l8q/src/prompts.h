/*
 * L7b 프롬프트 — 호스트 도구(tools/l7b_host_prefill.cpp)와 보드 앱이 같이 쓴다.
 * 바꾸면 golden.inc 를 다시 만든다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

struct l7b_prompt {
	const char *name;
	const char *text;
	bool parse_special;
};

static const struct l7b_prompt l7b_prompts[] = {
	/* 채팅 템플릿 (18 토큰, L7a 'chat' 과 같다) */
	{ "chat", "<|im_start|>user\n저압 선로가 활선인지 알려줘<|im_end|>\n<|im_start|>assistant\n", true },
	/* 긴 평문 (55 토큰, L7a 'long' 과 같다) — 배치가 커질 때 */
	{ "long", "The quick brown fox jumps over the lazy dog. "
		  "다람쥐 헌 쳇바퀴에 타고파. "
		  "The quick brown fox jumps over the lazy dog. "
		  "다람쥐 헌 쳇바퀴에 타고파. 1234567890", false },
};

#define L7B_N_PROMPTS (sizeof(l7b_prompts) / sizeof(l7b_prompts[0]))
#define L7B_MAX_TOK   64	/* = n_batch = n_ubatch. 프롬프트가 한 배치에 들어가야 한다 */
#define L7B_TOPK      5
#define L7B_N_CONT    8		/* prefill 뒤 이어서 greedy 로 낼 토큰 수 */
#define L7B_N_CTX     256
