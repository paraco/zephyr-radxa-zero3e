/* hubml 원장 — include/hubml/ledger.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/sha256.h>
#include <hubml/xmb.h>

LOG_MODULE_DECLARE(hubml);

const char *const hubml_alias_name[HUBML_NALIAS] = { "v0", "champion", "challenger", "shadow" };

static K_MUTEX_DEFINE(lmtx);
static char root[48];
static char hub_id[40];
static struct hubml_ledger_head head;

#define MAX_AL 32
static struct {
	uint64_t uid;
	uint32_t nmodel;	/* 이 장비 model 사건 수 → 다음 model_id 번호 */
	char id[HUBML_NALIAS][HUBML_MODEL_ID_LEN + 1];
} al[MAX_AL];

static void hex(const uint8_t *d, int n, char *o)
{
	static const char h[] = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		o[2 * i] = h[d[i] >> 4];
		o[2 * i + 1] = h[d[i] & 15];
	}
	o[2 * n] = 0;
}

static void line_hash(const char *l, size_t n, char out[65])
{
	struct sha256 s;
	uint8_t d[32];
	sha256_init(&s);
	sha256_update(&s, l, n);
	sha256_final(&s, d);
	hex(d, 32, out);
}

/* "key":"value" 의 value (줄 안, 이스케이프 없음 가정) */
static bool jstr(const char *l, size_t n, const char *key, char *out, size_t cap)
{
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\":\"", key);
	const size_t pl = strlen(pat);
	for (size_t i = 0; i + pl < n; i++) {
		if (!memcmp(l + i, pat, pl)) {
			size_t j = 0;
			for (const char *p = l + i + pl; p < l + n && *p != '"' && j + 1 < cap; p++) {
				out[j++] = *p;
			}
			out[j] = 0;
			return true;
		}
	}
	return false;
}

static bool jnum(const char *l, size_t n, const char *key, uint32_t *v)
{
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\":", key);
	const size_t pl = strlen(pat);
	for (size_t i = 0; i + pl < n; i++) {
		if (!memcmp(l + i, pat, pl)) {
			*v = (uint32_t)strtoul(l + i + pl, NULL, 10);
			return true;
		}
	}
	return false;
}

static int al_slot(uint64_t uid, bool make)
{
	int free_i = -1;
	for (int i = 0; i < MAX_AL; i++) {
		if (al[i].uid == uid && uid) {
			return i;
		}
		if (!al[i].uid && free_i < 0) {
			free_i = i;
		}
	}
	if (make && free_i >= 0) {
		memset(&al[free_i], 0, sizeof(al[0]));
		al[free_i].uid = uid;
	}
	return make ? free_i : -1;
}

static void replay(const char *l, size_t n)
{
	char k[20], u[20], a[16], m[HUBML_MODEL_ID_LEN + 1];
	if (!jstr(l, n, "kind", k, sizeof k) || !jstr(l, n, "uid", u, sizeof u)) {
		return;
	}
	const int s = al_slot(strtoull(u, NULL, 16), true);
	if (s < 0) {
		return;
	}
	if (!strcmp(k, "model")) {
		al[s].nmodel++;
	} else if (!strcmp(k, "alias") && jstr(l, n, "name", a, sizeof a) && jstr(l, n, "model_id", m, sizeof m)) {
		for (int i = 0; i < HUBML_NALIAS; i++) {
			if (!strcmp(a, hubml_alias_name[i])) {
				strcpy(al[s].id[i], m);
			}
		}
	}
}

static void lpath(char *p, size_t n) { snprintf(p, n, "%s/ledger.jsonl", root); }

