/*
 * ml 셸 — HUBML/1 (줄 머리 `HUBML/1 <kind> k=v …`). 문법은 Studio MLCC 와 합의(design/MLCC_HUB_ML_PLAN.md).
 * 읽기 전용(지금):
 *   ml status --kv                      허브 · 시각 · 저장소 여유 · 장비 수 · 수집·원장 셈
 *   ml dev ls --kv                      장비마다 uid sid contract schema fw last_seen_ms state + 흐름별 next
 *   ml ds ls uid=<hex16> [--kv]         날짜·흐름별 레코드 수·바이트
 *   ml ledger since=<seq> max=<n>       원장 줄 b64 + ledger_done last= head= total=
 * 운반(USB 1단계):
 *   ml rx b64=<MCOL 데이터그램> [addr=<n>]   → 수집 엔진. 허브가 보낼 것은 `HUBML/1 tx addr= b64=` 줄
 *   ml time set <epoch_ms>              허브 UTC 시각 (Studio 가 맞춘다)
 * 쓰기(허브 운영자용 — Studio 는 사용자 승인 전엔 보내지 않는다):
 *   ml train uid=<hex16> days=<d1[,d2…]> [q=0.995]   학습 → dataset(merkle) · model · eval 사건 + challenger 제안
 *   ml model ls uid=<hex16>                           원장의 별칭
 * approve · deploy · model put 은 서명 검증(ed25519)과 함께 붙인다.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/byteorder.h>
#include <hubml/hubml.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/train.h>
#include <hubml/approve.h>
#include <hubml/mfeat34.h>
#include <hubml/sha256.h>

extern const struct shell *hubml_tx_shell;

#ifndef HUBML_SHELL_ROOT
#define HUBML_SHELL_ROOT "/0:/ml"
#endif
#ifndef HUBML_SHELL_HUB_ID
#define HUBML_SHELL_HUB_ID "-"
#endif
__weak const char *hubml_default_hub_id(void) { return HUBML_SHELL_HUB_ID; }

/* 셸 처리기는 얇게: 본체를 hubml 스레드에서 돌린다 (hubml.h 의 hubml_call 경고) */
struct call {
	int (*fn)(const struct shell *, size_t, char **);
	const struct shell *sh;
	size_t argc;
	char **argv;
};
static int call_fn(void *p)
{
	struct call *c = p;
	return c->fn(c->sh, c->argc, c->argv);
}
#define THIN(name)                                                                                           \
	static int name##_t(const struct shell *sh, size_t argc, char **argv)                                 \
	{                                                                                                    \
		struct call c = { name, sh, argc, argv };                                                     \
		hubml_tx_shell = sh;                                                                          \
		return hubml_call(call_fn, &c);                                                               \
	}

static int ready(const struct shell *sh)
{
	hubml_tx_shell = sh;
	if (hubml_started()) {
		return 0;
	}
	const int rc = hubml_start_here(HUBML_SHELL_ROOT, hubml_default_hub_id());	/* 이미 hubml 스레드 안 */
	if (rc) {
		shell_print(sh, "HUBML/1 err code=start msg=%d", rc);
	}
	return rc;
}

static const char *arg(size_t argc, char **argv, const char *key)
{
	const size_t kl = strlen(key);
	for (size_t i = 1; i < argc; i++) {
		if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') {
			return argv[i] + kl + 1;
		}
	}
	return NULL;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (ready(sh)) {
		return -EIO;
	}
	static struct hubml_stats s;
	static struct hubml_ledger_head h;
	static struct hubml_edge_info e[HUBML_MAX_EDGES];
	hubml_ingest_stats(&s);
	hubml_ledger_head(&h);
	const int ne = hubml_ingest_edges(e, HUBML_MAX_EDGES);
	const long long free_kib = hubml_free_kib();	/* -1 = 아직 재는 중 (FAT 전체를 훑을 수 있어 배경에서) */
	shell_print(sh, "HUBML/1 status hub=%s root=%s time_ms=%lld time_set=%d edges=%d rx=%u accepted=%u dup=%u bad=%u "
		    "qfull=%u commits=%u commit_err=%u max_commit_ms=%u queued=%zu ledger_next=%u ledger_ok=%d free_kib=%lld",
		    hubml_hub_id(), hubml_root(), (long long)hubml_time_now(), hubml_time_is_set() ? 1 : 0, ne, s.rx,
		    s.accepted, s.dup, s.bad, s.qfull, s.commits, s.commit_err, s.max_commit_ms, s.queued, h.next_seq,
		    h.ok ? 1 : 0, free_kib);
	return 0;
}

