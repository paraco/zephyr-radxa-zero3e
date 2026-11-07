/* tools/l6b_host_argmax 가 만든다 — 손으로 고치지 않는다. */
/* 입력 토큰 5834 · 어휘 248320 · 문맥 n_ctx=256 n_batch=1 n_ubatch=1 */
/* 로짓 합 -565928.505139 · |합| 658807.216948 */

#define L6B_TOP_N 5
static const struct { int id; float logit; const char *piece; } l6b_host_top[] = {
	{     11,    13.073855f, "," },
	{    198,    12.173112f, "?" },
	{     13,    12.024055f, "." },
	{   5834,    11.356230f, "hi" },
	{     72,    10.624166f, "i" },
};
