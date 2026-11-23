/*
 * hubml_q12 (Lm12) — 시범 운영(shadow) 점수 보고 규약 MCOL SCORE + `ml shadow ls` (QEMU 가짜 셸).
 *  1 mcol score put/get 왕복 · 깨진 길이(모자람·넘침·n=0·n=9) 거부 · 상위 비트(flags·rsv·vflags bit4~7)는 거부하지 않음
 *  2 Edge 참조 구현(edge.c hubml_edge_score_batch)이 만든 SCORE 본문 = Python 기준(tools/hubml_testdata.py --score-inc)이 만든 바이트
 *    (둘 다 채움 · champion 만 · challenger 만 · 둘 다 빈 슬롯 · quality≠0 벡터) · FEAT n 이 0·9, 계약 다름, 모자란 본문 → 오류
 *    (같은 입력·기대 바이트를 Cortex-M33 쪽 hubml_edge_q 도 대조한다)
 *  3 여러 날 FEAT+SCORE 를 수집 엔진(ingest)으로 넣고 — 같은 날 두 쌍 · champ 만 valid · champion 빈 묶음 · quality≠0 ·
 *    SCORE 중복 재전송 · EVT 형식 아닌 레코드 · 깨진 SCORE · NaN/inf + valid 비트 · 상위 비트 켜짐 · 벡터 수 동률 쌍 · 1e12 대 점수 ·
 *    쌍 34 개(32 초과) · 파일 꼬리가 잘린 날(err=) — `ml shadow ls` 출력을
 *    Python 기준(shadow_lines)의 줄과 글자 그대로 대조
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
#include <hubml/hubml.h>
#include <hubml/edge.h>
#include <hubml/sha256.h>
#include "score_scenario.inc"
#include "score_edge_expected.inc"

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)
#define ROOT "/RAM:/ml"
#define NF 34
#define UID 0x4d50444d000000c1ull
#define T0 1794312000000LL	/* 2026-11-10 12:00 UTC */

static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static const uint32_t PV[][36] = {
#include "pv_vectors.inc"
};
static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

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

/* out 에서 "HUBML/1 shadow" 로 시작하는 줄을 차례로 want[] 와 글자 그대로 비교 (\r 은 뺀다). 틀린 줄은 찍는다 */
static bool lines_match(const char *o, const char *const *want, int nw)
{
	int k = 0;
	bool same = true;
	const char *p = o;
	while (*p) {
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		const char *q = p;
		p = e ? e + 1 : p + l;
		while (l && q[l - 1] == '\r') { l--; }
		const char *hit = NULL;
		for (size_t i = 0; i + 14 <= l; i++) {
			if (!strncmp(q + i, "HUBML/1 shadow", 14)) { hit = q + i; break; }
		}
		if (!hit) { continue; }
		l -= (size_t)(hit - q);
		static char exp[640];
		if (k < nw && strstr(want[k], "err=%d")) {
			snprintf(exp, sizeof exp, want[k], -EBADMSG);	/* 잘린 꼬리 — 오류 값은 libc 마다 달라 시험이 채운다 */
		} else if (k < nw) {
			snprintf(exp, sizeof exp, "%s", want[k]);
		} else {
			snprintf(exp, sizeof exp, "(없음)");
		}
		if (k >= nw || strlen(exp) != l || strncmp(hit, exp, l)) {
			printk("  줄 %d 다름\n   실제: %.*s\n   기대: %s\n", k, (int)l, hit, exp);
			same = false;
		}
		k++;
	}
	if (k != nw) { printk("  줄 수 %d (기대 %d)\n", k, nw); same = false; }
	return same;
}

/* ── Edge 쪽: 모델 슬롯에 kind 3 넣기 (Lm3 와 같은 BEGIN · CHUNK · END) ── */
static struct hubml_edge E;
static void edge_load(int slot, uint32_t num, uint16_t xfer)
{
	uint8_t b[MCOL_CMD_HDR + 44 + 4 + MCOL_CHUNK], r[MCOL_CMD_ACK_LEN], d[32];
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, k3, sizeof k3);
	sha256_final(&s, d);
	b[0] = MCOL_OP_BEGIN; b[1] = 0; sys_put_le16(xfer, b + 2);
	memset(b + 4, 0, 44);
	b[4] = (uint8_t)slot; b[5] = 3;
	sys_put_le32(sizeof k3, b + 8);
	sys_put_le32(num, b + 12);
	memcpy(b + 16, d, 32);
	hubml_edge_on_cmd(&E, b, MCOL_CMD_HDR + 44, r);
	for (uint32_t off = 0; off < sizeof k3; off += MCOL_CHUNK) {
		const uint32_t n = MIN((uint32_t)MCOL_CHUNK, (uint32_t)sizeof k3 - off);
		b[0] = MCOL_OP_CHUNK;
		sys_put_le32(off, b + 4);
		memcpy(b + 8, k3 + off, n);
		hubml_edge_on_cmd(&E, b, (uint16_t)(8 + n), r);
	}
	b[0] = MCOL_OP_END;
	hubml_edge_on_cmd(&E, b, MCOL_CMD_HDR, r);
}

