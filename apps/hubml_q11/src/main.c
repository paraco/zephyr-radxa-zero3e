/*
 * hubml_q11 (Lm11) — `ml train gate=<seq>` : 원장의 승인된 normal_gate 구간(측정 UTC)으로 학습 데이터를 거른다 (QEMU 가짜 셸).
 *  1 걸러내기 대조: 날짜 폴더 6 개에 .mcs + anchor.jsonl 을 쓰고 (경계 [시작,끝) · 24:00 · exclude 구멍 · D+1/D+2 파일 · 동률 앵커 ·
 *    time_set=0 · 앵커 없는 boot · dt 누적), 남은 벡터 id 열과 셈(unanchored/outside/excluded)을 Python 기준(tools/hubml_testdata.py
 *    gate_ref — Studio 규칙을 옮긴 두 번째 구현)이 만든 값과 비트까지 대조한다. 읽는 파일 집합(D·D+1·D+2)도 그 안에 들어 있다.
 *  2 거부: reject · kind 다름 · sig 없음 · uid 불일치 · 없는 seq · 구간 모양 위반 · 구간 수 초과 · gate+days 동시
 *  3 학습 한 번 끝까지(관문 경로): dataset 의 gate_seq · gate_ds_id · counts · merkle(include 날짜만), model 의 gate_seq, challenger 등록
 *  4 자동 선택: gate·days 없음 → 그 uid 의 가장 최근 accept+sig 관문 (reject · sig 없음은 건너뜀) · 없으면 no_gate
 *  5 관문 없는 예전 경로(days=): dataset · model 줄에 관문 칸이 없다
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/byteorder.h>
#include <ff.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/hubml.h>
#include <hubml/gate.h>
#include <hubml/train.h>
#include <hubml/sha256.h>
#include "gate_scenario.inc"

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)
#define ROOT "/RAM:/ml"
#define UA_S "4d50444d000000e1"
#define UB_S "4d50444d000000e2"
#define UF_S "4d50444d000000e3"
#define UA 0x4d50444d000000e1ull
#define UB 0x4d50444d000000e2ull
#define NF 34
static const struct shell *sh;
static char out[65536];
static const char *pending;
static K_SEM_DEFINE(cmd_go, 0, 1);
static K_SEM_DEFINE(cmd_done, 0, 1);
static void cmd_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&cmd_go, K_FOREVER);
		shell_execute_cmd(sh, pending);
		k_sem_give(&cmd_done);
	}
}
K_THREAD_DEFINE(cmd_thr, 4096, cmd_fn, NULL, NULL, NULL, 5, 0, 0);
static const char *run(const char *cmd)
{
	shell_backend_dummy_clear_output(sh);
	pending = cmd;
	k_sem_give(&cmd_go);
	k_sem_take(&cmd_done, K_FOREVER);
	size_t n;
	const char *o = shell_backend_dummy_get_output(sh, &n);
	memcpy(out, o, MIN(n, sizeof out - 1));
	out[MIN(n, sizeof out - 1)] = 0;
	return out;
}

/* ── 가짜 특징 ── */
static uint32_t rs = 777;
static double ur(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs >> 8) * (1.0 / 16777216.0) + 1e-9; }
static double gauss(void) { return sqrt(-2.0 * log(ur())) * cos(6.283185307179586 * ur()); }
static void gen_vec(uint32_t t, float *x)
{
	const double load = ((t / 7200) % 2 ? 0.8 : 0.4) + 0.02 * gauss();
	double z[4];
	for (int j = 0; j < 4; j++) { z[j] = gauss(); }
	for (int i = 0; i < NF; i++) {
		const double base = 1.0 + 0.1 * i / NF;
		double m = (i >= 16 && i <= 25) ? base * (0.5 + load) : base * (1.0 + 0.2 * load);
		for (int j = 0; j < 4; j++) { m += 0.05 * base * sin(i * (j + 1) + 1.0) * z[j]; }
		x[i] = (float)(m + 0.02 * base * gauss());
	}
	x[26] = (float)(1.0 + 0.005 * gauss());
	x[27] = (float)load;
}

