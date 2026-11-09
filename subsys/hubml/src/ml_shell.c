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
 * 쓰기 명령(train · approve · deploy · model put)은 사용자 승인 뒤 붙인다.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
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

extern const struct shell *hubml_tx_shell;

#ifndef HUBML_SHELL_ROOT
#define HUBML_SHELL_ROOT "/0:/ml"
#endif
#ifndef HUBML_SHELL_HUB_ID
#define HUBML_SHELL_HUB_ID "-"
#endif
__weak const char *hubml_default_hub_id(void) { return HUBML_SHELL_HUB_ID; }

static int ready(const struct shell *sh)
{
	hubml_tx_shell = sh;
	if (hubml_started()) {
		return 0;
	}
	const int rc = hubml_start(HUBML_SHELL_ROOT, hubml_default_hub_id());
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
	struct hubml_stats s;
	struct hubml_ledger_head h;
	struct hubml_edge_info e[HUBML_MAX_EDGES];
	hubml_ingest_stats(&s);
	hubml_ledger_head(&h);
	const int ne = hubml_ingest_edges(e, HUBML_MAX_EDGES);
	struct fs_statvfs v;
	hubml_fs_lock();
	const int vr = fs_statvfs(hubml_root(), &v);
	hubml_fs_unlock();
	const unsigned long long free_kib = vr ? 0 : (unsigned long long)v.f_bfree * v.f_frsize / 1024;
	shell_print(sh, "HUBML/1 status hub=%s root=%s time_ms=%lld time_set=%d edges=%d rx=%u accepted=%u dup=%u bad=%u "
		    "qfull=%u commits=%u commit_err=%u max_commit_ms=%u queued=%zu ledger_next=%u ledger_ok=%d free_kib=%llu",
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
	hubml_time_set(strtoll(argv[1], NULL, 10));
	shell_print(sh, "HUBML/1 time time_ms=%lld time_set=1", (long long)hubml_time_now());
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_dev, SHELL_CMD_ARG(ls, NULL, "ls [--kv] — 장비 표", cmd_dev_ls, 1, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_ds, SHELL_CMD_ARG(ls, NULL, "ls uid=<hex16> [--kv] — 날짜·흐름별", cmd_ds_ls, 2, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml_time, SHELL_CMD_ARG(set, NULL, "set <epoch_ms> — 허브 UTC", cmd_time_set, 2, 0),
			       SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_ml,
	SHELL_CMD_ARG(status, NULL, "status [--kv] — 허브 ML 상태", cmd_status, 1, 1),
	SHELL_CMD(dev, &sub_ml_dev, "장비", NULL),
	SHELL_CMD(ds, &sub_ml_ds, "데이터 묶음", NULL),
	SHELL_CMD_ARG(ledger, NULL, "ledger since=<seq> max=<n> — 원장 줄", cmd_ledger, 1, 2),
	SHELL_CMD_ARG(rx, NULL, "rx b64=<MCOL> [addr=<n>] — USB 운반 수신", cmd_rx, 2, 1),
	SHELL_CMD(time, &sub_ml_time, "시각", NULL),
	SHELL_CMD(commit, NULL, "지금 기록·ACK (시험용)", cmd_commit),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ml, &sub_ml, "MoruLab 허브 ML (HUBML/1)", NULL);
