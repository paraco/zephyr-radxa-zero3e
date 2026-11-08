/*
 * l7a_host_tok.cpp — L7a 토크나이저 정답값을 뽑는다.
 *
 *   l7a_host_tok <모델.gguf> > apps/llm_l7a/src/golden.inc
 *
 * 시험 문장은 apps/llm_l7a/src/cases.h (보드와 같은 파일).
 * 어휘만 읽는다(vocab_only) — 토크나이저는 가중치와 무관하다.
 * add_special = false (BOS 를 붙이지 않는다). parse_special 은 문장마다.
 * 되돌림(detokenize, unparse_special = 문장의 parse_special)도 함께 적는다 —
 * 원문과 같지 않을 수 있으므로 **호스트의 결과**를 정답으로 삼는다.
 *
 * 빌드 (골든 빌드의 libllama 와 같은 소스여야 한다):
 *   L=../upstream/llama.cpp
 *   c++ -std=c++17 -O1 -I$L/include -I$L/ggml/include -Iapps/llm_l7a/src \
 *       tools/l7a_host_tok.cpp -L$L/build-golden/bin -lllama -lggml -lggml-base \
 *       -Wl,-rpath,$L/build-golden/bin -o /tmp/claude-501/l7a_host_tok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llama.h"
#include "cases.h"

#include <cstdio>
#include <cstring>
#include <vector>

static void c_str_lit(const char *s, int n)
{
	std::putchar('"');
	for (int i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') { std::printf("\\%c", c); }
		else if (c == '\n') { std::printf("\\n"); }
		else if (c == '\r') { std::printf("\\r"); }
		else if (c == '\t') { std::printf("\\t"); }
		else if (c < 0x20 || c >= 0x7f) { std::printf("\\x%02X\"\"", c); }
		else { std::putchar(c); }
	}
	std::putchar('"');
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "사용법: %s <모델.gguf>\n", argv[0]);
		return 1;
	}
	llama_backend_init();
	llama_model_params mp = llama_model_default_params();
	mp.vocab_only = true;
	mp.n_gpu_layers = 0;
	llama_model *model = llama_model_load_from_file(argv[1], mp);
	if (model == nullptr) {
		std::fprintf(stderr, "모델 적재 실패\n");
		return 2;
	}
	const llama_vocab *vocab = llama_model_get_vocab(model);

	std::printf("/* tools/l7a_host_tok 이 만든다 — 손으로 고치지 않는다. */\n");
	std::printf("/* 어휘 %d 개 · add_special=false · 문장은 cases.h */\n\n",
		    llama_vocab_n_tokens(vocab));
	std::printf("#define L7A_HOST_N_VOCAB %d\n\n", llama_vocab_n_tokens(vocab));

	std::vector<int> off, cnt;
	std::vector<llama_token> all;
	std::vector<std::vector<char>> back;
	for (size_t i = 0; i < L7A_N_CASES; i++) {
		const l7a_case &c = l7a_cases[i];
		std::vector<llama_token> t(L7A_MAX_TOK);
		const int n = llama_tokenize(vocab, c.text, (int)std::strlen(c.text), t.data(),
					     L7A_MAX_TOK, false, c.parse_special);
		if (n < 0) {
			std::fprintf(stderr, "%s: 토큰이 %d 개 — L7A_MAX_TOK 를 키운다\n", c.name, -n);
			return 3;
		}
		off.push_back((int)all.size());
		cnt.push_back(n);
		all.insert(all.end(), t.begin(), t.begin() + n);
		std::vector<char> b(4096);
		const int m = llama_detokenize(vocab, t.data(), n, b.data(), (int)b.size(),
					       false, c.parse_special);
		b.resize(m < 0 ? 0 : m);
		back.push_back(b);
		std::fprintf(stderr, "%-9s %3d 토큰  되돌림 %s\n", c.name, n,
			     (m == (int)std::strlen(c.text) && std::memcmp(b.data(), c.text, m) == 0)
			     ? "원문과 같다" : "**원문과 다르다**");
	}
	std::printf("static const int l7a_gold_ids[] = {");
	for (size_t i = 0; i < all.size(); i++) {
		std::printf("%s%s%d", i ? "," : "", (i % 12) ? " " : "\n\t", all[i]);
	}
	std::printf("\n};\n\n");
	std::printf("static const struct { int off, n; const char *back; int back_len; } l7a_gold[] = {\n");
	for (size_t i = 0; i < L7A_N_CASES; i++) {
		std::printf("\t{ %4d, %3d, ", off[i], cnt[i]);
		c_str_lit(back[i].data(), (int)back[i].size());
		std::printf(", %d },\t/* %s */\n", (int)back[i].size(), l7a_cases[i].name);
	}
	std::printf("};\n");
	llama_model_free(model);
	return 0;
}
