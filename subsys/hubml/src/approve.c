/*
 * ml approve — 사람 승인(서명)으로 원장을 움직인다. 정본: MoruLab_MLOps design/MLCC_LEDGER_SPEC.md v0.2 §4.
 *
 *   ml approve b64=<승인 본문 JSON> sig=<ed25519 서명 b64> key=<key id>
 *   본문 = {"v":1,"hub":…,"kind":"alias"|"normal_gate","head":…,"exp":…,…사건 칸…} — 키 사전순 · 공백 없음 · 정수만
 *   서명 = ed25519( sha256(본문 바이트) ),  공개키 <root>/keys/<key id>.pub (32 B, key id [a-z0-9-]{1,32})
 *   검사 순서: 본문 모양(bad_json) → hub · kind(bad_arg) → time_set(denied) → exp(expired · 10 분 넘으면 bad_arg) →
 *              키(unknown_key) → 서명(sig) → 칸(bad_arg · not_found) → 원장 잠금 안에서 head(stale) → 덧붙이기
 *   원장 줄 = 공통 칸 + 본문 칸(v · hub · kind · head 제외) + approval_head + sig 객체. **그 밖의 칸은 넣지 않는다**
 *   (읽는 쪽이 v·seq·prev·ts·actor·sig·approval_head 를 빼고 본문을 다시 만들어 검증한다).
 *   alias champion 이면 기록 뒤 Edge champion 슬롯으로 배포를 시작한다(결과는 deploy 사건).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/base64.h>
#include <hubml/hubml.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/deploy.h>
#include <hubml/sha256.h>
#include <hubml/ed25519.h>
#include <hubml/approve.h>
#include <hubml/gate.h>

/* ── 정규 JSON 검사기: 공백 없음 · 객체 키 사전순(엄격) · 숫자는 정수 ─────────────── */
struct jp {
	const char *s, *e;
	bool loose;	/* true = 키 사전순 검사를 건너뛴다 (원장 줄·앵커 줄은 사전순이 아니다 — gate 읽기용) */
};
static int j_val(struct jp *p, int depth);
static int j_str(struct jp *p, const char **b, size_t *n)
{
	if (p->s >= p->e || *p->s != '"') { return -1; }
	const char *st = ++p->s;
	while (p->s < p->e && *p->s != '"') {
		if ((unsigned char)*p->s < 0x20) { return -1; }
		if (*p->s == '\\') { p->s++; }
		p->s++;
	}
	if (p->s >= p->e) { return -1; }
	if (b) { *b = st; *n = (size_t)(p->s - st); }
	p->s++;
	return 0;
}
static int keycmp(const char *a, size_t an, const char *b, size_t bn)
{
	const int c = memcmp(a, b, an < bn ? an : bn);
	return c ? c : (an < bn ? -1 : (an > bn ? 1 : 0));
}
static int j_obj(struct jp *p, int depth, void (*member)(const char *k, size_t kn, const char *v, size_t vn, void *u),
		 void *u)
{
	if (depth > 8 || *p->s != '{') { return -1; }
	p->s++;
	if (p->s < p->e && *p->s == '}') { p->s++; return 0; }
	const char *pk = NULL;
	size_t pkn = 0;
	for (;;) {
		const char *k;
		size_t kn;
		if (j_str(p, &k, &kn) || p->s >= p->e || *p->s != ':') { return -1; }
		if (!p->loose && pk && keycmp(pk, pkn, k, kn) >= 0) { return -2; }	/* 사전순이 아니거나 겹침 */
		pk = k;
		pkn = kn;
		p->s++;
		const char *vb = p->s;
		const int r = j_val(p, depth + 1);
		if (r) { return r; }
		if (member) { member(k, kn, vb, (size_t)(p->s - vb), u); }
		if (p->s < p->e && *p->s == ',') { p->s++; continue; }
		if (p->s < p->e && *p->s == '}') { p->s++; return 0; }
		return -1;
	}
}
static int j_val(struct jp *p, int depth)
{
	if (p->s >= p->e) { return -1; }
	const char c = *p->s;
	if (c == '"') { return j_str(p, NULL, NULL); }
	if (c == '{') { return j_obj(p, depth, NULL, NULL); }
	if (c == '[') {
		p->s++;
		if (p->s < p->e && *p->s == ']') { p->s++; return 0; }
		for (;;) {
			const int r = j_val(p, depth + 1);
			if (r) { return r; }
			if (p->s < p->e && *p->s == ',') { p->s++; continue; }
			if (p->s < p->e && *p->s == ']') { p->s++; return 0; }
			return -1;
		}
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		if (c == '-') { p->s++; }
		if (p->s >= p->e || *p->s < '0' || *p->s > '9') { return -1; }
		if (*p->s == '0' && p->s + 1 < p->e && p->s[1] >= '0' && p->s[1] <= '9') { return -1; }	/* 앞 0 */
		while (p->s < p->e && *p->s >= '0' && *p->s <= '9') { p->s++; }
		if (p->s < p->e && (*p->s == '.' || *p->s == 'e' || *p->s == 'E')) { return -3; }	/* 정수만 */
		return 0;
	}
	static const char *const lit[] = { "true", "false", "null" };
	for (int i = 0; i < 3; i++) {
		const size_t n = strlen(lit[i]);
		if ((size_t)(p->e - p->s) >= n && !memcmp(p->s, lit[i], n)) { p->s += n; return 0; }
	}
	return -1;	/* 공백도 여기서 걸린다 */
}