static void test_score_codec(void)
{
	struct mcol_score_ent en[8], g;
	for (int i = 0; i < 8; i++) {
		en[i] = (struct mcol_score_ent){ .champ = 0.25f * (float)i, .chal = 1.5f + (float)i, .vflags = (uint8_t)(i | 0xf0) };
	}
	struct mcol_score_hdr h = { .feat_seq = 0xfffffffe, .n = 8, .champ_num = 7, .chal_num = 0x01020304 }, g0;
	uint8_t pl[MCOL_MAX_PAYLOAD];
	const int L = mcol_score_put(pl, sizeof pl, &h, en);
	CHECK(L == 16 + 9 * 8, "mcol_score_put 길이 %d = 16 + 9n", L);
	CHECK(MCOL_T_SCORE == 11 && MCOL_S_EVT == 3, "MCOL_T_SCORE = %d · 흐름 EVT = %d", MCOL_T_SCORE, MCOL_S_EVT);
	CHECK(sys_get_le32(pl) == 0xfffffffe && pl[4] == 8 && pl[5] == 0 && sys_get_le16(pl + 6) == 0 &&
	      sys_get_le32(pl + 8) == 7 && sys_get_le32(pl + 12) == 0x01020304, "머리 16 B 배치 (feat_seq · n · flags · rsv · champ_num · chal_num)");
	bool same = !mcol_score_get(pl, (uint16_t)L, &g0) && g0.feat_seq == h.feat_seq && g0.n == 8 && g0.champ_num == 7 &&
		    g0.chal_num == 0x01020304 && g0.flags == 0;
	for (int i = 0; i < 8 && same; i++) {
		mcol_score_ent_get(pl, i, &g);
		same = g.champ == en[i].champ && g.chal == en[i].chal && g.vflags == (en[i].vflags & 0x0f);
	}
	CHECK(same, "put → get 왕복 (점수·vflags 같음, put 은 vflags 상위 비트를 쓰지 않음)");
	CHECK(mcol_score_get(pl, (uint16_t)(L - 1), &g0) == -EBADMSG, "길이 1 모자라면 거부");
	CHECK(mcol_score_get(pl, (uint16_t)(L + 1), &g0) == -EBADMSG, "길이 1 넘치면 거부");
	CHECK(mcol_score_get(pl, 15, &g0) == -EBADMSG, "머리보다 짧으면 거부");
	pl[4] = 0;
	CHECK(mcol_score_get(pl, 16, &g0) == -EBADMSG, "n=0 거부");
	pl[4] = 9;
	CHECK(mcol_score_get(pl, 16 + 81, &g0) == -EBADMSG, "n=9 거부 (길이가 16+9n 이어도)");
	pl[4] = 8;
	pl[5] = 0xff;
	sys_put_le16(0xabcd, pl + 6);
	pl[16 + 8] |= 0xf0;
	CHECK(mcol_score_get(pl, (uint16_t)L, &g0) == 0 && g0.flags == 0xff, "flags·rsv·vflags 상위 비트가 켜져 있어도 거부하지 않음");
	h.n = 0;
	CHECK(mcol_score_put(pl, sizeof pl, &h, en) == -EINVAL, "put n=0 거부");
	h.n = 9;
	CHECK(mcol_score_put(pl, sizeof pl, &h, en) == -EINVAL, "put n=9 거부");
	h.n = 8;
	CHECK(mcol_score_put(pl, 87, &h, en) == -EMSGSIZE, "put 버퍼 모자람 거부");
	/* 데이터그램에 실어도 왕복 (EVT 흐름) */
	uint8_t d[MCOL_MAX_DGRAM];
	struct mcol_hdr dh = { .type = MCOL_T_SCORE, .edge_sid = 1, .stream = MCOL_S_EVT, .seq = 5, .edge_ms = 9000, .len = (uint16_t)mcol_score_put(pl, sizeof pl, &h, en), .boot_id = 2 }, dd;
	const uint8_t *dp;
	const int m = mcol_encode(d, sizeof d, &dh, pl);
	CHECK(m == 24 + 88 + 4 && !mcol_decode(d, (size_t)m, &dd, &dp) && dd.type == MCOL_T_SCORE && dd.stream == MCOL_S_EVT && !mcol_score_get(dp, dd.len, &g0),
	      "MCOL 데이터그램(EVT 흐름)에 실어 왕복 %d B", m);
}

