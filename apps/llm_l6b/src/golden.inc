/* tools/l6b_golden.sh 가 만든다 — 손으로 고치지 않는다. */
/* 출처: upstream/llama.cpp/build-golden (보드와 같은 기능 집합). */
/* 토큰 5834 · 노드 전체 1017개 중 아래 지점을 대조한다. */

#define L6B_TOKEN 5834
#define L6B_N_NODES 1017

static const struct { const char *name; const char *op; float sum; } l6b_golden[] = {
	{ "model.input_embed", "GET_ROWS", -0.762861f },
	{ "norm-0", "RMS_NORM", -35.549904f },
	{ "attn_norm-0", "MUL", -43.128544f },
	{ "l_out-0", "ADD", -0.245448f },
	{ "l_out-3", "ADD", 0.605366f },
	{ "l_out-23", "ADD", 10.362761f },
	{ "norm", "RMS_NORM", 34.496681f },
	{ "result_norm", "MUL", 109.291367f },
	{ "result_output", "MUL_MAT", -565930.500000f },
};