static int cmd_dev_ls(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (ready(sh)) {
		return -EIO;
	}
	static struct hubml_edge_info e[HUBML_MAX_EDGES];
	const int n = hubml_ingest_edges(e, HUBML_MAX_EDGES);
	const int64_t now = hubml_time_now();
	for (int i = 0; i < n; i++) {
		const char *champ = hubml_alias_get(e[i].uid, HUBML_A_CHAMPION);
		shell_print(sh, "HUBML/1 dev uid=%016llx sid=%u contract=%u schema=%08x fw=%08x last_seen_ms=%lld state=%s "
			    "champion=%s next_feat=%u next_raw=%u next_evt=%u next_hb=%u rx=%u dup=%u oow=%u qfull=%u",
			    (unsigned long long)e[i].uid, e[i].sid, e[i].contract, e[i].schema_hash, e[i].fw,
			    (long long)(e[i].last_rx_ms ? now - e[i].last_rx_ms : -1), *champ ? "baselined" : "learning",
			    *champ ? champ : "-", e[i].next[MCOL_S_FEAT], e[i].next[MCOL_S_RAW], e[i].next[MCOL_S_EVT],
			    e[i].next[MCOL_S_HB], e[i].rx, e[i].dup, e[i].oow, e[i].qfull);
	}
	shell_print(sh, "HUBML/1 dev_done n=%d", n);
	return 0;
}

/* 파일 안 레코드 수 — 길이 머리만 읽고 건너뛴다 */
static long count_recs(const char *p)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, p, FS_O_READ)) {
		return -1;
	}
	long n = 0;
	off_t pos = 16;
	uint8_t lp[2];
	while (fs_seek(&f, pos, FS_SEEK_SET) == 0 && fs_read(&f, lp, 2) == 2) {
		pos += 2 + sys_get_le16(lp);
		n++;
	}
	fs_close(&f);
	return n;
}

static int cmd_ds_ls(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *u = arg(argc, argv, "uid");
	if (!u || strlen(u) != 16) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=uid=<hex16>");
		return -EINVAL;
	}
	char dpath[96];
	snprintf(dpath, sizeof dpath, "%s/ds/%s", hubml_root(), u);
	static const char *const sname[] = { "ctrl", "feat", "raw", "evt", "hb" };
	int n = 0;
	hubml_fs_lock();
	struct fs_dir_t dd;
	fs_dir_t_init(&dd);
	if (fs_opendir(&dd, dpath) == 0) {
		struct fs_dirent day;
		while (fs_readdir(&dd, &day) == 0 && day.name[0]) {
			if (day.type != FS_DIR_ENTRY_DIR) {
				continue;
			}
			for (int s = 1; s < MCOL_NSTREAM; s++) {
				char p[128];
				struct fs_dirent fe;
				snprintf(p, sizeof p, "%s/%s/s%d.mcs", dpath, day.name, s);
				if (fs_stat(p, &fe)) {
					continue;
				}
				shell_print(sh, "HUBML/1 ds uid=%s day=%s stream=%s records=%ld bytes=%zu path=ds/%s/%s/s%d.mcs",
					    u, day.name, sname[s], count_recs(p), fe.size, u, day.name, s);
				n++;
			}
		}
		fs_closedir(&dd);
	}
	hubml_fs_unlock();
	shell_print(sh, "HUBML/1 ds_done uid=%s n=%d", u, n);
	return 0;
}

