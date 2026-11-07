/*
 * l6b_host_argmax.cpp — 호스트의 **top-5 로짓**을 찍는다.
 *
 *   l6b_host_argmax <모델.gguf> [토큰id]
 *
 * ── 왜 필요한가 ─────────────────────────────────────────────────────────
 *
 * L6b 의 층별 합 비교는 층 16 이후로 뜻을 잃는다 — 이 구조는 1 ulp 를
 * 층 15 에서 50.8% 로 증폭한다(docs/l6b-sensitivity.md). 그런데 호스트의
 * 두 빌드(`-ffp-contract=off` 와 `=fast`)가 그만큼 갈려도 **같은 글**을
 * 낸다: `hi, I am a student`.
 *
 * 즉 추론의 결과를 결정하는 양은 합이 아니라 **argmax** 다. 그것을 보드와
 * 대조해야 한다.
 *
 * `llama-eval-callback` 은 텐서의 앞 몇 값만 찍고 `llama-simple` 은 글만
 * 낸다. top-5 를 정확히 보려면 이 작은 프로그램이 필요하다.
 *
 * ⚠ **골든 빌드로 링크해야 한다.** 기본 `build/` 는 i8mm·repack·Accelerate
 *   가 켜져 있어 보드와 다른 코드다(docs/l6b-golden-plan.md).
 */
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "사용법: %s <모델.gguf> [토큰id]\n", argv[0]);
		return 1;
	}
	const char *path = argv[1];
	const llama_token tok = argc > 2 ? (llama_token)std::atoi(argv[2]) : 5834;

	llama_backend_init();

	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers = 0;
	llama_model *model = llama_model_load_from_file(path, mp);
	if (model == nullptr) {
		std::fprintf(stderr, "모델 적재 실패\n");
		return 2;
	}

	/* ⚠ 보드와 **같은 문맥 설정**이어야 한다 (apps/llm_l6b/src/main.cpp) */
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

	llama_token t = tok;
	llama_batch batch = llama_batch_get_one(&t, 1);
	if (llama_decode(ctx, batch) != 0) {
		std::fprintf(stderr, "llama_decode 실패\n");
		return 4;
	}

	const llama_vocab *vocab = llama_model_get_vocab(model);
	const int n_vocab = llama_vocab_n_tokens(vocab);
	const float *logits = llama_get_logits_ith(ctx, 0);

	std::vector<int> idx(n_vocab);
	for (int i = 0; i < n_vocab; i++) {
		idx[i] = i;
	}
	std::partial_sort(idx.begin(), idx.begin() + 5, idx.end(),
			  [&](int a, int b) { return logits[a] > logits[b]; });

	/* 합도 함께 찍는다 — 보드의 result_output 합과 대조할 수 있게 */
	double sum = 0.0, abssum = 0.0;
	for (int i = 0; i < n_vocab; i++) {
		sum += logits[i];
		abssum += logits[i] < 0 ? -logits[i] : logits[i];
	}

	std::printf("/* tools/l6b_host_argmax 가 만든다 — 손으로 고치지 않는다. */\n");
	std::printf("/* 입력 토큰 %d · 어휘 %d · 문맥 n_ctx=256 n_batch=1 n_ubatch=1 */\n",
		    tok, n_vocab);
	std::printf("/* 로짓 합 %.6f · |합| %.6f */\n", sum, abssum);
	std::printf("\n#define L6B_TOP_N 5\n");
	std::printf("static const struct { int id; float logit; const char *piece; } "
		    "l6b_host_top[] = {\n");
	for (int k = 0; k < 5; k++) {
		char buf[64] = {0};
		const int n = llama_token_to_piece(vocab, idx[k], buf, sizeof buf - 1, 0, true);
		std::string p(buf, n > 0 ? n : 0);
		/* 따옴표·역슬래시를 막는다 */
		std::string esc;
		for (char c : p) {
			if (c == '"' || c == '\\') {
				esc += '\\';
			}
			esc += (c >= 32 && c < 127) ? c : '?';
		}
		std::printf("\t{ %6d, %12.6ff, \"%s\" },\n", idx[k], logits[idx[k]],
			    esc.c_str());
	}
	std::printf("};\n");

	llama_free(ctx);
	llama_model_free(model);
	llama_backend_free();
	return 0;
}
