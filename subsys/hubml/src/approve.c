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

/* ── 정규 JSON 검사기: 공백 없음 · 객체 키 사전순(엄격) · 숫자는 정수 ─────────────── */
struct jp {
	const char *s, *e;
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
		if (pk && keycmp(pk, pkn, k, kn) >= 0) { return -2; }	/* 사전순이 아니거나 겹침 */
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