/* ── 본문 칸 모으기 ───────────────────────────────────────────────────── */
#define MAXM 16
struct body {
	int n;
	struct { const char *k, *v; size_t kn, vn; } m[MAXM];
};
static void put_member(const char *k, size_t kn, const char *v, size_t vn, void *u)
{
	struct body *b = u;
	if (b->n < MAXM) {
		b->m[b->n].k = k; b->m[b->n].kn = kn; b->m[b->n].v = v; b->m[b->n].vn = vn;
	}
	b->n++;
}
static const char *get(const struct body *b, const char *key, size_t *vn)
{
	for (int i = 0; i < b->n && i < MAXM; i++) {
		if (b->m[i].kn == strlen(key) && !memcmp(b->m[i].k, key, b->m[i].kn)) {
			*vn = b->m[i].vn;
			return b->m[i].v;
		}
	}
	return NULL;
}
/* 문자열 값을 따옴표 없이 복사 (이스케이프 없는 짧은 값만) */
static bool get_str(const struct body *b, const char *key, char *out, size_t cap)
{
	size_t n;
	const char *v = get(b, key, &n);
	if (!v || n < 2 || v[0] != '"' || v[n - 1] != '"' || n - 2 >= cap || memchr(v, '\\', n)) {
		return false;
	}
	memcpy(out, v + 1, n - 2);
	out[n - 2] = 0;
	return true;
}

static int64_t parse_utc(const char *s)	/* YYYY-MM-DDTHH:MM:SSZ → epoch ms, 오류 −1 */
{
	int Y, M, D, h, m, sec;
	if (strlen(s) != 20 || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &m, &sec) != 6) {
		return -1;
	}
	/* 일수 (1970-01-01 기준, 그레고리력) */
	const int y = Y - (M <= 2);
	const int era = (y >= 0 ? y : y - 399) / 400;
	const int yoe = y - era * 400;
	const int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	const int64_t days = (int64_t)era * 146097 + doe - 719468;
	return ((days * 86400) + h * 3600 + m * 60 + sec) * 1000;
}

static bool key_ok(const char *k)
{
	const size_t n = strlen(k);
	if (n < 1 || n > 32) { return false; }
	for (size_t i = 0; i < n; i++) {
		if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '-')) { return false; }
	}
	return true;
}

