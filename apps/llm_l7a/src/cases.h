/*
 * L7a 시험 문장 — **호스트 도구(tools/l7a_host_tok.cpp)와 보드 앱이 같이 쓴다.**
 * 한쪽만 고치면 정답값이 어긋난다. 고친 뒤에는 golden.inc 를 다시 만든다.
 *
 * 무엇을 겨냥하나 (BPE 토크나이저가 틀리기 쉬운 곳):
 *   - 이미 아는 값(“hi” = 5834, L6b)으로 도구 자체를 맞춘다
 *   - 공백(앞·연속), 숫자(qwen 은 숫자를 한 자리씩 나눈다), 구두점
 *   - 한국어 (UTF-8 3바이트) · 이모지 (4바이트) · 섞인 글
 *   - 탭·줄바꿈·CRLF
 *   - 특수 토큰 <|im_start|> 등 (parse_special = true 인 경우만)
 *   - 빈 문자열, 긴 문장
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

struct l7a_case {
	const char *name;
	const char *text;
	bool parse_special;
};

static const struct l7a_case l7a_cases[] = {
	{ "hi",       "hi", false },
	{ "hello",    "Hello, world!", false },
	{ "spaces",   "  leading space and  double  spaces ", false },
	{ "korean",   "안녕하세요. 모루랩입니다.", false },
	{ "numbers",  "Numbers: 12345 3.14159 2026-11-08 0x7fff", false },
	{ "mixed",    "emoji \xF0\x9F\x98\x80 and 한글English혼합", false },
	{ "ws",       "tabs\tand\nnewlines\r\nend", false },
	{ "punct",    "a,b;c:d!?(e)[f]{g}\"h\" 'i' — j…", false },
	{ "chat",     "<|im_start|>user\n저압 선로가 활선인지 알려줘<|im_end|>\n<|im_start|>assistant\n", true },
	{ "chat_raw", "<|im_start|>user\nhi<|im_end|>", false },
	{ "empty",    "", false },
	{ "long",     "The quick brown fox jumps over the lazy dog. "
		      "다람쥐 헌 쳇바퀴에 타고파. "
		      "The quick brown fox jumps over the lazy dog. "
		      "다람쥐 헌 쳇바퀴에 타고파. 1234567890", false },
};

#define L7A_N_CASES (sizeof(l7a_cases) / sizeof(l7a_cases[0]))
#define L7A_MAX_TOK 256