int hubml_ledger_init(const char *r, const char *hub)
{
	k_mutex_lock(&lmtx, K_FOREVER);
	strncpy(root, r, sizeof root - 1);
	strncpy(hub_id, hub ? hub : "-", sizeof hub_id - 1);
	memset(&head, 0, sizeof head);
	memset(al, 0, sizeof al);
	head.ok = true;
	head.next_seq = 1;
	memset(head.hash, '0', 64);
	char p[64];
	lpath(p, sizeof p);
	hubml_fs_lock();
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = 0;
	if (fs_open(&f, p, FS_O_READ) == 0) {
		static char l[HUBML_LEDGER_LINE_MAX + 2];
		size_t n = 0;
		off_t good_end = 0, pos = 0;
		char c;
		while (fs_read(&f, &c, 1) == 1) {	/* 원장은 작다 — 한 바이트씩도 충분 */
			pos++;
			if (c != '\n') {
				if (n < sizeof l - 1) {
					l[n++] = c;
				}
				continue;
			}
			/* 한 줄: prev 가 앞 줄 해시인가, seq 가 이어지는가 */
			char prev[65];
			uint32_t seq;
			if (head.ok && (!jstr(l, n, "prev", prev, sizeof prev) || strcmp(prev, head.hash) ||
					!jnum(l, n, "seq", &seq) || seq != head.next_seq)) {
				head.ok = false;
				head.bad_seq = head.next_seq;
				LOG_ERR("원장 사슬이 %u 번에서 깨졌다 — 덧붙이기를 막는다", head.bad_seq);
			}
			if (head.ok) {
				line_hash(l, n, head.hash);
				replay(l, n);
				head.next_seq++;
				good_end = pos;
			}
			n = 0;
		}
		fs_close(&f);
		if (head.ok && pos > good_end) {
			/* 끝 줄이 반쪽 — 전원이 쓰는 도중에 끊겼다. 잘라 낸다 */
			head.torn_bytes = (uint32_t)(pos - good_end);
			fs_file_t_init(&f);
			if (fs_open(&f, p, FS_O_WRITE) == 0) {
				rc = fs_truncate(&f, good_end);
				fs_close(&f);
			}
			LOG_WRN("원장 끝 반쪽 줄 %u B 를 잘라 냈다", head.torn_bytes);
		}
	}
	hubml_fs_unlock();
	k_mutex_unlock(&lmtx);
	if (!rc && head.torn_bytes) {
		char fl[64];
		snprintf(fl, sizeof fl, "\"dropped_bytes\":%u,\"last_good_seq\":%u", head.torn_bytes, head.next_seq - 1);
		rc = hubml_ledger_append("ledger_repair", "hub", fl, NULL, NULL);
	}
	return rc;
}

void hubml_ledger_head(struct hubml_ledger_head *out)
{
	k_mutex_lock(&lmtx, K_FOREVER);
	*out = head;
	k_mutex_unlock(&lmtx);
}

static void ts_str(char *o, size_t n)
{
	const int64_t now = hubml_time_now();
	if (!hubml_time_is_set()) {
		snprintf(o, n, "T+%llds", (long long)(now / 1000));
		return;
	}
	time_t t = (time_t)(now / 1000);
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(o, n, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		 tm.tm_min, tm.tm_sec);
}

int hubml_ledger_append(const char *kind, const char *actor, const char *fields, const char *sig, uint32_t *seq_out)
{
	static char l[HUBML_LEDGER_LINE_MAX + 2];
	k_mutex_lock(&lmtx, K_FOREVER);
	if (!head.ok) {
		k_mutex_unlock(&lmtx);
		return -EBADMSG;
	}
	char ts[24];
	ts_str(ts, sizeof ts);
	const int n = snprintf(l, HUBML_LEDGER_LINE_MAX + 1,
			       "{\"v\":1,\"seq\":%u,\"prev\":\"%s\",\"ts\":\"%s\",\"kind\":\"%s\",\"hub\":\"%s\",\"actor\":\"%s\"%s%s%s%s}",
			       head.next_seq, head.hash, ts, kind, hub_id, actor ? actor : "hub",
			       (fields && *fields) ? "," : "", fields ? fields : "",
			       sig ? ",\"sig\":" : "", sig ? sig : "");
	if (n <= 0 || n > HUBML_LEDGER_LINE_MAX || memchr(l, '\n', (size_t)n)) {
		k_mutex_unlock(&lmtx);
		return -EINVAL;
	}
	l[n] = '\n';
	char p[64];
	lpath(p, sizeof p);
	hubml_fs_lock();
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, p, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (!rc) {
		rc = fs_write(&f, l, (size_t)n + 1) == n + 1 ? 0 : -EIO;
		const int rc2 = fs_close(&f);
		rc = rc ? rc : rc2;
	}
	hubml_fs_unlock();
	if (!rc) {
		line_hash(l, (size_t)n, head.hash);
		replay(l, (size_t)n);
		if (seq_out) {
			*seq_out = head.next_seq;
		}
		head.next_seq++;
	}
	k_mutex_unlock(&lmtx);
	return rc;
}

