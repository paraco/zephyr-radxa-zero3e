/*
 * hubml_q4 (Lm4) — 원장 시험 (QEMU A55, RAM 디스크).
 *  모델 등록(관문: 깨진 파일 거부) · 별칭 v0/challenger/champion(서명 없으면 거부)/승격/되돌리기 ·
 *  없는 모델로 옮기기 거부 · 재시작 뒤 별칭 복원 · 반쪽 꼬리 잘라 내기 · 중간 변조 감지(덧붙이기 막힘) ·
 *  since/max 읽기 · 모든 줄을 찍어 호스트 tools/ledger_verify.py 가 해시 사슬을 다시 계산한다.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/mcol.h>
#include <zephyr/sys/base64.h>
#include <math.h>

#define ROOT "/RAM:/ml"
#define UID 0x4d50444d00000001ull
static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static const uint8_t k1[] = {
#include "golden_kind1.xmb.inc"
};
static uint8_t bad[sizeof k3];
static bool ok = true;
static const char *SIG = "{\"alg\":\"ed25519\",\"key\":\"k1\",\"over\":\"sha256\",\"b64\":\"UNVERIFIED\"}";

#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)

static int lines;
static uint32_t first_seq;
static void rd_cb(uint32_t seq, const char *l, size_t n, void *u) { ARG_UNUSED(l); ARG_UNUSED(n); ARG_UNUSED(u); if (!lines) { first_seq = seq; } lines++; }
static void dump_cb(uint32_t seq, const char *l, size_t n, void *u) { ARG_UNUSED(seq); ARG_UNUSED(u); printk("LEDGER %.*s\n", (int)n, l); }

static int file_patch(const char *p, off_t at, const void *b, size_t n, bool append)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, p, append ? (FS_O_WRITE | FS_O_APPEND) : FS_O_RDWR);
	if (rc) { return rc; }
	if (!append) { fs_seek(&f, at, FS_SEEK_SET); }
	rc = fs_write(&f, b, n) == (ssize_t)n ? 0 : -EIO;
	fs_close(&f);
	return rc;
}

/* ── 허브 root 표본: 장비 2 대 × FEAT 50 데이터그램(400 벡터) + HB 를 수집 엔진으로 쌓는다 ── */
static int nop_send(uint32_t a, const uint8_t *b, size_t n) { ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(n); return 0; }
static const struct hubml_xport xp = { .send = nop_send };
static void sample_data(void)
{
	hubml_ingest_init(ROOT, &xp);
	static uint8_t pl[MCOL_MAX_PAYLOAD], out[MCOL_MAX_DGRAM];
	static float xs[8 * 34];
	for (int e = 0; e < 2; e++) {
		const uint64_t uid = UID + (uint64_t)e;
		struct mcol_hello hm = { .uid = uid, .fw = 0x0100, .contract = 34, .nfeat = 34,
					 .sensor_hash = 0x5e5a0001, .schema_hash = 0x4d463334 };
		mcol_hello_put(pl, &hm);
		struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN, .boot_id = 1 };
		int n = mcol_encode(out, sizeof out, &h, pl);
		hubml_ingest_rx((uint32_t)e + 1, out, (size_t)n);
		for (uint32_t sq = 0; sq < 50; sq++) {
			for (int i = 0; i < 8 * 34; i++) {
				const uint32_t t = sq * 8 + (uint32_t)i / 34;
				xs[i] = (float)(1.0 + 0.1 * (i % 34) / 34.0 + 0.05 * sin(0.37 * t + i % 34 + e));
			}
			for (int i = 0; i < 8; i++) { xs[i * 34 + 27] = e ? 0.8f : 0.4f; }
			const int L = mcol_feat_put(pl, sizeof pl, 34, 8, xs, NULL);
			struct mcol_hdr f = { .type = MCOL_T_FEAT, .edge_sid = (uint16_t)(e + 1), .stream = MCOL_S_FEAT,
					      .seq = sq, .edge_ms = sq * 8000ull, .len = (uint16_t)L, .boot_id = 1 };
			n = mcol_encode(out, sizeof out, &f, pl);
			hubml_ingest_rx((uint32_t)e + 1, out, (size_t)n);
			if (sq % 5 == 0) {
				uint8_t hb[24] = { 0 };
				struct mcol_hdr b = { .type = MCOL_T_HB, .edge_sid = (uint16_t)(e + 1), .stream = MCOL_S_HB,
						      .seq = sq / 5, .edge_ms = sq * 8000ull, .len = sizeof hb, .boot_id = 1 };
				n = mcol_encode(out, sizeof out, &b, hb);
				hubml_ingest_rx((uint32_t)e + 1, out, (size_t)n);
			}
		}
	}
	hubml_ingest_commit();
}