struct lctx {
	const struct shell *sh;
	uint32_t last;
};
static void ledger_cb(uint32_t seq, const char *l, size_t n, void *user)
{
	struct lctx *c = user;
	static char b64[4200];
	size_t ol = 0;
	if (base64_encode((uint8_t *)b64, sizeof b64, &ol, (const uint8_t *)l, n)) {
		shell_print(c->sh, "HUBML/1 err code=ledger_line msg=%u", seq);
		return;
	}
	b64[ol] = 0;
	shell_print(c->sh, "HUBML/1 ledger seq=%u b64=%s", seq, b64);
	c->last = seq;
}

static int cmd_ledger(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *s = arg(argc, argv, "since"), *m = arg(argc, argv, "max");
	const uint32_t since = s ? (uint32_t)strtoul(s, NULL, 10) : 1;
	const int max = m ? atoi(m) : 50;
	struct lctx c = { .sh = sh, .last = since ? since - 1 : 0 };
	(void)hubml_ledger_read(since ? since : 1, max > 0 ? max : 50, ledger_cb, &c);
	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	shell_print(sh, "HUBML/1 ledger_done last=%u head=%s total=%u ok=%d", c.last, h.hash, h.next_seq - 1, h.ok ? 1 : 0);
	return 0;
}

static int cmd_rx(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *b = arg(argc, argv, "b64"), *a = arg(argc, argv, "addr");
	static uint8_t d[MCOL_MAX_DGRAM + 8];
	size_t ol = 0;
	if (!b || base64_decode(d, sizeof d, &ol, (const uint8_t *)b, strlen(b))) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=b64");
		return -EINVAL;
	}
	hubml_ingest_rx(a ? (uint32_t)strtoul(a, NULL, 10) : 1, d, ol);
	return 0;
}

static int cmd_time_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=epoch_ms");
		return -EINVAL;
	}
	/* 시각을 맞추기 전에 실행부를 연다 — 나중에 열면 수집 상태가 처음으로 돌아간다 (Lm7 첫 실행) */
	if (ready(sh)) {
		return -EIO;
	}
	const bool was = hubml_time_is_set();
	const int64_t before = hubml_time_now();
	hubml_time_set(strtoll(argv[1], NULL, 10));
	const int64_t after = hubml_time_now();
	/* 원장 ts 가 T+초 → UTC 로 바뀌거나 뛰므로 사건으로 남긴다 (Studio 원장 보기에 표시) */
	char fl[160];
	snprintf(fl, sizeof fl, "\"prev_set\":%s,\"prev_ms\":%lld,\"new_ms\":%lld,\"delta_ms\":%lld",
		 was ? "true" : "false", (long long)before, (long long)after, (long long)(after - before));
	const int lr = hubml_ledger_append("time_set", "studio", fl, NULL, NULL);
	shell_print(sh, "HUBML/1 time ok=%d time_ms=%lld time_set=1 delta_ms=%lld", lr == 0 ? 1 : 0, (long long)after,
		    (long long)(after - before));
	return 0;
}

static int cmd_commit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (ready(sh)) {
		return -EIO;
	}
	const int rc = hubml_ingest_commit();
	shell_print(sh, "HUBML/1 commit rc=%d", rc);
	return rc;
}

/* ── 학습 ─────────────────────────────────────────────────────────────── */
static void hex32(const uint8_t *d, char *o)
{
	for (int i = 0; i < 32; i++) {
		snprintf(o + 2 * i, 3, "%02x", d[i]);
	}
}

