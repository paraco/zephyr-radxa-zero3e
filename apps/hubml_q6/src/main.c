/*
 * hubml_q6 (Lm6) — Studio 가 USB 셸로 할 일을 그대로 흉내 낸다.
 *  ml time set → ml rx(HELLO) → tx 줄의 HELLO_ACK 에서 sid → ml rx(FEAT 20 · HB 4) → ml commit → tx 줄의 ACK 풀기 →
 *  ml status --kv · ml dev ls --kv · ml ds ls uid= · (모델 등록 · champion) · ml ledger since=1 max=50 →
 *  원장 줄 b64 를 풀어 사슬을 다시 계산하고 ledger_done head 와 대조.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/byteorder.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/hubml.h>
#include <hubml/sha256.h>

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)
#define UID 0x4d50444d000000c1ull

static const struct shell *sh;
static char out[65536];

static const char *run(const char *cmd)
{
	shell_backend_dummy_clear_output(sh);
	shell_execute_cmd(sh, cmd);
	size_t n;
	const char *o = shell_backend_dummy_get_output(sh, &n);
	memcpy(out, o, MIN(n, sizeof out - 1));
	out[MIN(n, sizeof out - 1)] = 0;
	return out;
}

static void rx(const uint8_t *d, size_t n)
{
	static char cmd[2048];
	size_t ol = 0;
	int k = snprintf(cmd, sizeof cmd, "ml rx b64=");
	base64_encode((uint8_t *)cmd + k, sizeof cmd - k, &ol, d, n);
	cmd[k + ol] = 0;
	run(cmd);
}

/* 출력에서 "HUBML/1 tx addr=1 b64=" 줄들을 풀어 type 이 맞는 첫 데이터그램 본문을 돌려준다 */
static int find_tx(uint8_t type, struct mcol_hdr *h, uint8_t *pl)
{
	static uint8_t d[MCOL_MAX_DGRAM];
	const char *p = out;
	while ((p = strstr(p, "HUBML/1 tx addr=")) != NULL) {
		const char *b = strstr(p, "b64=");
		if (!b) { break; }
		b += 4;
		const char *e = b;
		while (*e && *e != '\r' && *e != '\n') { e++; }
		size_t ol = 0;
		const uint8_t *q;
		if (!base64_decode(d, sizeof d, &ol, (const uint8_t *)b, (size_t)(e - b)) && !mcol_decode(d, ol, h, &q) &&
		    h->type == type) {
			memcpy(pl, q, h->len);
			return 0;
		}
		p = e;
	}
	return -ENOENT;
}