int hubml_approve(const char *b64, const char *sig64, const char *key, struct hubml_approve_result *res)
{
	static char body[2048];
	static char fl[HUBML_LEDGER_LINE_MAX];
	memset(res, 0, sizeof(*res));
	size_t bn = 0;
	if (!b64 || !sig64 || !key || base64_decode((uint8_t *)body, sizeof body - 1, &bn, (const uint8_t *)b64, strlen(b64))) {
		res->code = "bad_arg";
		return -EINVAL;
	}
	body[bn] = 0;
	/* 1 모양 */
	struct body b = { 0 };
	struct jp p = { body, body + bn };
	if (j_obj(&p, 0, put_member, &b) || p.s != p.e || b.n > MAXM) {
		res->code = "bad_json";
		return -EBADMSG;
	}
	char hub[48], kind[16], head[80], exp[24], v[4];
	size_t vn;
	const char *vv = get(&b, "v", &vn);
	if (!vv || vn != 1 || vv[0] != '1' || !get_str(&b, "hub", hub, sizeof hub) || !get_str(&b, "kind", kind, sizeof kind) ||
	    !get_str(&b, "head", head, sizeof head) || !get_str(&b, "exp", exp, sizeof exp)) {
		res->code = "bad_arg";
		return -EINVAL;
	}
	(void)v;
	strncpy(res->kind, kind, sizeof res->kind - 1);
	/* 2 대상 */
	if (strcmp(hub, hubml_hub_id()) || (strcmp(kind, "alias") && strcmp(kind, "normal_gate"))) {
		res->code = "bad_arg";
		return -EINVAL;
	}
	/* 3 시각 */
	if (!hubml_time_is_set()) {
		res->code = "denied";
		return -EACCES;
	}
	const int64_t now = hubml_time_now(), e = parse_utc(exp);
	if (e < 0) {
		res->code = "bad_arg";
		return -EINVAL;
	}
	if (now > e) {
		res->code = "expired";
		return -ETIME;
	}
	if (e > now + 10 * 60 * 1000 + 2000) {
		res->code = "bad_arg";	/* exp ≤ 서명 시각 + 10 분 */
		return -EINVAL;
	}
	/* 4 키 */
	char kp[96];
	uint8_t pk[32];
	if (!key_ok(key)) {
		res->code = "unknown_key";
		return -ENOENT;
	}
	snprintf(kp, sizeof kp, "%s/keys/%s.pub", hubml_root(), key);
	struct fs_file_t f;
	fs_file_t_init(&f);
	hubml_fs_lock();
	int rc = fs_open(&f, kp, FS_O_READ);
	ssize_t kr = -1;
	if (!rc) {
		kr = fs_read(&f, pk, sizeof pk);
		fs_close(&f);
	}
	hubml_fs_unlock();
	if (kr != 32) {
		res->code = "unknown_key";
		return -ENOENT;
	}
	/* 5 서명: ed25519(sha256(본문)) */
	uint8_t sig[64], dg[32];
	size_t sn = 0;
	if (base64_decode(sig, sizeof sig, &sn, (const uint8_t *)sig64, strlen(sig64)) || sn != 64) {
		res->code = "sig";
		return -EBADMSG;
	}
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, body, bn);
	sha256_final(&s, dg);
	if (hubml_ed25519_verify(sig, dg, 32, pk)) {
		res->code = "sig";
		return -EPERM;
	}
	/* 6 사건 칸 */
	uint64_t uid = 0;
	char name[16] = "", model[32] = "";
	if (!strcmp(kind, "alias")) {
		char u[20], reason[16];
		if (!get_str(&b, "uid", u, sizeof u) || strlen(u) != 16 || !get_str(&b, "name", name, sizeof name) ||
		    !get_str(&b, "model_id", model, sizeof model) || !get_str(&b, "reason", reason, sizeof reason) ||
		    (strcmp(name, "v0") && strcmp(name, "champion")) || (strcmp(reason, "promote") && strcmp(reason, "rollback"))) {
			res->code = "bad_arg";
			return -EINVAL;
		}
		uid = strtoull(u, NULL, 16);
		static uint8_t probe[16];
		if (hubml_model_load(uid, model, probe, sizeof probe) <= 0) {
			res->code = "not_found";
			return -ENOENT;
		}
	} else {
		char ds[96], dec[12];
		size_t n;
		if (!get_str(&b, "ds_id", ds, sizeof ds) || !get_str(&b, "decision", dec, sizeof dec) ||
		    (strcmp(dec, "accept") && strcmp(dec, "reject")) || !get(&b, "include", &n) || !get(&b, "exclude", &n) ||
		    !get(&b, "d0", &n)) {
			res->code = "bad_arg";
			return -EINVAL;
		}
	}
	/* 7 원장 줄: 본문 칸(v·hub·kind·head 제외) 그대로 + approval_head, 끝에 sig */
	size_t o = 0;
	for (int i = 0; i < b.n; i++) {
		const char *k = b.m[i].k;
		const size_t kn = b.m[i].kn;
		if ((kn == 1 && k[0] == 'v') || (kn == 3 && !memcmp(k, "hub", 3)) || (kn == 4 && !memcmp(k, "kind", 4)) ||
		    (kn == 4 && !memcmp(k, "head", 4))) {
			continue;
		}
		const int w = snprintf(fl + o, sizeof fl - o, "%s\"%.*s\":%.*s", o ? "," : "", (int)kn, k, (int)b.m[i].vn, b.m[i].v);
		if (w < 0 || (size_t)w >= sizeof fl - o) {
			res->code = "bad_arg";
			return -E2BIG;
		}
		o += (size_t)w;
	}
	snprintf(fl + o, sizeof fl - o, ",\"approval_head\":\"%s\"", head);
	char sigobj[160], actor[48];
	snprintf(sigobj, sizeof sigobj, "{\"alg\":\"ed25519\",\"key\":\"%s\",\"over\":\"sha256\",\"b64\":\"%s\"}", key, sig64);
	snprintf(actor, sizeof actor, "studio:%s", key);
	rc = hubml_ledger_append_if_head(head, kind, actor, fl, sigobj, &res->seq);
	if (rc == -ESTALE) {
		res->code = "stale";
		return rc;
	}
	if (rc) {
		res->code = "ledger";
		return rc;
	}
	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	strcpy(res->head, h.hash);
	res->ok = true;
	/* 8 champion 이면 Edge 로 */
	res->deploy = 1;
	if (!strcmp(kind, "alias") && !strcmp(name, "champion")) {
		res->deploy = hubml_deploy_start(uid, model, 0);
	}
	return 0;
}