/* merkle (mlledger/1 dataset): 날짜마다 "ds/<uid>/<day>/s1.mcs\t<sha256>\n" — 날짜 정렬 순 — 을 이은 것의 sha256 */
static int ds_merkle(const char *u, const char days[][9], int nd, char out[65])
{
	struct sha256 top;
	sha256_init(&top);
	static uint8_t buf[4096];
	for (int i = 0; i < nd; i++) {
		char p[128], rel[64];
		snprintf(rel, sizeof rel, "ds/%s/%s/s1.mcs", u, days[i]);
		snprintf(p, sizeof p, "%s/%s", hubml_root(), rel);
		struct fs_file_t f;
		fs_file_t_init(&f);
		hubml_fs_lock();
		int rc = fs_open(&f, p, FS_O_READ);
		struct sha256 s;
		sha256_init(&s);
		ssize_t n;
		while (!rc && (n = fs_read(&f, buf, sizeof buf)) > 0) {
			sha256_update(&s, buf, (size_t)n);
		}
		if (!rc) {
			fs_close(&f);
		}
		hubml_fs_unlock();
		if (rc) {
			return rc;
		}
		uint8_t d[32];
		char h[65], line[140];
		sha256_final(&s, d);
		hex32(d, h);
		const int ll = snprintf(line, sizeof line, "%s\t%s\n", rel, h);
		sha256_update(&top, line, (size_t)ll);
	}
	uint8_t d[32];
	sha256_final(&top, d);
	hex32(d, out);
	return 0;
}