int main(void)
{
	printk("=== Lm6 ml 셸 · USB 운반 (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm6 FAIL\n"); return 0; }
	fs_mkdir("/RAM:/ml");
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);

	run("ml time set 1794182400000");
	CHECK(strstr(out, "HUBML/1 time time_ms=1794182400") != NULL, "ml time set → %.60s", out);

	uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM];
	struct mcol_hello hm = { .uid = UID, .fw = 0x0102, .contract = 34, .nfeat = 34, .sensor_hash = 1, .schema_hash = 0x4d463334 };
	mcol_hello_put(pl, &hm);
	struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN, .boot_id = 2 };
	rx(d, (size_t)mcol_encode(d, sizeof d, &h, pl));
	struct mcol_hdr r;
	uint8_t rp[MCOL_MAX_PAYLOAD];
	const int ha = find_tx(MCOL_T_HELLO_ACK, &r, rp);
	const uint16_t sid = ha ? 0 : sys_get_le16(rp);
	CHECK(!ha && sid == 1, "ml rx(HELLO) → tx 줄 HELLO_ACK sid=%u, 허브 시각 %llu", sid, ha ? 0ull : (unsigned long long)sys_get_le64(rp + 4));

	static float xs[8 * 34];
	for (int i = 0; i < 8 * 34; i++) { xs[i] = 1.0f + 0.01f * (float)(i % 34); }
	for (uint32_t sq = 0; sq < 20; sq++) {
		const int L = mcol_feat_put(pl, sizeof pl, 34, 8, xs, NULL);
		struct mcol_hdr f = { .type = MCOL_T_FEAT, .edge_sid = sid, .stream = MCOL_S_FEAT, .seq = sq, .len = (uint16_t)L };
		rx(d, (size_t)mcol_encode(d, sizeof d, &f, pl));
		if (sq % 5 == 0) {
			uint8_t hb[24] = { 0 };
			struct mcol_hdr b = { .type = MCOL_T_HB, .edge_sid = sid, .stream = MCOL_S_HB, .seq = sq / 5, .len = sizeof hb };
			rx(d, (size_t)mcol_encode(d, sizeof d, &b, hb));
		}
	}
	run("ml commit");
	int ack_feat = -1, ack_hb = -1;
	if (!find_tx(MCOL_T_ACK, &r, rp)) {
		for (int o = 0; o + MCOL_ACK_ENTRY_LEN <= r.len; o += MCOL_ACK_ENTRY_LEN) {
			if (rp[o] == MCOL_S_FEAT) { ack_feat = (int)sys_get_le32(rp + o + 1); }
			if (rp[o] == MCOL_S_HB) { ack_hb = (int)sys_get_le32(rp + o + 1); }
		}
	}
	/* 배경 스레드가 먼저 기록·ACK 했을 수도 있다 — 그러면 commit 출력에는 ACK 가 없을 수 있다 */
	CHECK(strstr(out, "HUBML/1 commit rc=0") != NULL, "ml commit → rc=0 (tx ACK feat next=%d hb next=%d)", ack_feat, ack_hb);

	run("ml status --kv");
	printk("  %s", out);
	CHECK(strstr(out, "HUBML/1 status hub=4d4257") && strstr(out, "edges=1") && strstr(out, "accepted=24") &&
	      strstr(out, "ledger_ok=1") && strstr(out, "time_set=1"), "ml status --kv");
	run("ml dev ls --kv");
	printk("  %s", out);
	CHECK(strstr(out, "uid=4d50444d000000c1") && strstr(out, "schema=4d463334") && strstr(out, "state=learning") &&
	      strstr(out, "next_feat=20") && strstr(out, "next_hb=4") && strstr(out, "HUBML/1 dev_done n=1"), "ml dev ls --kv");
	run("ml ds ls uid=4d50444d000000c1 --kv");
	printk("  %s", out);
	CHECK(strstr(out, "stream=feat records=20") && strstr(out, "stream=hb records=4") && strstr(out, "ds_done uid=4d50444d000000c1 n=2"),
	      "ml ds ls uid= → feat 20 · hb 4");
	run("ml ds ls uid=zz");
	CHECK(strstr(out, "HUBML/1 err code=bad_arg") != NULL, "잘못된 uid → err");

	char mid[25];
	hubml_model_register(UID, k3, sizeof k3, "hub", "\"generation\":0", mid);
	hubml_alias_set(UID, HUBML_A_CHAMPION, mid, "promote", "studio:k1",
			"{\"alg\":\"ed25519\",\"key\":\"k1\",\"over\":\"sha256\",\"b64\":\"UNVERIFIED\"}");
	run("ml dev ls --kv");
	CHECK(strstr(out, "state=baselined") && strstr(out, "champion=4d50444d000000c1:1"), "champion 뒤 state=baselined");

	run("ml ledger since=1 max=50");
	/* 줄들을 풀어 사슬을 다시 계산 */
	char prev[65];
	memset(prev, '0', 64);
	prev[64] = 0;
	int nl = 0, chain_ok = 1;
	const char *p = out;
	static uint8_t line[3100];
	while ((p = strstr(p, "HUBML/1 ledger seq=")) != NULL) {
		const char *b = strstr(p, "b64=") + 4, *e = b;
		while (*e && *e != '\r' && *e != '\n') { e++; }
		size_t ol = 0;
		base64_decode(line, sizeof line - 1, &ol, (const uint8_t *)b, (size_t)(e - b));
		line[ol] = 0;
		char want[80];
		snprintf(want, sizeof want, "\"prev\":\"%s\"", prev);
		if (!strstr((char *)line, want)) { chain_ok = 0; }
		uint8_t dg[32];
		struct sha256 s;
		sha256_init(&s); sha256_update(&s, line, ol); sha256_final(&s, dg);
		for (int i = 0; i < 32; i++) { snprintf(prev + 2 * i, 3, "%02x", dg[i]); }
		nl++;
		p = e;
	}
	char done[160];
	snprintf(done, sizeof done, "ledger_done last=%d head=%s total=%d ok=1", nl, prev, nl);
	CHECK(nl == 2 && chain_ok && strstr(out, done), "ml ledger → %d 줄 b64 · 사슬 다시 계산 · ledger_done head 일치", nl);
	printk("%s", out);
	printk(ok ? "=== Lm6 PASS\n" : "=== Lm6 FAIL\n");
	return 0;
}