/* ══ 정상 관문 읽기 (hubml/gate.h) — 위 스캐너(j_str · j_obj · j_val)를 loose 모드로 재사용한다 ═══════════════════ */
struct gmem {
	const char *kind, *dec, *ds, *inc, *exc;
	size_t kindn, decn, dsn, incn, excn;
	char sigc;	/* sig 값의 첫 글자 ('{' = 객체) */
};
static void gmem_cb(const char *k, size_t kn, const char *v, size_t vn, void *u)
{
	struct gmem *m = u;
#define K(name) (kn == sizeof(name) - 1 && !memcmp(k, name, kn))
	if (K("kind")) { m->kind = v; m->kindn = vn; }
	else if (K("decision")) { m->dec = v; m->decn = vn; }
	else if (K("ds_id")) { m->ds = v; m->dsn = vn; }
	else if (K("include")) { m->inc = v; m->incn = vn; }
	else if (K("exclude")) { m->exc = v; m->excn = vn; }
	else if (K("sig")) { m->sigc = vn ? v[0] : 0; }
#undef K
}
/* 따옴표 문자열 값 == lit ? */
static bool qeq(const char *v, size_t vn, const char *lit)
{
	const size_t n = strlen(lit);
	return v && vn == n + 2 && v[0] == '"' && v[vn - 1] == '"' && !memcmp(v + 1, lit, n);
}

