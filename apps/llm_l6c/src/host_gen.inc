/* tools/l6c_host_gen 이 만든다 — 손으로 고치지 않는다. */
/* 첫 토큰 5834 · 12개 생성 · n_ctx=256 n_batch=1 n_ubatch=1 */
/* ⚠ 기준은 **토큰 id 열**이다. 로짓은 1 ulp 교란에 4.3% 흔들린다
 *   (docs/l6b-sensitivity.md) — 값을 기준으로 삼으면 의미 없는
 *   실패가 난다. 로짓은 참고로만 적는다. */

#define L6C_FIRST_TOKEN 5834
#define L6C_N_GEN 12

static const struct { int id; float logit; const char *piece; } l6c_host_gen[] = {
	{     11,    13.073855f, "," },
	{    353,    14.484054f, " I" },
	{   1044,    17.968019f, " am" },
	{    264,    14.472815f, " a" },
	{   5286,    13.646438f, " student" },
	{    314,    16.856750f, " of" },
	{    279,    13.702618f, " the" },
	{  11759,    13.690474f, " university" },
	{    314,    16.794298f, " of" },
	{    279,    11.174417f, " the" },
	{   9897,    12.681550f, " north" },
	{    314,    14.721532f, " of" },
};

/* 생성된 글: ", I am a student of the university of the north of" */