int hubml_ledger_read(uint32_t since, int max, hubml_ledger_cb cb, void *user)
{
	char p[64];
	lpath(p, sizeof p);
	static char l[HUBML_LEDGER_LINE_MAX + 2];
	k_mutex_lock(&lmtx, K_FOREVER);
	hubml_fs_lock();
	struct fs_file_t f;
	fs_file_t_init(&f);
	int cnt = 0;
	if (fs_open(&f, p, FS_O_READ) == 0) {
		size_t n = 0;
		uint32_t seq = 1;
		char c;
		while (cnt < max && fs_read(&f, &c, 1) == 1) {
			if (c != '\n') {
				if (n < sizeof l - 1) {
					l[n++] = c;
				}
				continue;
			}
			if (seq >= since) {
				cb(seq, l, n, user);
				cnt++;
			}
			seq++;
			n = 0;
		}
		fs_close(&f);
	}
	hubml_fs_unlock();
	k_mutex_unlock(&lmtx);
	return cnt;
}

static int mkdirs(uint64_t uid)
{
	char p[80];
	snprintf(p, sizeof p, "%s/models", root);
	int rc = fs_mkdir(p);
	if (rc && rc != -EEXIST) {
		return rc;
	}
	snprintf(p, sizeof p, "%s/models/%016llx", root, (unsigned long long)uid);
	rc = fs_mkdir(p);
	return (rc == -EEXIST) ? 0 : rc;
}

int hubml_model_register(uint64_t uid, const uint8_t *xmb, size_t n, const char *actor, const char *extra,
			 char id_out[HUBML_MODEL_ID_LEN + 1])
{
	if (n < 64 || memcmp(xmb, "XMB1", 4)) {
		return -EINVAL;
	}
	/* 관문: 원장에 올리기 전에 파일 자체를 검사한다 (CRC · 상한 · 골든) — 깨진 모델은 원장에 들어가지 않는다 */
	{
		static struct xmb_model m1;
		static struct xmd_model m3;
		static K_MUTEX_DEFINE(vm);
		k_mutex_lock(&vm, K_FOREVER);
		const int v = xmb[5] == 1 ? xmb_parse(xmb, n, &m1, 1e-5f) :
			      (xmb[5] == 3 ? xmd_parse(xmb, n, &m3, 1e-5f) : -3);
		k_mutex_unlock(&vm);
		if (v) {
			LOG_WRN("모델 거부 (kind %u, 사유 %d)", xmb[5], v);
			return -EBADMSG;
		}
	}
	struct sha256 s;
	uint8_t d[32];
	char full[65];
	sha256_init(&s);
	sha256_update(&s, xmb, n);
	sha256_final(&s, d);
	hex(d, 32, full);
	k_mutex_lock(&lmtx, K_FOREVER);
	const int sl = al_slot(uid, true);
	const uint32_t num = sl >= 0 ? al[sl].nmodel + 1 : 0;
	k_mutex_unlock(&lmtx);
	if (sl < 0) {
		return -ENOMEM;
	}
	snprintf(id_out, HUBML_MODEL_ID_LEN + 1, "%016llx:%u", (unsigned long long)uid, num);
	char p[96];
	snprintf(p, sizeof p, "%s/models/%016llx/%u.xmb", root, (unsigned long long)uid, num);
	hubml_fs_lock();
	int rc = mkdirs(uid);
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (!rc) {
		rc = fs_open(&f, p, FS_O_CREATE | FS_O_WRITE);
	}
	if (!rc) {
		rc = fs_truncate(&f, 0);
		if (!rc && fs_write(&f, xmb, n) != (ssize_t)n) {
			rc = -EIO;
		}
		const int rc2 = fs_close(&f);
		rc = rc ? rc : rc2;
	}
	hubml_fs_unlock();
	if (rc) {
		return rc;
	}
	char fl[HUBML_LEDGER_LINE_MAX / 2];
	snprintf(fl, sizeof fl,
		 "\"model_id\":\"%s\",\"uid\":\"%016llx\",\"type\":\"%s\",\"file\":\"models/%016llx/%u.xmb\","
		 "\"size\":%u,\"sha256\":\"%s\",\"contract\":%u,\"origin\":\"%s\"%s%s",
		 id_out, (unsigned long long)uid, xmb[5] == 3 ? "baseline_stats" : "tree", (unsigned long long)uid, num,
		 (unsigned)n, full, xmb[6], (actor && !strncmp(actor, "studio", 6)) ? "studio" : "hub",
		 extra ? "," : "", extra ? extra : "");
	return hubml_ledger_append("model", actor, fl, NULL, NULL);
}