/* 머리: kind · decision · sig · uid 까지만 본다. 0 = 통과, 아니면 사유 */
static const char *gate_head(const char *line, size_t n, const char *uid16, struct gmem *m)
{
	memset(m, 0, sizeof *m);
	struct jp p = { line, line + n, true };
	if (j_obj(&p, 0, gmem_cb, m) || p.s != p.e) { return "gate_json"; }
	if (!qeq(m->kind, m->kindn, "normal_gate")) { return "gate_kind"; }
	if (!qeq(m->dec, m->decn, "accept")) { return "gate_decision"; }
	if (m->sigc != '{') { return "gate_nosig"; }
	/* ds_id = "ds-<16hex>-…" */
	if (!m->ds || m->dsn < 2 + 3 + 16 + 1 || memcmp(m->ds, "\"ds-", 4) || m->ds[4 + 16] != '-' || memchr(m->ds, '\\', m->dsn)) {
		return "gate_uid";
	}
	for (int i = 0; i < 16; i++) {
		char a = m->ds[4 + i], b = uid16[i];
		a = (a >= 'A' && a <= 'F') ? (char)(a + 32) : a;
		b = (b >= 'A' && b <= 'F') ? (char)(b + 32) : b;
		if (!b || a != b) { return "gate_uid"; }
	}
	return NULL;
}

/* ── 날짜 ── */
static int32_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const int yoe = y - era * 400;
	const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}
static void civil_from_days(int32_t z, int *y, int *m, int *d)
{
	z += 719468;
	const int era = (z >= 0 ? z : z - 146096) / 146097;
	const int doe = z - era * 146097;
	const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp + (mp < 10 ? 3 : -9);
	*y = yoe + era * 400 + (*m <= 2);
}
static void day_str(int32_t dn, char out[9])
{
	int y, m, d;
	civil_from_days(dn, &y, &m, &d);
	snprintf(out, 9, "%04d%02d%02d", y, m, d);
}
/* yyyymmdd 8 자리 숫자 + 실제 있는 날짜 → 일 수. 오류 = INT32_MIN */
static int32_t day_num(const char *s, size_t n)
{
	if (n != 8) { return INT32_MIN; }
	int v[8];
	for (int i = 0; i < 8; i++) {
		if (s[i] < '0' || s[i] > '9') { return INT32_MIN; }
		v[i] = s[i] - '0';
	}
	const int y = v[0] * 1000 + v[1] * 100 + v[2] * 10 + v[3], m = v[4] * 10 + v[5], d = v[6] * 10 + v[7];
	if (m < 1 || m > 12 || d < 1 || d > 31) { return INT32_MIN; }
	const int32_t z = days_from_civil(y, m, d);
	int yy, mm, dd;
	civil_from_days(z, &yy, &mm, &dd);
	return (yy == y && mm == m && dd == d) ? z : INT32_MIN;
}
/* "HH:MM" → 분. 끝이면 "24:00" 허용. 오류 -1 */
static int hhmm(const char *s, size_t n, bool is_end)
{
	if (n != 5 || s[2] != ':') { return -1; }
	for (int i = 0; i < 5; i++) {
		if (i != 2 && (s[i] < '0' || s[i] > '9')) { return -1; }
	}
	const int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
	if (m > 59) { return -1; }
	if (h == 24) { return (is_end && m == 0) ? 1440 : -1; }
	return h > 23 ? -1 : h * 60 + m;
}

