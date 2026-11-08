/*
 * hub_host_ref.cpp — hubllm 의 greedy 답을 호스트에서 같은 입력·같은 문맥 설정으로 낸다.
 *
 *   hub_host_ref <모델.gguf> <n_gen> <id,id,…>
 *     → tids=<a,b,…>   (EOG 에서 멈춘다. EOG 토큰도 포함한다 — 보드와 같다)
 *
 * 보드(apps/hubllm/src/hub_llm.cpp)와 같은 문맥: n_ctx 1024 · n_batch 1024 ·
 * n_ubatch 32 · flash-attn 끔 · 단발. 호스트 1스레드 · 보드 4스레드.
 * ⚠ build-golden 의 libllama 로 링크한다 (기본 build/ 는 다른 코드다).
 *
 * 빌드: tools/l7a_host_tok.cpp 머리말과 같다 (-I 는 필요 없다).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llama.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 4) { std::fprintf(stderr, "사용법: %s <모델> <n_gen> <ids>\n", argv[0]); return 1; }
	const int n_gen = std::atoi(argv[2]);
	std::vector<llama_token> in;
	for (char *p = argv[3]; *p;) {
		in.push_back((llama_token)std::strtol(p, &p, 10));
		if (*p == ',') { p++; } else if (*p) { break; }
	}
	llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
	llama_backend_init();
	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers = 0;
	llama_model *model = llama_model_load_from_file(argv[1], mp);
	if (!model) { return 2; }
	llama_context_params cp = llama_context_default_params();
	cp.n_ctx = 1024; cp.n_batch = 1024; cp.n_ubatch = 32;
	cp.n_threads = 1; cp.n_threads_batch = 1;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
	llama_context *ctx = llama_init_from_model(model, cp);
	if (!ctx) { return 3; }
	const llama_vocab *vocab = llama_model_get_vocab(model);
	llama_sampler *smp = llama_sampler_chain_init(llama_sampler_chain_default_params());
	llama_sampler_chain_add(smp, llama_sampler_init_greedy());
	if (llama_decode(ctx, llama_batch_get_one(in.data(), (int)in.size())) != 0) { return 4; }
	std::printf("tids=");
	for (int i = 0; i < n_gen; i++) {
		llama_token t = llama_sampler_sample(smp, ctx, -1);
		std::printf(i ? ",%d" : "%d", t);
		if (llama_vocab_is_eog(vocab, t) || i == n_gen - 1) { break; }
		if (llama_decode(ctx, llama_batch_get_one(&t, 1)) != 0) { return 5; }
	}
	std::printf("\n");
	return 0;
}