static void test_edge_batch(void)
{
	static float xs[8 * NF];
	uint8_t pl[MCOL_MAX_PAYLOAD], got[MCOL_SCORE_HDR + 9 * 8];
	int nok = 0;
	for (int c = 0; c < (int)ARRAY_SIZE(ES_CASE); c++) {
		const struct es_case *k = &ES_CASE[c];
		hubml_edge_init(&E, NF);
		if (k->slots & 1) { edge_load(0, k->champ_num, 1); }
		if (k->slots & 2) { edge_load(1, k->chal_num, 2); }
		for (int i = 0; i < k->n; i++) {
			for (int j = 0; j < NF; j++) { xs[i * NF + j] = f_of(PV[k->first + i][j]); }
		}
		const int fl = mcol_feat_put(pl, sizeof pl, NF, k->n, xs, k->q);
		const int n = hubml_edge_score_batch(&E, pl, (uint16_t)fl, k->feat_seq, got, sizeof got);
		const bool same = n == k->len && !memcmp(got, k->body, (size_t)k->len);
		nok += same;
		CHECK(same, "Edge SCORE 본문 = Python 기준 (feat_seq %u · 슬롯 %u · n=%d · %d B)", (unsigned)k->feat_seq, k->slots, k->n, n);
	}
	CHECK(nok == (int)ARRAY_SIZE(ES_CASE), "Edge SCORE 기대 바이트 %d/%d 일치", nok, (int)ARRAY_SIZE(ES_CASE));
	/* 오류 */
	hubml_edge_init(&E, NF);
	edge_load(0, 1, 1);
	for (int i = 0; i < 8; i++) { for (int j = 0; j < NF; j++) { xs[i * NF + j] = f_of(PV[i][j]); } }
	const int fl = mcol_feat_put(pl, sizeof pl, NF, 8, xs, NULL);
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)fl, 1, got, sizeof got) == 16 + 72, "정상 8 벡터는 88 B");
	pl[0] = 0;
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)fl, 1, got, sizeof got) < 0, "FEAT n=0 → 오류");
	pl[0] = 9;
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)fl, 1, got, sizeof got) < 0, "FEAT n=9 → 오류");
	pl[0] = 8;
	pl[1] = 33;
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)fl, 1, got, sizeof got) < 0, "FEAT 계약 다름 → 오류");
	pl[1] = NF;
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)(fl - 1), 1, got, sizeof got) < 0, "FEAT 본문이 모자람 → 오류");
	CHECK(hubml_edge_score_batch(&E, pl, (uint16_t)fl, 1, got, 87) < 0, "SCORE 출력 버퍼 모자람 → 오류");
	CHECK(hubml_edge_score_batch(&E, pl, 3, 1, got, sizeof got) < 0, "FEAT 머리보다 짧음 → 오류");
}

/* 그날 EVT 파일의 마지막 레코드 꼬리를 5 B 자른다 — 전원이 나가 마지막 레코드가 반만 쓰인 것처럼 */
static void truncate_tail(const char *day)
{
	char p[96];
	struct fs_dirent de;
	struct fs_file_t f;
	snprintf(p, sizeof p, ROOT "/ds/%s/%s/s%d.mcs", "4d50444d000000c1", day, MCOL_S_EVT);
	if (fs_stat(p, &de)) { printk("stat %s 실패\n", p); return; }
	fs_file_t_init(&f);
	if (fs_open(&f, p, FS_O_RDWR)) { printk("open %s 실패\n", p); return; }
	fs_truncate(&f, de.size - 5);
	fs_close(&f);
}

static void send_rec(uint8_t type, uint8_t stream, uint32_t seq, const uint8_t *body, uint16_t len, uint64_t ms)
{
	static uint8_t d[MCOL_MAX_DGRAM];
	struct mcol_hdr h = { .type = type, .edge_sid = 1, .stream = stream, .seq = seq, .edge_ms = ms, .len = len, .boot_id = 1 };
	hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &h, body));
}