/* ── RAM 디스크에 날짜 폴더 · .mcs · anchor.jsonl 쓰기 ── */
static void mkd(const char *p) { const int rc = fs_mkdir(p); if (rc && rc != -EEXIST) { printk("mkdir %s %d\n", p, rc); } }
static void day_dir(const char *uid, const char *day, char *dir, size_t cap)
{
	char p[96];
	snprintf(p, sizeof p, ROOT "/ds");
	mkd(p);
	snprintf(p, sizeof p, ROOT "/ds/%s", uid);
	mkd(p);
	snprintf(dir, cap, ROOT "/ds/%s/%s", uid, day);
	mkd(dir);
}
static struct fs_file_t wf;
static void mcs_open(const char *dir, uint64_t uid)
{
	char p[128];
	snprintf(p, sizeof p, "%s/s1.mcs", dir);
	fs_file_t_init(&wf);
	fs_unlink(p);
	if (fs_open(&wf, p, FS_O_CREATE | FS_O_WRITE)) { printk("open %s 실패\n", p); }
	uint8_t hd[16] = { 'M', 'C', 'S', '1' };
	sys_put_le64(uid, hd + 4);
	hd[12] = MCOL_S_FEAT;
	hd[13] = MCOL_VER;
	fs_write(&wf, hd, 16);
}
static void mcs_rec(uint16_t boot, uint32_t seq, uint64_t edge, int n, const uint16_t *dt, const uint8_t *q, const float *xs)
{
	static uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM + 2];
	const int L = mcol_feat_put(pl, sizeof pl, NF, n, xs, q);
	const size_t vl = mcol_feat_vec_len(NF);
	for (int i = 0; i < n; i++) {
		sys_put_le16(dt ? dt[i] : 1000, pl + MCOL_FEAT_HDR + (size_t)i * vl + 4 * NF + 2);
	}
	struct mcol_hdr h = { .type = MCOL_T_FEAT, .edge_sid = 1, .stream = MCOL_S_FEAT, .seq = seq, .edge_ms = edge,
			      .len = (uint16_t)L, .boot_id = boot };
	const int m = mcol_encode(d + 2, sizeof d - 2, &h, pl);
	sys_put_le16((uint16_t)m, d);
	fs_write(&wf, d, (size_t)m + 2);
}
static void anchor_line(const char *dir, uint32_t seq, uint16_t boot, int64_t edge, int64_t hub, int ts)
{
	char p[128], l[200];
	snprintf(p, sizeof p, "%s/anchor.jsonl", dir);
	const int n = snprintf(l, sizeof l, "{\"seq\":%u,\"stream\":1,\"boot_id\":%u,\"edge_ms\":%lld,\"hub_ms\":%lld,\"time_set\":%d}\n",
			       seq, boot, (long long)edge, (long long)hub, ts);
	struct fs_file_t f;
	fs_file_t_init(&f);
	fs_open(&f, p, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	fs_write(&f, l, (size_t)n);
	fs_close(&f);
}

/* ── 원장 도우미 ── */
static const char *SIG = "{\"alg\":\"ed25519\",\"key\":\"k1\",\"over\":\"sha256\",\"b64\":\"UNVERIFIED\"}";
static uint32_t add_gate(const char *kind, const char *uid, const char *dec, const char *inc, const char *exc, bool sig)
{
	static char f[2800];
	uint32_t seq = 0;
	snprintf(f, sizeof f, "\"ds_id\":\"ds-%s-20261110-20261111-1\",\"include\":%s,\"exclude\":%s,\"decision\":\"%s\","
		 "\"d0\":{\"verdict\":\"pass\"},\"exp\":\"2026-11-09T00:00:00Z\"", uid, inc, exc, dec);
	const int rc = hubml_ledger_append(kind, "studio:k1", f, sig ? SIG : NULL, &seq);
	if (rc) { printk("ledger append rc=%d\n", rc); }
	return seq;
}
static uint32_t acc(const char *uid, const char *inc, const char *exc) { return add_gate("normal_gate", uid, "accept", inc, exc, true); }

struct fnd { const char *a, *b; uint32_t seq; char line[3010]; };
static void fnd_cb(uint32_t seq, const char *l, size_t n, void *u)
{
	struct fnd *f = u;
	static char t[3010];
	memcpy(t, l, MIN(n, sizeof t - 1));
	t[MIN(n, sizeof t - 1)] = 0;
	if (strstr(t, f->a) && (!f->b || strstr(t, f->b))) {
		f->seq = seq;
		memcpy(f->line, t, sizeof f->line);
	}
}
/* 마지막으로 a · b 가 둘 다 있는 줄 (없으면 seq 0) */
static uint32_t find_line(const char *a, const char *b, struct fnd *f)
{
	*f = (struct fnd){ .a = a, .b = b };
	hubml_ledger_read(1, 100000, fnd_cb, f);
	return f->seq;
}

/* ── 걸러내기 대조 ── */
static uint32_t got[512];
static int ngot;
static void got_cb(const float *x, uint32_t idx, void *u) { ARG_UNUSED(u); ARG_UNUSED(idx); if (ngot < 512) { got[ngot] = (uint32_t)x[0]; } ngot++; }
static bool same_ids(const uint32_t *want, int n) { return ngot == n && !memcmp(got, want, sizeof(uint32_t) * (size_t)n); }

static void merkle_of(const char *uid, const char days[][9], int nd, char out_hex[65])
{
	struct sha256 top;
	sha256_init(&top);
	static uint8_t buf[4096];
	for (int i = 0; i < nd; i++) {
		char p[128], line[160], hx[65];
		snprintf(p, sizeof p, ROOT "/ds/%s/%s/s1.mcs", uid, days[i]);
		struct fs_file_t f;
		fs_file_t_init(&f);
		struct sha256 s;
		sha256_init(&s);
		ssize_t n;
		fs_open(&f, p, FS_O_READ);
		while ((n = fs_read(&f, buf, sizeof buf)) > 0) { sha256_update(&s, buf, (size_t)n); }
		fs_close(&f);
		uint8_t dg[32];
		sha256_final(&s, dg);
		for (int k = 0; k < 32; k++) { snprintf(hx + 2 * k, 3, "%02x", dg[k]); }
		const int ll = snprintf(line, sizeof line, "ds/%s/%s/s1.mcs\t%s\n", uid, days[i], hx);
		sha256_update(&top, line, (size_t)ll);
	}
	uint8_t dg[32];
	sha256_final(&top, dg);
	for (int k = 0; k < 32; k++) { snprintf(out_hex + 2 * k, 3, "%02x", dg[k]); }
}

static struct hubml_gate G1, G2;

static void check_filter(const char *name, const struct hubml_gate *g, const uint32_t *want, int nwant, uint32_t un, uint32_t outs,
			 uint32_t exc, uint32_t bad)
{
	struct hubml_gate_counts cn;
	ngot = 0;
	const int n = hubml_feat_each_gate(ROOT, UA, g->fdays, g->nfdays, g, got_cb, NULL, &cn);
	CHECK(n == nwant && same_ids(want, nwant), "%s 남은 벡터 id 열이 Python 기준과 같음 (%d 개, 읽은 날짜 %d 개)", name, n, g->nfdays);
	CHECK(cn.unanchored == un && cn.outside == outs && cn.excluded == exc && cn.bad == bad,
	      "%s 셈 unanchored=%u outside=%u excluded=%u bad=%u (기대 %u/%u/%u/%u — bad = quality≠0 + 비유한)", name, cn.unanchored, cn.outside,
	      cn.excluded, cn.bad, un, outs, exc, bad);
	/* 두 번째 패스도 같다 (패스마다 같은 흐름) */
	struct hubml_gate_counts c2;
	ngot = 0;
	hubml_feat_each_gate(ROOT, UA, g->fdays, g->nfdays, g, got_cb, NULL, &c2);
	CHECK(same_ids(want, nwant) && !memcmp(&cn, &c2, sizeof cn), "%s 두 번째 패스도 같은 흐름·같은 셈", name);
}

static void reject(const char *what, uint32_t seq, const char *code, const char *why)
{
	char cmd[96], want[96];
	snprintf(cmd, sizeof cmd, "ml train uid=" UA_S " gate=%u", seq);
	snprintf(want, sizeof want, "HUBML/1 err code=%s msg=%s", code, why);
	run(cmd);
	CHECK(strstr(out, want) != NULL, "거부 %s → %s", what, want + 8);
}

int main(void)
{
	printk("=== Lm11 ml train gate (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm11 FAIL\n"); return 0; }
	fs_mkdir(ROOT);
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);
	run("ml time set 1794186000000");

	/* ── 데이터: 시나리오(uid A) ── */
	{
		static float xs[8 * NF];
		char dir[128];
		for (int di = 0; di < ARRAY_SIZE(SC_DAYS); di++) {
			day_dir(UA_S, SC_DAYS[di], dir, sizeof dir);
			mcs_open(dir, UA);
			uint32_t seq = 0, a = 0;
			for (int i = 0; i < ARRAY_SIZE(SC_REC); i++) {
				const struct sc_rec *r = &SC_REC[i];
				if (r->day != di) { continue; }
				for (int k = 0; k < r->n; k++) {
					gen_vec(r->id0 + (uint32_t)k, &xs[k * NF]);
					xs[k * NF] = (float)(r->id0 + (uint32_t)k);
					if (r->nf[k]) { xs[k * NF + 5] = NAN; }
				}
				mcs_rec(r->boot, seq++, r->edge, r->n, r->dt, r->q, xs);
			}
			fs_close(&wf);
			for (int i = 0; i < ARRAY_SIZE(SC_ANCH); i++) {
				if (SC_ANCH[i].day == di) { anchor_line(dir, a++, SC_ANCH[i].boot, SC_ANCH[i].edge, SC_ANCH[i].hub, SC_ANCH[i].ts); }
			}
		}
	}
	/* 원장: 관문 2 개 (승인됨) */
	const uint32_t g1 = acc(UA_S, SC_G1_INC, SC_G1_EXC), g2 = acc(UA_S, SC_G2_INC, SC_G2_EXC);
	const char *code, *why;
	CHECK(!hubml_gate_load(g1, UA_S, &G1, &code, &why) && !hubml_gate_load(g2, UA_S, &G2, &code, &why),
	      "원장 normal_gate 두 줄(seq %u, %u)을 읽음", g1, g2);
	CHECK(G1.nidays == 2 && !strcmp(G1.fdays[0], "20261110") && G1.nfdays == 4 && !strcmp(G1.fdays[3], "20261113") &&
	      G2.nidays == 1 && G2.nfdays == 3 && !strcmp(G2.fdays[2], "20261112"),
	      "읽을 파일 = include 날짜마다 D·D+1·D+2 의 합집합 (G1 %d 개 · G2 %d 개)", G1.nfdays, G2.nfdays);
	check_filter("G1", &G1, SC_G1_KEPT, ARRAY_SIZE(SC_G1_KEPT), SC_G1_UNANCH, SC_G1_OUTSIDE, SC_G1_EXCLUDED, SC_G1_BAD);
	check_filter("G2", &G2, SC_G2_KEPT, ARRAY_SIZE(SC_G2_KEPT), SC_G2_UNANCH, SC_G2_OUTSIDE, SC_G2_EXCLUDED, SC_G2_BAD);
	/* 달·해 넘김 날짜 더하기 */
	{
		static struct hubml_gate Gy;
		const uint32_t s = acc(UA_S, "[[\"20261231\",\"00:00\",\"24:00\"],[\"20260228\",\"00:00\",\"01:00\"]]", "[]");
		CHECK(!hubml_gate_load(s, UA_S, &Gy, &code, &why) && Gy.nidays == 2 && Gy.nfdays == 6 &&
		      !strcmp(Gy.fdays[0], "20260228") && !strcmp(Gy.fdays[1], "20260301") && !strcmp(Gy.fdays[2], "20260302") &&
		      !strcmp(Gy.fdays[3], "20261231") && !strcmp(Gy.fdays[4], "20270101") && !strcmp(Gy.fdays[5], "20270102"),
		      "달·해 넘김 날짜 더하기 (20260228 → 0301·0302, 20261231 → 20270101·0102)");
	}

	/* ── 거부 ── */
	static const char *const OKI = "[[\"20261110\",\"10:00\",\"12:00\"]]";
	reject("decision=reject", add_gate("normal_gate", UA_S, "reject", OKI, "[]", true), "bad_arg", "gate_decision");
	reject("kind 다름", add_gate("maintenance", UA_S, "accept", OKI, "[]", true), "bad_arg", "gate_kind");
	reject("sig 없음", add_gate("normal_gate", UA_S, "accept", OKI, "[]", false), "bad_arg", "gate_nosig");
	reject("uid 불일치", add_gate("normal_gate", "4d50444d000000ff", "accept", OKI, "[]", true), "bad_arg", "gate_uid");
	reject("없는 seq", 9999, "not_found", "gate_not_found");
	reject("시작=끝", acc(UA_S, "[[\"20261110\",\"10:00\",\"10:00\"]]", "[]"), "bad_arg", "gate_range");
	reject("시작>끝", acc(UA_S, "[[\"20261110\",\"12:00\",\"10:00\"]]", "[]"), "bad_arg", "gate_range");
	reject("25:00", acc(UA_S, "[[\"20261110\",\"10:00\",\"25:00\"]]", "[]"), "bad_arg", "gate_time");
	reject("24:00 시작", acc(UA_S, "[[\"20261110\",\"24:00\",\"24:00\"]]", "[]"), "bad_arg", "gate_time");
	reject("24:30", acc(UA_S, "[[\"20261110\",\"10:00\",\"24:30\"]]", "[]"), "bad_arg", "gate_time");
	reject("분 60", acc(UA_S, "[[\"20261110\",\"10:60\",\"11:00\"]]", "[]"), "bad_arg", "gate_time");
	reject("exclude 사유 칸 없음", acc(UA_S, OKI, "[[\"20261110\",\"10:30\",\"10:40\"]]"), "bad_arg", "gate_reason");
	reject("exclude 사유 빈 문자열", acc(UA_S, OKI, "[[\"20261110\",\"10:30\",\"10:40\",\"\"]]"), "bad_arg", "gate_reason");
	reject("날짜 7 자리", acc(UA_S, "[[\"2026111\",\"10:00\",\"12:00\"]]", "[]"), "bad_arg", "gate_date");
	reject("날짜 없는 달", acc(UA_S, "[[\"20261332\",\"10:00\",\"12:00\"]]", "[]"), "bad_arg", "gate_date");
	reject("날짜 숫자 아님", acc(UA_S, "[[\"2026-1-10\",\"10:00\",\"12:00\"]]", "[]"), "bad_arg", "gate_date");
	reject("include 비었음", acc(UA_S, "[]", "[]"), "bad_arg", "gate_include_empty");
	{
		static char many[1500];
		int o = snprintf(many, sizeof many, "[");
		for (int i = 0; i < 33; i++) { o += snprintf(many + o, sizeof many - o, "%s[\"20261110\",\"00:00\",\"00:01\"]", i ? "," : ""); }
		snprintf(many + o, sizeof many - o, "]");
		reject("include 33 개", acc(UA_S, many, "[]"), "bad_arg", "gate_too_many");
		o = snprintf(many, sizeof many, "[");
		for (int i = 0; i < 33; i++) { o += snprintf(many + o, sizeof many - o, "%s[\"20261110\",\"00:00\",\"00:01\",\"x\"]", i ? "," : ""); }
		snprintf(many + o, sizeof many - o, "]");
		reject("exclude 33 개", acc(UA_S, OKI, many), "bad_arg", "gate_too_many");
		/* 32 개는 통과 */
		static struct hubml_gate Gm;
		o = snprintf(many, sizeof many, "[");
		for (int i = 0; i < 32; i++) { o += snprintf(many + o, sizeof many - o, "%s[\"20261110\",\"00:00\",\"00:01\"]", i ? "," : ""); }
		snprintf(many + o, sizeof many - o, "]");
		const uint32_t s = acc(UA_S, many, "[]");
		CHECK(!hubml_gate_load(s, UA_S, &Gm, &code, &why) && Gm.ninc == 32 && Gm.nidays == 1 && Gm.nfdays == 3, "include 32 개(상한)는 통과");
	}
	run("ml train uid=" UA_S " gate=1 days=20261110");
	CHECK(strstr(out, "HUBML/1 err code=bad_arg msg=gate_and_days") != NULL, "gate 와 days 동시 → bad_arg");
	run("ml train uid=" UA_S " gate=4294967297");
	CHECK(strstr(out, "HUBML/1 err code=bad_arg msg=gate_seq") != NULL, "gate=4294967297 (> UINT32_MAX) → bad_arg");
	run("ml train uid=" UA_S " gate=abc");
	CHECK(strstr(out, "HUBML/1 err code=bad_arg msg=gate_seq") != NULL, "gate=abc → bad_arg");
	run("ml train uid=" UF_S);
	CHECK(strstr(out, "HUBML/1 err code=no_gate msg=uid=" UF_S) != NULL, "관문이 하나도 없는 uid → err code=no_gate");

	/* uid A · G1 은 77 벡터 — 학습 최소(1000)에 못 미쳐 err code=train msg=-61. 거른 뒤 수로 판정한다는 증거 */
	{
		char cmd[96];
		snprintf(cmd, sizeof cmd, "ml train uid=" UA_S " gate=%u", g1);
		run(cmd);
		CHECK(strstr(out, "HUBML/1 err code=train msg=gate_too_few n=84 min=1000") != NULL, "G1(거른 뒤 %d 벡터 < 1000) → err code=train msg=gate_too_few n=84 min=1000", (int)ARRAY_SIZE(SC_G1_KEPT));
	}

	/* ── 자동 선택 + 큰 사례 (uid B) ── */
	{
		static float xs[8 * NF];
		char dir[128];
		day_dir(UB_S, SC_B_DAY, dir, sizeof dir);
		mcs_open(dir, UB);
		for (uint32_t r = 0; r < 502; r++) {
			const bool bb = r >= 500;
			for (int k = 0; k < 8; k++) { gen_vec(r * 8 + (uint32_t)k, &xs[k * NF]); }
			mcs_rec(bb ? 4 : 3, r, bb ? 5000000ull + 8000ull * (r - 500) : 8000ull * r, 8, NULL, NULL, xs);
		}
		fs_close(&wf);
		anchor_line(dir, 0, 3, 0, SC_B_HUB, 1);	/* 2026-11-20 00:00:00 UTC */
		for (int k = 1; k <= 2; k++) {	/* D+1 · D+2 파일: UTC 가 1120 인 묶음 (00:50 · 00:55) */
			char dd[9], d2[128];
			snprintf(dd, sizeof dd, "2026112%d", k);
			day_dir(UB_S, dd, d2, sizeof d2);
			mcs_open(d2, UB);
			for (int j = 0; j < 8; j++) { gen_vec(9000 + (uint32_t)j, &xs[j * NF]); }
			if (k == 1) { xs[3 * NF + 5] = NAN; }	/* D+1 파일의 비유한 한 벡터 (예전 days= 경로는 1120 만 읽는다) */
			mcs_rec(3, 0, 0, 8, NULL, NULL, xs);
			fs_close(&wf);
			anchor_line(d2, 0, 3, 0, SC_B_HUB + (k == 1 ? 50 : 55) * 60000LL, 1);
			if (k == 2) { for (int j = 1; j <= 1030; j++) { anchor_line(d2, (uint32_t)j, 99, j, j, 1); } }	/* 앵커 상한(1024) 넘김 → anchor_over=7 */
		}
	}
	const uint32_t b1 = acc(UB_S, SC_B1_INC, "[]"), b2 = acc(UB_S, SC_B_INC, SC_B_EXC);
	const uint32_t b3 = add_gate("normal_gate", UB_S, "reject", SC_B1_INC, "[]", true);
	const uint32_t b4 = add_gate("normal_gate", UB_S, "accept", SC_B1_INC, "[]", false);
	{
		uint32_t pick = 0;
		CHECK(!hubml_gate_latest(UB_S, &pick) && pick == b2, "자동 선택 = 가장 최근 accept+sig (관문 %u·%u·%u·%u 중 %u, reject %u · sig 없음 %u 는 건너뜀)",
		      b1, b2, b3, b4, pick, b3, b4);
		char cmd[256], want[120], vw[32];
		run("ml train uid=" UB_S);
		printk("  %s\n", out);
		snprintf(want, sizeof want, " gate=%u unanchored=%d excluded=%d outside=%d anchor_over=7", b2, SC_B_UNANCH, SC_B_EXCLUDED, SC_B_OUTSIDE);
		snprintf(vw, sizeof vw, "vectors=%d ", SC_B_KEPT);
		CHECK(strstr(out, "HUBML/1 train uid=" UB_S) && strstr(out, "rc=0") && strstr(out, "model_id=" UB_S ":1") && strstr(out, vw) && strstr(out, want),
		      "ml train uid=B (gate 없음) → 자동으로 관문 %u 로 학습, 응답 끝 %s", b2, want);
		CHECK(strstr(out, "train=2412 hold=603") != NULL, "n_total=3015 에서 학습 2412 · 평가 603 (패스·평가가 같은 걸러진 흐름)");
		static struct fnd fd;
		snprintf(cmd, sizeof cmd, "\"gate_seq\":%u,", b2);
		const uint32_t sq = find_line("\"kind\":\"dataset\"", cmd, &fd);
		snprintf(cmd, sizeof cmd, "\"counts\":{\"vectors\":3015,\"bad_quality\":1,\"dup\":0,\"gaps\":0,\"excluded\":600,\"outside\":400,\"unanchored\":16}");
		CHECK(sq && strstr(fd.line, cmd) && strstr(fd.line, "\"days\":[\"" SC_B_DAY "\"]"), "dataset: counts · days (자동 선택 경로)");
		snprintf(cmd, sizeof cmd, "\"gate_ds_id\":\"ds-" UB_S "-20261110-20261111-1\"");
		CHECK(strstr(fd.line, cmd) != NULL, "dataset: gate_ds_id = 관문의 ds_id");
		static const char bd[1][9] = { SC_B_DAY };
		char mk[65], mw[100];
		merkle_of(UB_S, bd, 1, mk);
		snprintf(mw, sizeof mw, "\"merkle\":\"%s\"", mk);
		CHECK(strstr(fd.line, mw) != NULL, "dataset merkle = include 날짜(1120)의 s1.mcs 만 (읽었지만 D+1·D+2 넘침 파일 1121·1122 는 해시 안 함)");
		snprintf(cmd, sizeof cmd, "\"gate_seq\":%u,\"schema_hash\"", b2);
		CHECK(find_line("\"kind\":\"model\"", cmd, &fd) != 0 && strstr(fd.line, "\"train_ds\":\"ds-" UB_S "-20261120-20261120-"),
		      "model 사건: train_ds 바로 뒤에 gate_seq=%u", b2);
		run("ml model ls uid=" UB_S);
		CHECK(strstr(out, "alias=challenger model_id=" UB_S ":1") != NULL, "model 이 등록되고 challenger 로 제안됨");
		/* 관문 없는 예전 경로 (days=) */
		run("ml train uid=" UB_S " days=" SC_B_DAY);
		printk("  %s\n", out);
		char vw2[32];
		snprintf(vw2, sizeof vw2, "vectors=%d ", SC_B_ALL);
		CHECK(strstr(out, "rc=0") && strstr(out, vw2) && !strstr(out, " gate="), "days= 경로 → 앵커·관문 없이 quality 0 전부 (%d), 응답 끝에 gate= 없음", SC_B_ALL);
		char dsid[100] = "";
		char cmd2[256];
		const char *d = strstr(out, "ds_id=");
		if (d) { sscanf(d + 6, "%99s", dsid); }
		snprintf(cmd2, sizeof cmd2, "\"ds_id\":\"%s\"", dsid);
		find_line("\"kind\":\"dataset\"", cmd2, &fd);
		char leg[128];
		snprintf(leg, sizeof leg, "\"counts\":{\"vectors\":%d,\"bad_quality\":0,\"dup\":0,\"gaps\":0},\"merkle\":\"", SC_B_ALL);
		CHECK(strstr(fd.line, leg) && !strstr(fd.line, "gate") && !strstr(fd.line, "excluded") && strstr(fd.line, "\"regimes\":["),
		      "예전 경로 dataset 줄에는 관문 칸이 없고 counts 형식이 그대로");
		snprintf(cmd, sizeof cmd, "\"train_ds\":\"%s\",\"schema_hash\"", dsid);
		const uint32_t mq = find_line("\"kind\":\"model\"", cmd, &fd);
		CHECK(mq != 0 && !strstr(fd.line, "gate_seq"), "예전 경로 model 줄: train_ds 바로 뒤 schema_hash, gate_seq 없음");
	}

	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	CHECK(h.ok, "원장 사슬 정상 (%u 줄)", h.next_seq - 1);
	size_t unused = 0;
	k_thread_stack_space_get(cmd_thr, &unused);
	CHECK(unused >= 1024, "명령을 친 스레드(4 KiB, 실기 셸과 같음) 남은 스택 %zu B ≥ 1 KiB", unused);
	printk(ok ? "=== Lm11 PASS\n" : "=== Lm11 FAIL\n");
	return 0;
}
