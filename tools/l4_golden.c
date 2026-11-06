/*
 * L4 골든값 생성기 — 호스트에서 **같은 l4_graph.c** 를 돌린다.
 * 그래프를 양쪽에 따로 쓰면 전사 오류를 "커널이 다르다" 로 오진한다.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "l4_graph.h"

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

int main(void)
{
	struct l4_results r;
	int rc = l4_run(&r);

	if (rc != 0) {
		fprintf(stderr, "l4_run 실패: %d\n", rc);
		return 1;
	}
	fprintf(stderr, "호스트 커널 플래그: dotprod=%d i8mm=%d\n", r.dotprod, r.i8mm);

	printf("/*\n * l4_golden.h — 생성 파일이다. 손으로 고치지 않는다.\n *\n");
	printf(" *   $ ./tools/l4_golden.sh\n *\n");
	printf(" * 출처: 호스트에서 apps/llm_l4/src/l4_graph.c 를 **그대로** 돌린 결과다.\n");
	printf(" * 같은 파일을 Zephyr 에서도 컴파일하므로 차이는 플랫폼뿐이다.\n");
	printf(" *\n * 호스트 커널 플래그: dotprod=%d i8mm=%d\n", r.dotprod, r.i8mm);
	printf(" */\n#ifndef L4_GOLDEN_H\n#define L4_GOLDEN_H\n\n#include <stdint.h>\n\n");
	printf("#define L4_HOST_DOTPROD %d\n#define L4_HOST_I8MM    %d\n\n", r.dotprod, r.i8mm);
	dump("l4_g_mm_f32", r.mm_f32, L4_MM_N);
	dump("l4_g_mm_q4_0", r.mm_q4_0, L4_MM_N);
	dump("l4_g_rms", r.rms, L4_RMS_NE);
	dump("l4_g_silu", r.silu, L4_SILU_NE);
	dump("l4_g_rope", r.rope, L4_ROPE_NE);
	printf("#endif /* L4_GOLDEN_H */\n");
	return 0;
}