/* [[날짜,시작,끝(,사유)],…] → iv. 0 = 정상, 아니면 사유 */
static const char *parse_ivs(const char *v, size_t vn, bool is_exc, struct hubml_gate_iv *out, int *cnt)
{
	*cnt = 0;
	if (!v) { return is_exc ? NULL : "gate_include_empty"; }
	struct jp p = { v, v + vn, true };
	if (p.s >= p.e || *p.s != '[') { return "gate_shape"; }
	p.s++;
	if (p.s < p.e && *p.s == ']') { return is_exc ? NULL : "gate_include_empty"; }
	for (;;) {
		if (p.s >= p.e || *p.s != '[') { return "gate_shape"; }
		p.s++;
		const char *sv[4];
		size_t sn[4];
		int ns = 0;
		for (;;) {
			if (ns >= 4 || j_str(&p, &sv[ns], &sn[ns])) { return "gate_shape"; }
			ns++;
			if (p.s < p.e && *p.s == ',') { p.s++; continue; }
			if (p.s < p.e && *p.s == ']') { p.s++; break; }
			return "gate_shape";
		}
		if (ns != (is_exc ? 4 : 3)) { return is_exc && ns == 3 ? "gate_reason" : "gate_shape"; }
		if (is_exc && sn[3] == 0) { return "gate_reason"; }
		if (*cnt >= HUBML_GATE_MAX_IV) { return "gate_too_many"; }
		struct hubml_gate_iv *iv = &out[*cnt];
		iv->dn = day_num(sv[0], sn[0]);
		if (iv->dn == INT32_MIN) { return "gate_date"; }
		memcpy(iv->day, sv[0], 8);
		iv->day[8] = 0;
		const int a = hhmm(sv[1], sn[1], false), b = hhmm(sv[2], sn[2], true);
		if (a < 0 || b < 0) { return "gate_time"; }
		if (a >= b) { return "gate_range"; }
		iv->a = (uint16_t)a;
		iv->b = (uint16_t)b;
		(*cnt)++;
		if (p.s < p.e && *p.s == ',') { p.s++; continue; }
		if (p.s < p.e && *p.s == ']') { p.s++; break; }
		return "gate_shape";
	}
	return p.s == p.e ? NULL : "gate_shape";
}

