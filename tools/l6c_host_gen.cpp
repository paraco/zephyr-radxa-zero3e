/*
 * l6c_host_gen.cpp — 호스트의 **탐욕 생성 토큰 열**을 뽑는다.
 *
 *   l6c_host_gen <모델.gguf> <첫토큰id> <개수>
 *
 * ── 왜 필요한가 ─────────────────────────────────────────────────────────
 *
 * L6c 는 토큰을 **이어서** 넣어 KV 캐시와 DeltaNet 재귀 상태가 토큰 사이로
 * 제대로 넘어가는지 가린다. 상태가 안 넘어가면 둘째 토큰부터 틀린다 —
 * 그래서 **생성된 토큰 열**이 그 자체로 시험이다.
 *
 * ⚠ 보드와 **같은 문맥 설정**이어야 한다. n_ctx·n_batch·n_ubatch·
 *   flash-attn 이 다르면 그래프가 달라져 비교가 무의미해진다.
 *
 * ⚠ 골든 빌드로 링크해야 한다 — 기본 `build/` 는 i8mm·repack·Accelerate 가
 *   켜져 있어 보드와 다른 코드다(docs/l6b-golden-plan.md).
 *
 * ⚠ 로짓이 1 ulp 교란에 4.3% 흔들리는데도 argmax 는 견딘다
 *   (docs/l6b-sensitivity.md). 그래서 **토큰 id 열**을 기준으로 삼는다 —
 *   로짓 값을 기준으로 삼으면 의미 없는 실패가 난다.
 */
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "사용법: %s <모델.gguf> <첫토큰id> <개수>\n", argv[0]);
		return 1;
	}
	const char *path = argv[1];
	const llama_token first = (llama_token)std::atoi(argv[2]);
	const int n_gen = std::atoi(argv[3]);

	llama_backend_init();

	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers = 0;
	llama_model *model = llama_model_load_from_file(path, mp);
	if (model == nullptr) {
		std::fprintf(stderr, "모델 적재 실패\n");
		return 2;
	}

	/* ⚠ 보드와 같은 설정 (apps/llm_l6c/src/main.cpp) */
	llama_context_params cp = llama_context_default_params();
	cp.n_ctx           = 256;
	cp.n_batch         = 1;
	cp.n_ubatch        = 1;
	cp.n_threads       = 1;
	cp.n_threads_batch = 1;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;

	llama_context *ctx = llama_init_from_model(model, cp);
	if (ctx == nullptr) {
		std::fprintf(stderr, "문맥 생성 실패\n");
		return 3;
	}

	const llama_vocab *vocab = llama_model_get_vocab(model);
	const int n_vocab = llama_vocab_n_tokens(vocab);

	std::vector<llama_token> out;
	std::vector<float> top_logit;
	llama_token t = first;

	for (int step = 0; step < n_gen; step++) {
		llama_batch b = llama_batch_get_one(&t, 1);
		if (llama_decode(ctx, b) != 0) {
			std::fprintf(stderr, "llama_decode 실패 (step %d)\n", step);
			return 4;
		}
		const float *lg = llama_get_logits_ith(ctx, 0);
		int best = 0;
		for (int i = 1; i < n_vocab; i++) {
			if (lg[i] > lg[best]) {
				best = i;
			}
		}
		out.push_back(best);
		top_logit.push_back(lg[best]);
		t = best;
	}

	std::printf("/* tools/l6c_host_gen 이 만든다 — 손으로 고치지 않는다. */\n");
	std::printf("/* 첫 토큰 %d · %d개 생성 · n_ctx=256 n_batch=1 n_ubatch=1 */\n",
		    first, n_gen);
	std::printf("/* ⚠ 기준은 **토큰 id 열**이다. 로짓은 1 ulp 교란에 4.3%% 흔들린다\n");
	std::printf(" *   (docs/l6b-sensitivity.md) — 값을 기준으로 삼으면 의미 없는\n");
	std::printf(" *   실패가 난다. 로짓은 참고로만 적는다. */\n\n");
	std::printf("#define L6C_FIRST_TOKEN %d\n", first);
	std::printf("#define L6C_N_GEN %d\n\n", n_gen);
	std::printf("static const struct { int id; float logit; const char *piece; } "
		    "l6c_host_gen[] = {\n");
	std::string text;
	for (size_t k = 0; k < out.size(); k++) {
		char buf[64] = {0};
		const int n = llama_token_to_piece(vocab, out[k], buf, sizeof buf - 1, 0, true);
		std::string p(buf, n > 0 ? n : 0);
		text += p;
		std::string esc;
		for (char c : p) {
			if (c == '"' || c == '\\') {
				esc += '\\';
			}
			esc += (c >= 32 && c < 127) ? c : '?';
		}
		std::printf("\t{ %6d, %12.6ff, \"%s\" },\n", out[k], top_logit[k], esc.c_str());
	}
	std::printf("};\n");

	/* 사람이 읽을 글도 적는다 — 숫자만 보면 맞는지 감이 안 온다 */
	std::string esc;
	for (char c : text) {
		if (c == '"' || c == '\\') {
			esc += '\\';
		}
		esc += (c >= 32 && c < 127) ? c : '?';
	}
	std::printf("\n/* 생성된 글: \"%s\" */\n", esc.c_str());

	llama_free(ctx);
	llama_model_free(model);
	llama_backend_free();
	return 0;
}