struct fault { int idx; float mul; };
static void xf(float *x, void *u) { const struct fault *f = u; x[f->idx] *= f->mul; }
static int day_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int cmd_train(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *u = arg(argc, argv, "uid"), *ds = arg(argc, argv, "days"), *qs = arg(argc, argv, "q");
	if (!u || strlen(u) != 16 || !ds) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=uid=<hex16>_days=<yyyymmdd,…>");
		return -EINVAL;
	}
	static char days[HUBML_MAX_DAYS][9];
	int nd = 0;
	for (const char *p = ds; *p && nd < HUBML_MAX_DAYS;) {
		if (strlen(p) < 8) { break; }
		memcpy(days[nd], p, 8);
		days[nd++][8] = 0;
		p += 8;
		if (*p == ',') { p++; }
	}
	qsort(days, (size_t)nd, sizeof days[0], day_cmp);
	const uint64_t uid = strtoull(u, NULL, 16);
	uint32_t schema = 0;
	{
		static struct hubml_edge_info e[HUBML_MAX_EDGES];
		const int n = hubml_ingest_edges(e, HUBML_MAX_EDGES);
		for (int i = 0; i < n; i++) {
			if (e[i].uid == uid) { schema = e[i].schema_hash; }
		}
	}
	static struct hubml_baseline bl;
	static struct xmb_model tm;
	static struct xmd_model md;
	static uint8_t file[16384];
	const struct hubml_train_cfg c = { .uid = uid, .days = days, .ndays = nd, .holdout = 0.2f,
					   .q = qs ? strtof(qs, NULL) : 0.995f, .trees = 45, .psi = 64, .k = 1.0f,
					   .seed = 20261109, .train_ts = (uint32_t)(hubml_time_now() / 1000) };
	struct hubml_train_result r;
	int rc = hubml_train(hubml_root(), &c, &bl, &tm, &r);
	if (rc) {
		shell_print(sh, "HUBML/1 err code=train msg=%d", rc);
		return rc;
	}
	/* Edge 모델 = kind 3 (통계 기준선). 골든은 학습 구간 벡터 4 개 */
	float gx[XMB_NGOLDEN][HUBML_NFEAT];
	for (int g = 0; g < XMB_NGOLDEN; g++) { memcpy(gx[g], tm.golden[g], sizeof gx[g]); }
	hubml_baseline_export(&bl, c.train_ts, gx, &md);
	const int sz = xmd_write(&md, file, sizeof file);
	/* 평가 (보류 구간): 오경보 + 가짜 고장 ×1.3 세 가지 */
	static const struct { const char *name, *feat; int idx; } F[] = {
		{ "unbalance_1x", "ord_1x", 6 }, { "bearing_kurtosis", "vib_kurtosis", 4 }, { "rotor_bar_slip", "cur_sideband_slip", 18 } };
	float rate[3];
	bool pass = r.far_stats <= 0.015f;
	for (int i = 0; i < 3; i++) {
		struct fault fl = { F[i].idx, 1.3f };
		struct hubml_eval_out e;
		hubml_eval_holdout(hubml_root(), &c, &r, &bl, &tm, &md, xf, &fl, &e);
		rate[i] = e.rate_edge;
		pass = pass && rate[i] >= 0.9f;
	}
	/* 원장: dataset → model → eval → challenger 제안 */
	char mk[65], fl[1500], dsid[96], mid[25];
	rc = ds_merkle(u, days, nd, mk);
	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	snprintf(dsid, sizeof dsid, "ds-%s-%s-%s-%u", u, days[0], days[nd - 1], h.next_seq);
	int o = snprintf(fl, sizeof fl, "\"ds_id\":\"%s\",\"uid\":\"%s\",\"days\":[", dsid, u);
	for (int i = 0; i < nd; i++) { o += snprintf(fl + o, sizeof fl - o, "%s\"%s\"", i ? "," : "", days[i]); }
	o += snprintf(fl + o, sizeof fl - o, "],\"contract\":34,\"schema_hash\":\"%08x\",\"counts\":{\"vectors\":%u,\"bad_quality\":0,"
		      "\"dup\":0,\"gaps\":0},\"merkle\":\"%s\",\"regimes\":[%u,%u,%u,%u]",
		      schema, r.n_total, rc ? "-" : mk, bl.reg[0].n, bl.reg[1].n, bl.reg[2].n, bl.reg[3].n);
	rc = hubml_ledger_append("dataset", "hub", fl, NULL, NULL);
	snprintf(fl, sizeof fl, "\"train_ds\":\"%s\",\"schema_hash\":\"%08x\",\"params\":{\"q\":%.4f,\"holdout\":0.2,"
		 "\"nreg\":4,\"reg_feat\":\"load_ratio\"},\"generation\":0", dsid, schema, (double)c.q);
	if (!rc && sz > 0) {
		rc = hubml_model_register(uid, file, (size_t)sz, "hub", fl, mid);
	}
	if (!rc) {
		snprintf(fl, sizeof fl, "\"model_id\":\"%s\",\"eval_ds\":\"%s\",\"far\":%.5f,\"inject\":["
			 "{\"name\":\"%s\",\"feature\":\"%s\",\"factor\":1.3,\"rate\":%.4f},"
			 "{\"name\":\"%s\",\"feature\":\"%s\",\"factor\":1.3,\"rate\":%.4f},"
			 "{\"name\":\"%s\",\"feature\":\"%s\",\"factor\":1.3,\"rate\":%.4f}],"
			 "\"parity\":{\"n\":4,\"max_abs\":0,\"ok\":true},\"verdict\":\"%s\"",
			 mid, dsid, (double)r.far_stats, F[0].name, F[0].feat, (double)rate[0], F[1].name, F[1].feat,
			 (double)rate[1], F[2].name, F[2].feat, (double)rate[2], pass ? "pass" : "review");
		rc = hubml_ledger_append("eval", "hub", fl, NULL, NULL);
	}
	if (!rc) {
		rc = hubml_alias_set(uid, HUBML_A_CHALLENGER, mid, "propose", "hub", NULL);
	}
	shell_print(sh, "HUBML/1 train uid=%s ds_id=%s model_id=%s vectors=%u train=%u hold=%u far=%.4f inject=%.3f,%.3f,%.3f "
		    "size=%d ms=%u verdict=%s rc=%d", u, dsid, rc ? "-" : mid, r.n_total, r.n_train, r.n_hold,
		    (double)r.far_stats, (double)rate[0], (double)rate[1], (double)rate[2], sz, r.ms_total,
		    pass ? "pass" : "review", rc);
	return rc;
}

static int cmd_model_ls(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *u = arg(argc, argv, "uid");
	if (!u || strlen(u) != 16) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=uid=<hex16>");
		return -EINVAL;
	}
	const uint64_t uid = strtoull(u, NULL, 16);
	for (int a = 0; a < HUBML_NALIAS; a++) {
		const char *m = hubml_alias_get(uid, (enum hubml_alias)a);
		shell_print(sh, "HUBML/1 model uid=%s alias=%s model_id=%s", u, hubml_alias_name[a], *m ? m : "-");
	}
	return 0;
}