/* root 아래 모든 파일을 base64 로 찍는다 → 호스트 tools/hubroot_extract.py 가 폴더로 되살린다 */
static void dump_tree(const char *dir)
{
	struct fs_dir_t d;
	fs_dir_t_init(&d);
	if (fs_opendir(&d, dir)) { return; }
	struct fs_dirent e;
	while (fs_readdir(&d, &e) == 0 && e.name[0]) {
		char p[128];
		snprintf(p, sizeof p, "%s/%s", dir, e.name);
		if (e.type == FS_DIR_ENTRY_DIR) {
			dump_tree(p);
			continue;
		}
		struct fs_file_t f;
		fs_file_t_init(&f);
		if (fs_open(&f, p, FS_O_READ)) { continue; }
		printk("FILE_BEGIN %s %zu\n", p + strlen(ROOT) + 1, e.size);
		static uint8_t b[57 * 64];
		ssize_t n;
		while ((n = fs_read(&f, b, sizeof b)) > 0) {
			for (ssize_t o = 0; o < n; o += 57) {
				char line[80];
				size_t ol = 0;
				base64_encode((uint8_t *)line, sizeof line, &ol, b + o, (size_t)MIN(57, n - o));
				line[ol] = 0;
				printk("FILE64 %s\n", line);
			}
		}
		fs_close(&f);
		printk("FILE_END\n");
	}
	fs_closedir(&d);
}

