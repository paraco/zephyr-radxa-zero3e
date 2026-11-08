/*
 * gguf_embd_q8.c — token_embd.weight(= 묶인 출력층)만 q8_0 으로 바꾼 GGUF 를 만든다.
 *
 *   gguf_embd_q8 <입력.gguf> <출력.gguf>
 *
 * 왜: 생성의 가장 큰 몫이 출력층 q6_K 행렬곱(약 102 ms/토큰, 6비트 풀기 계산에 묶임 —
 * docs/l8p-prefill-profile.md). q8_0 점곱은 풀기가 거의 없다.
 *
 * 방법: q6_K 를 f32 로 풀고(ggml to_float) q8_0 으로 다시 담는다(ggml_quantize_chunk).
 *   ⚠ 원본(F16)에서 바로 만든 q8_0 과 같지 않다 — q6_K 오차 위에 q8_0 반올림이 얹힌다
 *     (q8_0 이 더 촘촘해 더해지는 오차는 작다). 다른 텐서·KV 는 바이트 그대로 옮긴다.
 *
 * 빌드:
 *   L=../upstream/llama.cpp
 *   cc -O2 -I$L/ggml/include tools/gguf_embd_q8.c -L$L/build-golden/bin -lggml-base -lggml \
 *      -Wl,-rpath,$L/build-golden/bin -o /tmp/claude-501/gguf_embd_q8
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ggml.h"
#include "gguf.h"

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "사용법: %s <입력.gguf> <출력.gguf>\n", argv[0]); return 1; }
	struct ggml_context *src_ctx = NULL;
	struct gguf_init_params ip = { .no_alloc = false, .ctx = &src_ctx };
	struct gguf_context *src = gguf_init_from_file(argv[1], ip);
	if (!src) { fprintf(stderr, "입력을 못 읽었다\n"); return 2; }

	struct gguf_context *dst = gguf_init_empty();
	gguf_set_kv(dst, src);

	struct ggml_tensor *te = ggml_get_tensor(src_ctx, "token_embd.weight");
	if (!te || te->type != GGML_TYPE_Q6_K) { fprintf(stderr, "token_embd 가 q6_K 가 아니다\n"); return 3; }
	const int64_t ncol = te->ne[0], nrow = te->ne[1];
	const size_t q8sz = ggml_row_size(GGML_TYPE_Q8_0, ncol) * nrow;
	struct ggml_init_params gp = { .mem_size = q8sz + 4096, .mem_buffer = NULL, .no_alloc = false };
	struct ggml_context *nctx = ggml_init(gp);
	struct ggml_tensor *nt = ggml_new_tensor_2d(nctx, GGML_TYPE_Q8_0, ncol, nrow);
	ggml_set_name(nt, "token_embd.weight");

	float *row = malloc(sizeof(float) * ncol * 256);
	const struct ggml_type_traits *tr = ggml_get_type_traits(GGML_TYPE_Q6_K);
	const size_t srow = ggml_row_size(GGML_TYPE_Q6_K, ncol);
	double max_abs_err = 0;
	for (int64_t r0 = 0; r0 < nrow; r0 += 256) {
		const int64_t nr = (nrow - r0) < 256 ? (nrow - r0) : 256;
		for (int64_t r = 0; r < nr; r++) {
			tr->to_float((const char *)te->data + (r0 + r) * srow, row + r * ncol, ncol);
		}
		/* ⚠ start 는 src·dst 양쪽에 적용된다 — 덩이 버퍼를 넘기므로 0 으로 하고 dst 를 옮긴다 */
		ggml_quantize_chunk(GGML_TYPE_Q8_0, row,
				    (char *)nt->data + r0 * ggml_row_size(GGML_TYPE_Q8_0, ncol), 0, nr, ncol, NULL);
		if (r0 == 0) {	/* 첫 덩이로 되돌림 오차를 가늠 */
			float *back = malloc(sizeof(float) * ncol);
			const struct ggml_type_traits *t8 = ggml_get_type_traits(GGML_TYPE_Q8_0);
			for (int64_t r = 0; r < nr; r++) {
				t8->to_float((const char *)nt->data + r * ggml_row_size(GGML_TYPE_Q8_0, ncol), back, ncol);
				for (int64_t c = 0; c < ncol; c++) {
					const double e = back[c] - row[r * ncol + c];
					if ((e < 0 ? -e : e) > max_abs_err) { max_abs_err = e < 0 ? -e : e; }
				}
			}
			free(back);
		}
	}
	free(row);

	for (int64_t i = 0; i < gguf_get_n_tensors(src); i++) {
		const char *n = gguf_get_tensor_name(src, i);
		gguf_add_tensor(dst, strcmp(n, "token_embd.weight") ? ggml_get_tensor(src_ctx, n) : nt);
	}
	if (!gguf_write_to_file(dst, argv[2], false)) { fprintf(stderr, "쓰기 실패\n"); return 4; }
	printf("token_embd %lld x %lld : q6_K %.1f MiB -> q8_0 %.1f MiB · 첫 256 행 q6→q8 최대 절대 오차 %.6f\n",
	       (long long)ncol, (long long)nrow, (double)ggml_nbytes(te) / 1048576.0,
	       (double)q8sz / 1048576.0, max_abs_err);
	return 0;
}
