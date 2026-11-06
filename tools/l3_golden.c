/*
 * L3 골든값 생성기 — **ggml 의 C 구현 그 자체**를 호출한다.
 *
 * 파이썬 역양자화기(gguf-py/gguf/quants.py)를 쓰지 않는 이유: L3 의 허용
 * 오차가 0(비트 일치)인데, 파이썬과 C 는 연산 결합 순서가 달라 반올림이
 * 달라질 수 있다. 골든값은 우리가 포팅할 바로 그 코드에서 나와야 한다.
 *
 * ggml-quants.c 를 소스로 포함해 한 번역 단위로 묶는다.
 *
 * 쓰는 법: l3_golden <gguf> <데이터시작오프셋> <q4_0오프셋> <q4_k오프셋>
 * 출력: C 헤더 (f32 를 **비트 패턴**으로 찍는다 — 10진수로는 비트 일치를
 *      확인할 수 없다)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ggml-quants.h"

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "사용법: %s <gguf> <data_off> <q4_0_off> <q4_k_off>\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	long data_off = strtol(argv[2], NULL, 0);
	long q40_off = strtol(argv[3], NULL, 0);
	long q4k_off = strtol(argv[4], NULL, 0);

	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 2; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(sz);
	if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); return 2; }
	fclose(f);

	/* t.q4_0 — 논리 형태 (32, 4): 4행 × 32원소 = 블록 4개 */
	const int q40_blocks = 4;
	const int q40_n = q40_blocks * QK4_0;
	float *y40 = malloc(q40_n * sizeof(float));
	dequantize_row_q4_0((const block_q4_0 *)(buf + data_off + q40_off), y40, q40_n);

	/* t.q4_k — 논리 형태 (256, 2): 2행 × 256원소 = 슈퍼블록 2개 */
	const int q4k_blocks = 2;
	const int q4k_n = q4k_blocks * QK_K;
	float *y4k = malloc(q4k_n * sizeof(float));
	dequantize_row_q4_K((const block_q4_K *)(buf + data_off + q4k_off), y4k, q4k_n);

	printf("/*\n");
	printf(" * l3_golden.h — 생성 파일이다. 손으로 고치지 않는다.\n");
	printf(" *\n");
	printf(" *   $ ./tools/l3_golden.sh\n");
	printf(" *\n");
	printf(" * 출처: ggml 의 C 구현 dequantize_row_q4_0 / dequantize_row_q4_K 를\n");
	printf(" * 호스트에서 **그대로 호출한** 결과다. 파이썬 역양자화기를 쓰지 않았다 —\n");
	printf(" * L3 의 허용 오차가 0(비트 일치)이고 연산 결합 순서가 다르면 반올림이\n");
	printf(" * 달라지기 때문이다.\n");
	printf(" *\n");
	printf(" * f32 를 비트 패턴으로 적는다. 10진수로는 비트 일치를 확인할 수 없다.\n");
	printf(" */\n");
	printf("#ifndef L3_GOLDEN_H\n#define L3_GOLDEN_H\n\n#include <stdint.h>\n\n");
	printf("#define L3_Q4_0_BLOCKS %d\n", q40_blocks);
	printf("#define L3_Q4_0_N      %d\n", q40_n);
	printf("#define L3_Q4_K_BLOCKS %d\n", q4k_blocks);
	printf("#define L3_Q4_K_N      %d\n", q4k_n);
	printf("#define L3_QK4_0       %d\n", QK4_0);
	printf("#define L3_QK_K        %d\n\n", QK_K);
	printf("#define L3_Q4_0_FILE_OFF %ldu  /* 데이터 시작 + 텐서 오프셋 */\n", data_off + q40_off);
	printf("#define L3_Q4_K_FILE_OFF %ldu\n\n", data_off + q4k_off);

	printf("static const uint32_t l3_q4_0_golden_bits[L3_Q4_0_N] = {\n");
	for (int i = 0; i < q40_n; i++) {
		uint32_t b; memcpy(&b, &y40[i], 4);
		printf("%s0x%08xu,", (i % 8 == 0) ? "\t" : " ", b);
		if (i % 8 == 7) printf("\n");
	}
	printf("%s};\n\n", (q40_n % 8) ? "\n" : "");

	printf("static const uint32_t l3_q4_k_golden_bits[L3_Q4_K_N] = {\n");
	for (int i = 0; i < q4k_n; i++) {
		uint32_t b; memcpy(&b, &y4k[i], 4);
		printf("%s0x%08xu,", (i % 8 == 0) ? "\t" : " ", b);
		if (i % 8 == 7) printf("\n");
	}
	printf("%s};\n\n", (q4k_n % 8) ? "\n" : "");
	printf("#endif /* L3_GOLDEN_H */\n");

	return 0;
}
