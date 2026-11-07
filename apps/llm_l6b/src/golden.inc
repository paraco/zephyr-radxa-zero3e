/* tools/l6b_golden.sh 가 만든다 — 손으로 고치지 않는다. */
/* 출처: upstream/llama.cpp/build-golden (보드와 같은 기능 집합). */
/* 토큰 5834 · 노드 전체 1017개 (서로 다른 이름 933개). */
/* ⚠ 이름이 유일하지 않다 — nth(같은 이름 중 몇 번째)가 키의 일부다. */

#define L6B_TOKEN 5834
#define L6B_N_NODES 1017

static const struct { const char *name; int nth; const char *op; float sum; } l6b_golden[] = {
	{ "model.input_embed", 0, "GET_ROWS", -0.762861f },
	{ "norm-0", 0, "RMS_NORM", -35.549904f },
	{ "attn_norm-0", 0, "MUL", -43.128544f },
	{ "norm-0", 1, "RMS_NORM", 184.643585f },
	{ "norm-0", 2, "RMS_NORM", -8.183719f },
	{ "l_out-0", 0, "ADD", -0.245448f },
	{ "l_out-1", 0, "ADD", 0.151270f },
	{ "l_out-2", 0, "ADD", 1.335250f },
	{ "l_out-3", 0, "ADD", 0.605366f },
	{ "l_out-4", 0, "ADD", 2.180613f },
	{ "l_out-5", 0, "ADD", 0.968158f },
	{ "l_out-6", 0, "ADD", -1.372722f },
	{ "l_out-7", 0, "ADD", -0.577628f },
	{ "l_out-8", 0, "ADD", -0.518838f },
	{ "l_out-9", 0, "ADD", -1.181742f },
	{ "l_out-10", 0, "ADD", -1.625014f },
	{ "l_out-11", 0, "ADD", 1.435425f },
	{ "l_out-12", 0, "ADD", 0.845511f },
	{ "l_out-13", 0, "ADD", 1.668248f },
	{ "l_out-14", 0, "ADD", 6.266537f },
	{ "l_out-15", 0, "ADD", 1.080498f },
	{ "l_out-16", 0, "ADD", -1.398125f },
	{ "l_out-17", 0, "ADD", -4.123839f },
	{ "l_out-18", 0, "ADD", -6.731330f },
	{ "l_out-19", 0, "ADD", -3.241256f },
	{ "l_out-20", 0, "ADD", -2.635335f },
	{ "l_out-21", 0, "ADD", -1.857480f },
	{ "l_out-22", 0, "ADD", -0.499140f },
	{ "l_out-23", 0, "ADD", 10.362761f },
	{ "norm", 0, "RMS_NORM", 34.496681f },
	{ "result_norm", 0, "MUL", 109.291367f },
	{ "result_output", 0, "MUL_MAT", -565930.500000f },
};
