/*
 * l7b_host_prefill.cpp — L7b 정답값: 프롬프트를 **한 배치로** 넣은 뒤의
 * 마지막 위치 상위 5 와, 이어서 greedy 로 낸 토큰 8 개.
 *
 *   l7b_host_prefill <모델.gguf> > apps/llm_l7b/src/golden.inc
 *
 * 보드와 같은 문맥 설정: n_ctx 256 · n_batch = n_ubatch = 64 · flash-attn 끔.
 * 스레드는 1 (보드는 4 — L6c 와 같은 물음: "4스레드 보드가 1스레드 호스트와
 * 같은 토큰을 내는가").
 *
 * 빌드: tools/l7a_host_tok.cpp 머리말과 같다 (-Iapps/llm_l7b/src).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llama.h"
#include "prompts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

static int argmax(const float *lg, int n)
{
	int b = 0;
	for (int i = 1; i < n; i++) { if (lg[i] > lg[b]) { b = i; } }
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 2) { std::fprintf(stderr, "사용법: %s <모델.gguf>\n", argv[0]); return 1; }
	llama_backend_init();
	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers = 0;
	llama_model *model = llama_model_load_from_file(argv[1], mp);
	if (!model) { return 2; }
	llama_context_params cp = llama_context_default_params();
	cp.n_ctx = L7B_N_CTX;
	cp.n_batch = L7B_MAX_TOK;
	cp.n_ubatch = L7B_MAX_TOK;
	cp.n_threads = 1;
	cp.n_threads_batch = 1;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
	llama_context *ctx = llama_init_from_model(model, cp);
	if (!ctx) { return 3; }
	const llama_vocab *vocab = llama_model_get_vocab(model);
	const int nv = llama_vocab_n_tokens(vocab);

	std::printf("/* tools/l7b_host_prefill 이 만든다 — 손으로 고치지 않는다. */\n");
	std::printf("/* n_ctx %d · n_batch = n_ubatch = %d · 스레드 1 · flash-attn 끔 */\n\n",
		    L7B_N_CTX, L7B_MAX_TOK);
	std::printf("static const struct {\n\tint n_prompt;\n\tint prompt[%d];\n"
		    "\tint top_id[%d];\n\tfloat top_logit[%d];\n\tint cont[%d];\n"
		    "} l7b_gold[] = {\n", L7B_MAX_TOK, L7B_TOPK, L7B_TOPK, L7B_N_CONT);

	for (size_t p = 0; p < L7B_N_PROMPTS; p++) {
		const l7b_prompt &P = l7b_prompts[p];
		std::vector<llama_token> t(L7B_MAX_TOK);
		const int n = llama_tokenize(vocab, P.text, (int)std::strlen(P.text), t.data(),
					     L7B_MAX_TOK, false, P.parse_special);
		if (n <= 0) { std::fprintf(stderr, "%s: 토큰 %d\n", P.name, n); return 4; }
		llama_memory_clear(llama_get_memory(ctx), true);
		if (llama_decode(ctx, llama_batch_get_one(t.data(), n)) != 0) { return 5; }
		const float *lg = llama_get_logits_ith(ctx, -1);
		std::vector<int> idx(nv);
		std::iota(idx.begin(), idx.end(), 0);
		std::partial_sort(idx.begin(), idx.begin() + L7B_TOPK, idx.end(),
				  [&](int a, int b) { return lg[a] > lg[b]; });
		std::vector<int> top(idx.begin(), idx.begin() + L7B_TOPK);
		std::vector<float> topl;
		for (int k : top) { topl.push_back(lg[k]); }

		std::vector<int> cont;
		llama_token cur = top[0];
		for (int s = 0; s < L7B_N_CONT; s++) {
			cont.push_back(cur);
			if (s == L7B_N_CONT - 1) { break; }
			if (llama_decode(ctx, llama_batch_get_one(&cur, 1)) != 0) { return 6; }
			cur = argmax(llama_get_logits_ith(ctx, -1), nv);
		}

		std::printf("\t{ /* %s */ %d, {", P.name, n);
		for (int i = 0; i < n; i++) { std::printf("%s%d", i ? ", " : "", t[i]); }
		std::printf("},\n\t  {");
		for (int i = 0; i < L7B_TOPK; i++) { std::printf("%s%d", i ? ", " : "", top[i]); }
		std::printf("},\n\t  {");
		for (int i = 0; i < L7B_TOPK; i++) { std::printf("%s%.6ff", i ? ", " : "", topl[i]); }
		std::printf("},\n\t  {");
		for (int i = 0; i < L7B_N_CONT; i++) { std::printf("%s%d", i ? ", " : "", cont[i]); }
		std::printf("} },\n");

		std::string txt;
		for (int id : cont) {
			char b[64];
			const int m = llama_token_to_piece(vocab, id, b, sizeof b, 0, true);
			if (m > 0) { txt.append(b, m); }
		}
		std::fprintf(stderr, "%-5s %2d 토큰 · 1등 %d (%.4f) 2등 %d (%.4f) · 이어서: \"%s\"\n",
			     P.name, n, top[0], topl[0], top[1], topl[1], txt.c_str());
	}
	std::printf("};\n");
	llama_free(ctx);
	llama_model_free(model);
	return 0;
}