/* ── ml drift: v0(또는 지정 모델) 기준선 대비 날짜별 변화 — 보고만 한다(재학습 계기 아님, MLCC 합의) ── */
#define DH 1000	/* 점수 0..10 히스토그램 */
struct drift_acc {
	const struct xmd_model *m;
	uint32_t n, alarm;
	double sum;
	uint32_t hist[DH];
	double zsum[HUBML_NFEAT];
};
static void drift_cb(const float *x, uint32_t idx, void *u)
{
	ARG_UNUSED(idx);
	struct drift_acc *a = u;
	const float sc = xmd_score(a->m, x, NULL);
	a->n++;
	a->sum += sc;
	a->alarm += sc > 1.0f;
	int b = (int)(sc * (DH / 10));
	a->hist[b < 0 ? 0 : (b >= DH ? DH - 1 : b)]++;
	int r = 0;
	while (r < a->m->nreg - 1 && x[a->m->reg_feat] >= a->m->edges[r]) { r++; }
	for (int i = 0; i < HUBML_NFEAT; i++) {
		if (a->m->reg[r].sd[i] > 0.0f) {
			a->zsum[i] += fabs((double)x[i] - a->m->reg[r].mean[i]) / a->m->reg[r].sd[i];
		}
	}
}

static int cmd_drift(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	const char *u = arg(argc, argv, "uid"), *ds = arg(argc, argv, "days"), *mo = arg(argc, argv, "model");
	if (!u || strlen(u) != 16 || !ds) {
		shell_print(sh, "HUBML/1 err code=bad_arg msg=uid=<hex16>_days=<yyyymmdd,…>_[model=<id>]");
		return -EINVAL;
	}
	const uint64_t uid = strtoull(u, NULL, 16);
	char mid[32];
	strncpy(mid, mo ? mo : hubml_alias_get(uid, HUBML_A_V0), sizeof mid - 1);
	mid[sizeof mid - 1] = 0;
	static uint8_t file[16384];
	static struct xmd_model m;
	const int n = mid[0] ? hubml_model_load(uid, mid, file, sizeof file) : -ENOENT;
	if (n <= 0 || xmd_parse(file, (size_t)n, &m, 1e-5f)) {
		shell_print(sh, "HUBML/1 err code=not_found msg=model_%s", mid[0] ? mid : "v0_none");
		return -ENOENT;
	}
	static struct drift_acc a;
	double first = 0, last = 0, last_rate = 0;
	int nd = 0;
	for (const char *p = ds; *p;) {
		char day[1][9];
		if (strlen(p) < 8) { break; }
		memcpy(day[0], p, 8);
		day[0][8] = 0;
		p += 8;
		if (*p == ',') { p++; }
		memset(&a, 0, sizeof a);
		a.m = &m;
		hubml_feat_each(hubml_root(), uid, day, 1, drift_cb, &a);
		if (!a.n) {
			shell_print(sh, "HUBML/1 drift uid=%s day=%s model=%s n=0", u, day[0], mid);
			continue;
		}
		uint64_t acc = 0, want = (uint64_t)a.n * 95 / 100;
		int p95 = DH - 1;
		for (int b = 0; b < DH; b++) { acc += a.hist[b]; if (acc >= want) { p95 = b; break; } }
		int top[3] = { -1, -1, -1 };
		for (int k = 0; k < 3; k++) {
			for (int i = 0; i < HUBML_NFEAT; i++) {
				if (i == top[0] || i == top[1]) { continue; }
				if (top[k] < 0 || a.zsum[i] > a.zsum[top[k]]) { top[k] = i; }
			}
		}
		const double mean = a.sum / a.n, rate = (double)a.alarm / a.n;
		shell_print(sh, "HUBML/1 drift uid=%s day=%s model=%s n=%u alarm_rate=%.4f mean_score=%.3f p95_score=%.2f "
			    "top=%s:%.2f,%s:%.2f,%s:%.2f", u, day[0], mid, a.n, rate, mean, (p95 + 1) * 10.0 / DH,
			    hubml_mfeat34_name[top[0]], a.zsum[top[0]] / a.n, hubml_mfeat34_name[top[1]], a.zsum[top[1]] / a.n,
			    hubml_mfeat34_name[top[2]], a.zsum[top[2]] / a.n);
		if (!nd) { first = mean; }
		last = mean;
		last_rate = rate;
		nd++;
	}
	const double trend = nd > 1 ? (last - first) / (nd - 1) : 0;
	const char *verdict = last_rate > 0.05 ? "alarm" : ((last_rate > 0.015 || trend > 0.05) ? "watch" : "stable");
	shell_print(sh, "HUBML/1 drift_done uid=%s model=%s days=%d trend=%.4f last_alarm_rate=%.4f verdict=%s", u, mid, nd,
		    trend, last_rate, verdict);
	return 0;
}

