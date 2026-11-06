/*
 * L5a 골든값 생성기 — 호스트에서 **같은 l5_graph.c** 를 돌린다.
 * 규약과 빌드 방법은 tools/host_golden.sh 에 있다.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "l5_graph.h"

static void dump(const char *name, const float *v, int n)
{
	printf("static const uint32_t %s[%d] = {\n", name, n);
	for (int i = 0; i < n; i++) {
		uint32_t b; memcpy(&b, &v[i], 4);
		printf("%s0x%08xu,", (i % 8 == 0) ? "\t" : " ", b);
		if (i % 8 == 7) { printf("\n"); }
	}
	printf("%s};\n\n", (n % 8) ? "\n" : "");
}

static void dump_ne(const char *name, const int64_t *ne)
{
	printf("static const int64_t %s[4] = { %lld, %lld, %lld, %lld };\n",
	       name, (long long)ne[0], (long long)ne[1], (long long)ne[2], (long long)ne[3]);
}

int main(void)
{
	struct l5_results r;
	int rc = l5_run(&r);

	if (rc != 0) {
		fprintf(stderr, "l5_run 실패: %d\n", rc);
		return 1;
	}
	fprintf(stderr, "호스트 커널 플래그: dotprod=%d i8mm=%d\n", r.dotprod, r.i8mm);
	fprintf(stderr, "conv ne=[%lld,%lld,%lld,%lld] crc=0x%08x · state crc=0x%08x\n",
		(long long)r.ne_o[0], (long long)r.ne_o[1], (long long)r.ne_o[2],
		(long long)r.ne_o[3], r.conv_crc, r.state_crc);

	printf("/*\n * l5_golden.h — 생성 파일이다. 손으로 고치지 않는다.\n *\n");
	printf(" *   $ ./tools/host_golden.sh llm_l5a\n *\n");
	printf(" * 출처: 호스트에서 apps/llm_l5a/src/l5_graph.c 를 **그대로** 돌린 결과다.\n");
	printf(" * 같은 파일을 Zephyr 에서도 컴파일하므로 차이는 플랫폼뿐이다.\n");
	printf(" *\n * 호스트 커널 플래그: dotprod=%d i8mm=%d\n", r.dotprod, r.i8mm);
	printf(" */\n#ifndef L5_GOLDEN_H\n#define L5_GOLDEN_H\n\n#include <stdint.h>\n\n");
	printf("#define L5_HOST_DOTPROD %d\n#define L5_HOST_I8MM    %d\n", r.dotprod, r.i8mm);
	printf("#define L5_G_CONV_CRC   0x%08xu\n", r.conv_crc);
	printf("#define L5_G_STATE_CRC  0x%08xu\n\n", r.state_crc);
	dump_ne("l5_g_ne_q_perm", r.ne_q_perm);
	dump_ne("l5_g_ne_sk", r.ne_sk);
	dump_ne("l5_g_ne_kd", r.ne_kd);
	dump_ne("l5_g_ne_o", r.ne_o);
	printf("\n");
	dump("l5_g_conv", r.conv, L5_CONV_SAMP);
	dump("l5_g_beta", r.beta, L5_H);
	dump("l5_g_a_softplus", r.a_softplus, L5_H);
	dump("l5_g_gate", r.gate, L5_H);
	dump("l5_g_o", r.o, L5_O_N);
	dump("l5_g_state", r.state, L5_STATE_SAMP);
	dump("l5_g_normg", r.normg, L5_O_N);
	printf("#endif /* L5_GOLDEN_H */\n");
	return 0;
}
