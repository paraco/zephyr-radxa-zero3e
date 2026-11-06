/* L5b 골든값 생성기 — 호스트에서 같은 l5b_graph.c 를 돌린다.
 * 규약과 빌드 방법은 tools/host_golden.sh 에 있다. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "l5b_graph.h"

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
	struct l5b_results r;
	int rc = l5b_run(&r);

	if (rc != 0) {
		fprintf(stderr, "l5b_run 실패: %d\n", rc);
		return 1;
	}
	fprintf(stderr, "호스트: dotprod=%d i8mm=%d · 부분RoPE 변화 lo=%d hi=%d\n",
		r.dotprod, r.i8mm, r.rope_changed_lo, r.rope_changed_hi);

	printf("/*\n * l5b_golden.h — 생성 파일이다. 손으로 고치지 않는다.\n *\n");
	printf(" *   $ ./tools/host_golden.sh llm_l5b\n *\n");
	printf(" * 출처: 호스트에서 apps/llm_l5b/src/l5b_graph.c 를 그대로 돌린 결과다.\n");
	printf(" * 호스트: dotprod=%d i8mm=%d\n */\n", r.dotprod, r.i8mm);
	printf("#ifndef L5B_GOLDEN_H\n#define L5B_GOLDEN_H\n\n#include <stdint.h>\n\n");
	printf("#define L5B_HOST_DOTPROD %d\n#define L5B_HOST_I8MM %d\n", r.dotprod, r.i8mm);
	printf("#define L5B_G_ROPE_LO %d\n#define L5B_G_ROPE_HI %d\n",
	       r.rope_changed_lo, r.rope_changed_hi);
	printf("#define L5B_G_QNORM_CRC 0x%08xu\n", r.q_norm_crc);
	printf("#define L5B_G_KNORM_CRC 0x%08xu\n", r.k_norm_crc);
	printf("#define L5B_G_QROPE_CRC 0x%08xu\n", r.q_rope_crc);
	printf("#define L5B_G_KROPE_CRC 0x%08xu\n", r.k_rope_crc);
	printf("#define L5B_G_ATT_CRC   0x%08xu\n", r.att_crc);
	printf("#define L5B_G_GATE_CRC  0x%08xu\n", r.gate_crc);
	printf("#define L5B_G_GATED_CRC 0x%08xu\n", r.gated_crc);
	printf("#define L5B_G_OUT_CRC   0x%08xu\n\n", r.out_crc);
	dump_ne("l5b_g_ne_q_view", r.ne_q_view);
	dump_ne("l5b_g_ne_gate", r.ne_gate);
	dump_ne("l5b_g_ne_kq", r.ne_kq);
	dump_ne("l5b_g_ne_att2d", r.ne_att2d);
	dump_ne("l5b_g_ne_out", r.ne_out);
	printf("\n");
	dump("l5b_g_q_norm", r.q_norm, L5B_Q_SAMP);
	dump("l5b_g_k_norm", r.k_norm, L5B_K_SAMP);
	dump("l5b_g_q_rope", r.q_rope, L5B_Q_SAMP);
	dump("l5b_g_k_rope", r.k_rope, L5B_K_SAMP);
	dump("l5b_g_kq", r.kq, L5B_KQ_N);
	dump("l5b_g_att", r.att, L5B_ATT_SAMP);
	dump("l5b_g_gate", r.gate, L5B_ATT_SAMP);
	dump("l5b_g_gated", r.gated, L5B_ATT_SAMP);
	dump("l5b_g_out", r.out, L5B_OUT_SAMP);
	printf("#endif /* L5B_GOLDEN_H */\n");
	return 0;
}