static int day_cmp9(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int uniq9(char (*d)[9], int n)
{
	qsort(d, (size_t)n, 9, day_cmp9);
	int m = 0;
	for (int i = 0; i < n; i++) {
		if (!m || strcmp(d[m - 1], d[i])) { memcpy(d[m++], d[i], 9); }
	}
	return m;
}

int hubml_gate_parse(const char *line, size_t n, const char *uid16, struct hubml_gate *g, const char **code,
		     const char **why)
{
	struct gmem m;
	const char *w = gate_head(line, n, uid16, &m);
	*code = "bad_arg";
	if (!w) {
		memset(g, 0, sizeof *g);
		w = parse_ivs(m.inc, m.incn, false, g->inc, &g->ninc);
		if (!w) { w = parse_ivs(m.exc, m.excn, true, g->exc, &g->nexc); }
		if (!w && m.dsn - 2 < sizeof g->ds_id) {
			memcpy(g->ds_id, m.ds + 1, m.dsn - 2);
		} else if (!w) {
			w = "gate_uid";
		}
	}
	if (w) {
		*why = w;
		return -EINVAL;
	}
	/* include 날짜(정렬·중복 없음) · 읽을 파일 날짜 = 각 D 의 D · D+1 · D+2 의 합집합 */
	for (int i = 0; i < g->ninc; i++) { memcpy(g->idays[i], g->inc[i].day, 9); }
	g->nidays = uniq9(g->idays, g->ninc);
	int nf = 0;
	for (int i = 0; i < g->nidays; i++) {
		const int32_t dn = day_num(g->idays[i], 8);
		for (int k = 0; k < 3; k++) { day_str(dn + k, g->fdays[nf++]); }
	}
	g->nfdays = uniq9(g->fdays, nf);
	return 0;
}

struct gl_ctx {
	uint32_t want;
	const char *uid;
	struct hubml_gate *g;
	const char *code, *why;
	bool seen;
};
static void gl_cb(uint32_t seq, const char *line, size_t n, void *u)
{
	struct gl_ctx *c = u;
	if (c->seen || seq != c->want) { return; }
	c->seen = true;
	hubml_gate_parse(line, n, c->uid, c->g, &c->code, &c->why);
	if (!c->why) { c->g->seq = seq; }
}
int hubml_gate_load(uint32_t seq, const char *uid16, struct hubml_gate *g, const char **code, const char **why)
{
	struct gl_ctx c = { .want = seq, .uid = uid16, .g = g };
	if (seq > 0) { hubml_ledger_read(seq, 1, gl_cb, &c); }
	if (!c.seen) {
		*code = "not_found";
		*why = "gate_not_found";
		return -ENOENT;
	}
	if (c.why) {
		*code = c.code;
		*why = c.why;
		return -EINVAL;
	}
	return 0;
}

struct gla_ctx {
	const char *uid;
	uint32_t best;
};
static void gla_cb(uint32_t seq, const char *line, size_t n, void *u)
{
	struct gla_ctx *c = u;
	bool hit = false;	/* 싼 걸러내기: "normal_gate" 글자가 있는 줄만 */
	for (size_t i = 0; !hit && i + 13 <= n; i++) { hit = !memcmp(line + i, "\"normal_gate\"", 13); }
	if (!hit) { return; }
	struct gmem m;
	if (!gate_head(line, n, c->uid, &m)) { c->best = seq; }
}
int hubml_gate_latest(const char *uid16, uint32_t *seq)
{
	struct gla_ctx c = { .uid = uid16 };
	hubml_ledger_read(1, 0x7fffffff, gla_cb, &c);
	if (!c.best) { return -ENOENT; }
	*seq = c.best;
	return 0;
}

int hubml_gate_check(const struct hubml_gate *g, int64_t utc_ms)
{
	int64_t dn = utc_ms / 86400000LL, ms = utc_ms % 86400000LL;
	if (ms < 0) { ms += 86400000LL; dn--; }
	bool in = false;
	for (int i = 0; i < g->ninc && !in; i++) {
		in = g->inc[i].dn == dn && ms >= g->inc[i].a * 60000LL && ms < g->inc[i].b * 60000LL;
	}
	if (!in) { return 1; }
	for (int i = 0; i < g->nexc; i++) {
		if (g->exc[i].dn == dn && ms >= g->exc[i].a * 60000LL && ms < g->exc[i].b * 60000LL) { return 2; }
	}
	return 0;
}

/* ── anchor.jsonl 한 줄: {"seq":n,"stream":1,"boot_id":B,"edge_ms":E,"hub_ms":H,"time_set":0|1} ── */
struct amem {
	int have;
	int64_t boot, edge, hub, ts;
};
static void amem_cb(const char *k, size_t kn, const char *v, size_t vn, void *u)
{
	struct amem *m = u;
	char t[24];
	if (vn == 0 || vn >= sizeof t || (v[0] != '-' && (v[0] < '0' || v[0] > '9'))) { return; }
	memcpy(t, v, vn);
	t[vn] = 0;
	const int64_t x = strtoll(t, NULL, 10);
#define K(name) (kn == sizeof(name) - 1 && !memcmp(k, name, kn))
	if (K("boot_id")) { m->boot = x; m->have |= 1; }
	else if (K("edge_ms")) { m->edge = x; m->have |= 2; }
	else if (K("hub_ms")) { m->hub = x; m->have |= 4; }
	else if (K("time_set")) { m->ts = x; m->have |= 8; }
#undef K
}
int hubml_anchor_parse(const char *line, size_t n, struct hubml_anchor *a, bool *time_set)
{
	struct amem m = { 0 };
	struct jp p = { line, line + n, true };
	if (j_obj(&p, 0, amem_cb, &m) || m.have != 15 || m.boot < 0 || m.boot > 0xffff) { return -EBADMSG; }
	a->boot = (uint16_t)m.boot;
	a->edge_ms = m.edge;
	a->hub_ms = m.hub;
	*time_set = m.ts != 0;
	return 0;
}
