/*
 * gguf_hash.c — hubllm 의 model_hash · model_smp 를 호스트에서 같은 규칙으로 계산한다.
 *
 *   gguf_hash <모델.gguf>
 *     → model_bytes=<n> data_off=<n> model_hash=<64hex> model_smp=<64hex>
 *
 * 보드(apps/hubllm/src/hub_llm.cpp model_scan)와 **같은 gguf 코드·같은 sha256.c**
 * 를 쓴다. 규칙:
 *   model_hash = sha256(파일[0 : data_off])            — 머리·KV·텐서 정보
 *   model_smp  = sha256(데이터 구간의 균등 64 곳 × 64 KiB) — 가중치 표본
 *
 * 빌드:
 *   L=../upstream/llama.cpp
 *   cc -O2 -I$L/ggml/include -Iapps/hubllm/src tools/gguf_hash.c apps/hubllm/src/sha256.c \
 *      -L$L/build-golden/bin -lggml-base -Wl,-rpath,$L/build-golden/bin -o /tmp/claude-501/gguf_hash
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include "gguf.h"
#include "sha256.h"

static void hex(const uint8_t *d) { for (int i = 0; i < 32; i++) { printf("%02x", d[i]); } }

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "사용법: %s <모델.gguf>\n", argv[0]); return 1; }
	struct gguf_init_params gp = { .no_alloc = true, .ctx = NULL };
	struct gguf_context *g = gguf_init_from_file(argv[1], gp);
	if (!g) { fprintf(stderr, "GGUF 를 못 읽었다\n"); return 2; }
	const size_t off = gguf_get_data_offset(g);
	size_t end = 0;
	for (int64_t i = 0; i < gguf_get_n_tensors(g); i++) {
		const size_t e = gguf_get_tensor_offset(g, i) + gguf_get_tensor_size(g, i);
		if (e > end) { end = e; }
	}
	gguf_free(g);

	FILE *f = fopen(argv[1], "rb");
	uint8_t *buf = malloc(off > 65536 ? off : 65536);
	if (!f || !buf || fread(buf, 1, off, f) != off) { return 3; }
	uint8_t d[32];
	struct sha256 h;
	sha256_init(&h);
	sha256_update(&h, buf, off);
	sha256_final(&h, d);
	printf("model_bytes=%zu data_off=%zu model_hash=", off + end, off);
	hex(d);

	const size_t CH = 65536, NS = 64;
	const size_t n = end > CH ? CH : end;
	sha256_init(&h);
	for (size_t k = 0; k < NS; k++) {
		const size_t pos = (end > CH) ? off + (end - CH) * k / (NS - 1) : off;
		if (fseek(f, (long)pos, SEEK_SET) || fread(buf, 1, n, f) != n) { return 4; }
		sha256_update(&h, buf, n);
	}
	sha256_final(&h, d);
	printf(" model_smp=");
	hex(d);
	printf("\n");
	fclose(f);
	return 0;
}