int main(void)
{
	printk("=== Lm4 hubml 원장 (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm4 FAIL\n"); return 0; }
	fs_mkdir(ROOT);
	hubml_time_set(1794182400000LL);
	sample_data();
	hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
	struct hubml_ledger_head h;

	char id3[HUBML_MODEL_ID_LEN + 1], id1[HUBML_MODEL_ID_LEN + 1], idb[HUBML_MODEL_ID_LEN + 1];
	const char *DS = "\"ds_id\":\"ds-4d50444d00000001-20261109-20261109-1\",\"uid\":\"4d50444d00000001\",\"days\":[\"20261109\"],"
			 "\"contract\":34,\"schema_hash\":\"4d463334\",\"counts\":{\"vectors\":86400,\"bad_quality\":0,\"dup\":0,\"gaps\":0},"
			 "\"merkle\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"regimes\":[0,36000,0,32922]";
	CHECK(hubml_ledger_append("dataset", "hub", DS, NULL, NULL) == 0, "dataset 사건");
	CHECK(hubml_model_register(UID, k3, sizeof k3, "hub", "\"train_ds\":\"ds-4d50444d00000001-20261109-20261109-1\",\"schema_hash\":\"4d463334\",\"params\":{\"q\":0.995,\"holdout\":0.2,\"nreg\":4,\"reg_feat\":\"load_ratio\"},\"generation\":0", id3) == 0, "kind 3 등록 %s", id3);
	CHECK(hubml_model_register(UID, k1, sizeof k1, "hub", "\"generation\":0", id1) == 0, "kind 1 등록 %s", id1);
	CHECK(!strcmp(id3, "4d50444d00000001:1") && !strcmp(id1, "4d50444d00000001:2"), "model_id = <uid>:<n> (장비별 1 부터)");
	memcpy(bad, k3, sizeof k3);
	bad[200] ^= 0x01;
	CHECK(hubml_model_register(UID, bad, sizeof bad, "hub", NULL, idb) == -EBADMSG, "본문이 깨진 모델은 원장에 못 들어간다");

	CHECK(hubml_alias_set(UID, HUBML_A_V0, id3, "propose", "hub", NULL) == 0, "v0 = %s", id3);
	CHECK(hubml_alias_set(UID, HUBML_A_CHALLENGER, id1, "propose", "hub", NULL) == 0, "challenger = %s", id1);
	CHECK(hubml_alias_set(UID, HUBML_A_CHAMPION, id3, "promote", "studio:k1", NULL) == -EACCES, "서명 없는 champion 승격 거부");
	CHECK(hubml_alias_set(UID, HUBML_A_CHAMPION, "4d50444d00000001:9", "promote", "studio:k1", SIG) == -ENOENT, "없는 모델로는 못 옮긴다");
	CHECK(hubml_alias_set(UID, HUBML_A_CHAMPION, id3, "promote", "studio:k1", SIG) == 0, "champion = v0 (서명)");
	CHECK(hubml_alias_set(UID, HUBML_A_CHAMPION, id1, "promote", "studio:k1", SIG) == 0, "승격: champion = challenger");
	CHECK(!strcmp(hubml_alias_get(UID, HUBML_A_CHAMPION), id1), "champion 이 %s", id1);
	CHECK(hubml_alias_set(UID, HUBML_A_CHAMPION, id3, "rollback", "studio:k1", SIG) == 0, "되돌리기: champion = v0");
	hubml_ledger_head(&h);
	const uint32_t n_before = h.next_seq;
	char hash_before[65];
	strcpy(hash_before, h.hash);

	/* 재시작 → 같은 머리 · 같은 별칭 */
	hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
	hubml_ledger_head(&h);
	CHECK(h.ok && h.next_seq == n_before && !strcmp(h.hash, hash_before), "재시작: 사슬 %u 줄 · 머리 해시 같음", h.next_seq);
	CHECK(!strcmp(hubml_alias_get(UID, HUBML_A_CHAMPION), id3) && !strcmp(hubml_alias_get(UID, HUBML_A_V0), id3) &&
	      !strcmp(hubml_alias_get(UID, HUBML_A_CHALLENGER), id1), "재시작: 별칭 복원 (champion=v0=%s, challenger=%s)", id3, id1);

	/* 반쪽 꼬리 (쓰는 도중 전원 끊김) */
	const char torn[] = "{\"seq\":99,\"prev\":\"abc";
	file_patch(ROOT "/ledger.jsonl", 0, torn, sizeof torn - 1, true);
	hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
	hubml_ledger_head(&h);
	CHECK(h.ok && h.torn_bytes == sizeof torn - 1 && h.next_seq == n_before + 1, "반쪽 꼬리 %u B 잘라 냄 + ledger_repair 사건, 사슬 그대로", h.torn_bytes);
	CHECK(hubml_ledger_append("feedback", "studio:k1", "\"alarm_id\":7,\"uid\":\"4d50444d00000001\",\"accuracy\":\"correct\",\"chips\":[\"bearing\"],\"cause\":\"\",\"action\":\"\",\"note\":\"\",\"author\":\"kim\"", NULL, NULL) == 0, "잘라 낸 뒤 덧붙이기");

	/* 중간 변조 → 감지, 덧붙이기 막힘 → 되돌리면 정상 */
	struct fs_dirent de;
	fs_stat(ROOT "/ledger.jsonl", &de);
	static char buf[8192];
	struct fs_file_t f;
	fs_file_t_init(&f);
	fs_open(&f, ROOT "/ledger.jsonl", FS_O_READ);
	const ssize_t L = fs_read(&f, buf, sizeof buf - 1);
	fs_close(&f);
	buf[L] = 0;
	char *third = strstr(buf, "\"seq\":2,");
	char *pos = third ? strstr(third, "\"size\":") : NULL;
	if (pos) {
		const off_t at = (off_t)(pos - buf) + 7;	/* "size": 뒤 첫 숫자 */
		const char orig = *(pos + 7), evil = orig == '1' ? '3' : '1';
		file_patch(ROOT "/ledger.jsonl", at, &evil, 1, false);
		hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
		hubml_ledger_head(&h);
		CHECK(!h.ok && h.bad_seq == 3, "2 번 줄 1 바이트 변조 → 3 번(prev 불일치)에서 깨짐 감지 (bad_seq %u)", h.bad_seq);
		CHECK(hubml_ledger_append("eval", "hub", "\"x\":1", NULL, NULL) == -EBADMSG, "깨진 사슬에는 덧붙이지 않는다");
		file_patch(ROOT "/ledger.jsonl", at, &orig, 1, false);
		hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
		hubml_ledger_head(&h);
		CHECK(h.ok, "되돌리면 사슬 정상 (%u 줄)", h.next_seq);
	} else {
		CHECK(false, "변조할 줄을 못 찾았다");
	}

	lines = 0;
	const int got = hubml_ledger_read(2, 3, rd_cb, NULL);
	CHECK(got == 3 && first_seq == 2, "since=2 max=3 → %d 줄 (처음 %u)", got, first_seq);
	static uint8_t mb[40000];
	CHECK(hubml_model_load(UID, id3, mb, sizeof mb) == (int)sizeof k3 && !memcmp(mb, k3, sizeof k3), "모델 파일 다시 읽기 같음");

	hubml_ledger_read(1, 1000, dump_cb, NULL);
	printk("LEDGER_END\n");
	dump_tree(ROOT);
	printk("TREE_END\n");
	printk(ok ? "=== Lm4 PASS\n" : "=== Lm4 FAIL\n");
	return 0;
}