int main(void)
{
	printk("=== Lm12 ml shadow ls (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm12 FAIL\n"); return 0; }
	fs_mkdir(ROOT);
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);

	test_score_codec();
	test_edge_batch();

	/* ── 수집: 날마다 시각을 맞추고 FEAT + SCORE 를 넣는다 ── */
	char cmd[160];
	snprintf(cmd, sizeof cmd, "ml time set %lld", (long long)T0);
	run(cmd);
	{
		uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM];
		struct mcol_hello hm = { .uid = UID, .fw = 0x0100, .contract = NF, .nfeat = NF, .schema_hash = 0x4d463334 };
		mcol_hello_put(pl, &hm);
		struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN };
		hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &h, pl));
	}
	static float xs[8 * NF];
	static uint8_t fpl[MCOL_MAX_PAYLOAD];
	int cur = 0, resends = 0;
	for (int k = 0; k < SS_NREC; k++) {
		const struct ss_rec *r = &SS_REC[k];
		if (r->day != cur) {
			hubml_ingest_commit();
			if (cur == SS_TORN_DAY) { truncate_tail(SS_DAYS[cur]); }
			cur = r->day;
			snprintf(cmd, sizeof cmd, "ml time set %lld", (long long)(T0 + 86400000LL * cur));
			run(cmd);
		}
		for (int i = 0; i < 8; i++) {
			for (int j = 0; j < NF; j++) { xs[i * NF + j] = f_of(PV[(k * 8 + i) % 64][j]); }
		}
		const int fl = mcol_feat_put(fpl, sizeof fpl, NF, 8, xs, NULL);
		send_rec(MCOL_T_FEAT, MCOL_S_FEAT, (uint32_t)k, fpl, (uint16_t)fl, (uint64_t)k * 8000);
		for (int t = 0; t <= r->resend; t++) {
			send_rec(r->type, MCOL_S_EVT, (uint32_t)k, r->body, r->len, (uint64_t)k * 8000);	/* 재전송 = 같은 seq */
		}
		resends += r->resend;
	}
	hubml_ingest_commit();
	struct hubml_stats st;
	hubml_ingest_stats(&st);
	CHECK(st.dup >= (uint32_t)resends && st.bad == 0, "SCORE 재전송 %d 번 → ingest 중복 %u 로 걸러짐 · 깨진 데이터그램 %u (SCORE 본문이 깨져도 데이터그램은 정상 — EVT 흐름의 알 수 없는 type 도 받음)",
	      resends, st.dup, st.bad);
	int tot = 0;
	bool perday = true;
	for (int d = 0; d < (int)ARRAY_SIZE(SS_DAYS); d++) {
		const int n = hubml_store_scan(ROOT, UID, SS_DAYS[d], MCOL_S_EVT, NULL, NULL, NULL);
		if (d == SS_TORN_DAY) {
			perday = perday && n == -EBADMSG;	/* 꼬리가 잘린 날 — 오류 */
			continue;
		}
		tot += n;
		perday = perday && n == SS_DAY_NREC[d];
	}
	CHECK(perday, "EVT 흐름 저장 레코드가 날마다 보낸 서로 다른 레코드 수와 같음 (합 %d, 잘린 날은 -EBADMSG)", tot);

	run("ml shadow ls uid=" SS_UID_S " days=" SS_DAYS_ARG " --kv");
	const bool m1 = lines_match(out, SS_LINES, ARRAY_SIZE(SS_LINES));
	CHECK(m1, "ml shadow ls 출력 %d 줄이 Python 기준과 글자 그대로 같음", (int)ARRAY_SIZE(SS_LINES));
	run("ml shadow ls uid=" SS_UID_S " days=" SS_DAYS_ARG);
	CHECK(lines_match(out, SS_LINES, ARRAY_SIZE(SS_LINES)), "--kv 없이도 같음");
	run("ml shadow ls uid=" SS_UNK_ARG " days=" SS_UNK_DAY);
	CHECK(lines_match(out, SS_UNK_LINES, ARRAY_SIZE(SS_UNK_LINES)), "장비가 없으면 그날 n=0 줄");
	run("ml shadow ls uid=" SS_UID_S);
	CHECK(strstr(out, "HUBML/1 shadow uid") == NULL, "days 없으면 요약 줄을 내지 않음");
	run("ml shadow ls uid=1234 days=20261110");
	CHECK(strstr(out, "HUBML/1 err code=bad_arg") != NULL, "uid 모양이 틀리면 bad_arg");

	size_t unused = 0;
	k_thread_stack_space_get(cmd_thr, &unused);
	CHECK(unused >= 1024, "명령을 친 스레드(4 KiB) 남은 스택 %zu B ≥ 1 KiB", unused);
	printk(ok ? "=== Lm12 PASS\n" : "=== Lm12 FAIL\n");
	return 0;
}