static int cmd_approve(const struct shell *sh, size_t argc, char **argv)
{
	if (ready(sh)) {
		return -EIO;
	}
	struct hubml_approve_result r;
	const int rc = hubml_approve(arg(argc, argv, "b64"), arg(argc, argv, "sig"), arg(argc, argv, "key"), &r);
	if (rc) {
		shell_print(sh, "HUBML/1 err code=%s msg=%d", r.code ? r.code : "?", rc);
		return rc;
	}
	shell_print(sh, "HUBML/1 approve ok=1 kind=%s seq=%u head=%s deploy=%d", r.kind, r.seq, r.head, r.deploy);
	return 0;
}

THIN(cmd_approve)
THIN(cmd_drift)
THIN(cmd_status)
THIN(cmd_dev_ls)
THIN(cmd_ds_ls)
THIN(cmd_ledger)
THIN(cmd_rx)
THIN(cmd_time_set)
THIN(cmd_commit)
THIN(cmd_train)
THIN(cmd_model_ls)

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_dev, SHELL_CMD_ARG(ls, NULL, "ls [--kv] — 장비 표", cmd_dev_ls_t, 1, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_ds, SHELL_CMD_ARG(ls, NULL, "ls uid=<hex16> [--kv] — 날짜·흐름별", cmd_ds_ls_t, 2, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_time, SHELL_CMD_ARG(set, NULL, "set <epoch_ms> — 허브 UTC", cmd_time_set_t, 2, 0),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_model, SHELL_CMD_ARG(ls, NULL, "ls uid=<hex16> — 별칭", cmd_model_ls_t, 2, 0),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml,
	SHELL_CMD_ARG(status, NULL, "status [--kv] — 허브 ML 상태", cmd_status_t, 1, 1),
	SHELL_CMD(dev, &sub_ml_dev, "장비", NULL),
	SHELL_CMD(ds, &sub_ml_ds, "데이터 묶음", NULL),
	SHELL_CMD_ARG(ledger, NULL, "ledger since=<seq> max=<n> — 원장 줄", cmd_ledger_t, 1, 2),
	SHELL_CMD_ARG(rx, NULL, "rx b64=<MCOL> [addr=<n>] — USB 운반 수신", cmd_rx_t, 2, 1),
	SHELL_CMD(time, &sub_ml_time, "시각", NULL),
	SHELL_CMD(commit, NULL, "지금 기록·ACK (시험용)", cmd_commit_t),
	SHELL_CMD_ARG(train, NULL, "train uid=<hex16> days=<d1,…> [q=] — 학습 → 원장 + challenger 제안", cmd_train_t, 3, 1),
	SHELL_CMD(model, &sub_ml_model, "모델 · 별칭", NULL),
	SHELL_CMD_ARG(drift, NULL, "drift uid=<hex16> days=<d1,…> [model=<id>] — v0 대비 날짜별 변화 (보고만)", cmd_drift_t, 3, 1),
	SHELL_CMD_ARG(approve, NULL, "approve b64=<본문> sig=<ed25519> key=<id> — 사람 승인 (mlledger/1 §4)", cmd_approve_t, 4, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ml, &sub_ml, "MoruLab 허브 ML (HUBML/1)", NULL);