static void model_path(char *p, size_t n, uint64_t uid, const char *model_id)
{
	const char *c = strchr(model_id, ':');
	snprintf(p, n, "%s/models/%016llx/%s.xmb", root, (unsigned long long)uid, c ? c + 1 : model_id);
}

int hubml_alias_set(uint64_t uid, enum hubml_alias a, const char *model_id, const char *reason, const char *actor,
		    const char *sig)
{
	if (a >= HUBML_NALIAS) {
		return -EINVAL;
	}
	if (a == HUBML_A_CHAMPION && (!sig || !*sig)) {
		return -EACCES;	/* 운영 승격은 사람 승인 서명이 있어야 한다 — 자동 승격 없음 */
	}
	char p[96];
	struct fs_dirent de;
	model_path(p, sizeof p, uid, model_id);
	hubml_fs_lock();
	const int st = fs_stat(p, &de);
	hubml_fs_unlock();
	if (st) {
		return -ENOENT;	/* 원장에 없는 모델로는 옮기지 않는다 */
	}
	const char *prev = hubml_alias_get(uid, a);
	char pv[HUBML_MODEL_ID_LEN + 3];
	snprintf(pv, sizeof pv, *prev ? "\"%s\"" : "null", prev);
	char fl[HUBML_LEDGER_LINE_MAX / 2];
	snprintf(fl, sizeof fl, "\"uid\":\"%016llx\",\"name\":\"%s\",\"model_id\":\"%s\",\"prev_model_id\":%s,\"reason\":\"%s\"",
		 (unsigned long long)uid, hubml_alias_name[a], model_id, pv, reason ? reason : "propose");
	return hubml_ledger_append("alias", actor, fl, sig, NULL);
}

const char *hubml_alias_get(uint64_t uid, enum hubml_alias a)
{
	const int s = al_slot(uid, false);
	return (s >= 0 && a < HUBML_NALIAS) ? al[s].id[a] : "";
}

int hubml_model_load(uint64_t uid, const char *id, uint8_t *buf, size_t cap)
{
	char p[96];
	model_path(p, sizeof p, uid, id);
	hubml_fs_lock();
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, p, FS_O_READ);
	if (!rc) {
		const ssize_t n = fs_read(&f, buf, cap);
		rc = n < 0 ? (int)n : (int)n;
		fs_close(&f);
	}
	hubml_fs_unlock();
	return rc;
}
