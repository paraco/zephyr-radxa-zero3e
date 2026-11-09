/*
 * hubml_q9 (Lm9) — ml approve (QEMU 가짜 셸, 명령은 4 KiB 스레드). 서명은 시험 빌드의 ed25519 서명 함수로
 * (Lm8 에서 Mac cryptography 와 바이트까지 같음을 확인). 원장 줄은 호스트 tools/ledger_verify.py --pk 가
 * 본문을 다시 만들어 Mac cryptography 로 서명을 검증한다.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/base64.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/hubml.h>
#include <hubml/sha256.h>
#include <hubml/ed25519.h>

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)
#define UIDS "4d50444d000000f9"
#define HUB "4d425737333500000000000000032106"
static const struct shell *sh;
static char out[65536];
static const char *pending;
static K_SEM_DEFINE(cmd_go, 0, 1);
static K_SEM_DEFINE(cmd_done, 0, 1);
static void cmd_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) { k_sem_take(&cmd_go, K_FOREVER); shell_execute_cmd(sh, pending); k_sem_give(&cmd_done); }
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
static uint8_t seed[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
			    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
static uint8_t pk[32];

static void utc(int64_t ms, char *o)
{
	time_t t = (time_t)(ms / 1000);
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(o, 24, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static const char *head(void) { static struct hubml_ledger_head h; hubml_ledger_head(&h); return h.hash; }

/* 본문(그대로) 에 서명해 ml approve 를 친다. flip = 서명 1 비트 바꾸기 */
static const char *approve(const char *body, const char *key, bool flip)
{
	static char cmd[4000], b64[2000], s64[100];
	uint8_t dg[32], sig[64], pkx[32];
	struct sha256 s;
	sha256_init(&s); sha256_update(&s, body, strlen(body)); sha256_final(&s, dg);
	hubml_ed25519_sign(sig, dg, 32, seed, pkx);
	if (flip) { sig[10] ^= 1; }
	size_t ol;
	base64_encode((uint8_t *)b64, sizeof b64, &ol, (const uint8_t *)body, strlen(body)); b64[ol] = 0;
	base64_encode((uint8_t *)s64, sizeof s64, &ol, sig, 64); s64[ol] = 0;
	snprintf(cmd, sizeof cmd, "ml approve b64=%s sig=%s key=%s", b64, s64, key);
	return run(cmd);
}
static char body[1500];
static const char *alias_body(const char *exp, const char *hd, const char *hub, const char *model, const char *name)
{
	snprintf(body, sizeof body,
		 "{\"exp\":\"%s\",\"head\":\"%s\",\"hub\":\"%s\",\"kind\":\"alias\",\"model_id\":\"%s\",\"name\":\"%s\",\"reason\":\"promote\",\"uid\":\"" UIDS "\",\"v\":1}",
		 exp, hd, hub, model, name);
	return body;
}
static void dump_cb(uint32_t seq, const char *l, size_t n, void *u) { ARG_UNUSED(seq); ARG_UNUSED(u); printk("LEDGER %.*s\n", (int)n, l); }

int main(void)
{
	printk("=== Lm9 ml approve (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm9 FAIL\n"); return 0; }
	fs_mkdir("/RAM:/ml");
	fs_mkdir("/RAM:/ml/keys");
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);
	uint8_t d0[32] = { 0 }, s0[64];
	hubml_ed25519_sign(s0, d0, 32, seed, pk);
	struct fs_file_t f;
	fs_file_t_init(&f);
	fs_open(&f, "/RAM:/ml/keys/k1.pub", FS_O_CREATE | FS_O_WRITE);
	fs_write(&f, pk, 32);
	fs_close(&f);
	printk("PK");
	for (int i = 0; i < 32; i++) { printk("%02x", pk[i]); }
	printk("\n");

	run("ml status --kv");	/* 연다 — 시각은 아직 */
	char mid[25];
	hubml_model_register(0x4d50444d000000f9ull, k3, sizeof k3, "hub", NULL, mid);
	char exp[24];
	utc(hubml_time_now() + 5 * 60000, exp);
	CHECK(strstr(approve(alias_body(exp, head(), HUB, mid, "champion"), "k1", false), "code=denied"), "시각 맞추기 전 → denied");

	run("ml time set 1794182400000");
	const int64_t now = 1794182400000LL;
	utc(now + 5 * 60000, exp);
	char h1[65];
	strcpy(h1, head());
	CHECK(strstr(approve(alias_body(exp, h1, HUB, mid, "champion"), "k1", true), "code=sig"), "서명 1 비트 → sig");
	CHECK(strstr(approve(alias_body(exp, h1, HUB, mid, "champion"), "k9", false), "code=unknown_key"), "없는 키 → unknown_key");
	CHECK(strstr(approve(alias_body(exp, h1, HUB, mid, "champion"), "K1", false), "code=unknown_key"), "키 이름 규칙 밖(K1) → unknown_key");
	CHECK(strstr(approve(alias_body(exp, h1, "ffff", mid, "champion"), "k1", false), "code=bad_arg"), "다른 허브 → bad_arg");
	CHECK(strstr(approve(alias_body(exp, h1, HUB, "4d50444d000000f9:7", "champion"), "k1", false), "code=not_found"), "없는 모델 → not_found");
	char ex2[24];
	utc(now - 1000, ex2);
	CHECK(strstr(approve(alias_body(ex2, h1, HUB, mid, "champion"), "k1", false), "code=expired"), "exp 지남 → expired");
	utc(now + 20 * 60000, ex2);
	CHECK(strstr(approve(alias_body(ex2, h1, HUB, mid, "champion"), "k1", false), "code=bad_arg"), "exp 가 10 분 넘음 → bad_arg");
	/* 비정규 본문 */
	alias_body(exp, h1, HUB, mid, "champion");
	char nc[1500];
	snprintf(nc, sizeof nc, "{\"exp\": \"%s\"%s", exp, strchr(body + 1, ','));	/* 콜론 뒤 공백 */
	CHECK(strstr(approve(nc, "k1", false), "code=bad_json"), "공백 → bad_json");
	snprintf(nc, sizeof nc, "{\"head\":\"%s\",\"exp\":\"%s\",\"hub\":\"" HUB "\",\"kind\":\"alias\",\"model_id\":\"%s\",\"name\":\"champion\",\"reason\":\"promote\",\"uid\":\"" UIDS "\",\"v\":1}", h1, exp, mid);
	CHECK(strstr(approve(nc, "k1", false), "code=bad_json"), "키 순서 어긋남 → bad_json");
	snprintf(nc, sizeof nc, "{\"a\":1.5,%s", body + 1);
	CHECK(strstr(approve(nc, "k1", false), "code=bad_json"), "실수 → bad_json");
	CHECK(!strstr(run("ml ledger since=1 max=50"), "approval_head"), "실패한 승인은 원장에 아무것도 남기지 않는다");

	/* 정상 */
	const char *r = approve(alias_body(exp, h1, HUB, mid, "champion"), "k1", false);
	CHECK(strstr(r, "HUBML/1 approve ok=1 kind=alias") && strstr(r, "deploy=-2"), "alias champion 승인 → ok · Edge 없음 deploy=-2 (%.80s)", r);
	run("ml model ls uid=" UIDS);
	CHECK(strstr(out, "alias=champion model_id=4d50444d000000f9:1"), "champion 이동됨");
	CHECK(strstr(approve(alias_body(exp, h1, HUB, mid, "champion"), "k1", false), "code=stale"), "같은 서명 다시 → stale (재사용 막힘)");
	snprintf(body, sizeof body,
		 "{\"d0\":{\"iso_zone\":\"A\",\"notes\":\"bench\",\"verdict\":\"pass\"},\"decision\":\"accept\",\"ds_id\":\"ds-" UIDS "-20261109-20261109-3\","
		 "\"exclude\":[[\"20261109\",\"12:00\",\"12:30\"]],\"exp\":\"%s\",\"head\":\"%s\",\"hub\":\"" HUB "\",\"include\":[[\"20261109\",\"00:00\",\"23:59\"]],"
		 "\"kind\":\"normal_gate\",\"v\":1}", exp, head());
	CHECK(strstr(approve(body, "k1", false), "HUBML/1 approve ok=1 kind=normal_gate"), "normal_gate 승인 → ok");

	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	CHECK(h.ok, "원장 사슬 정상 (%u 줄)", h.next_seq - 1);
	size_t unused = 0;
	k_thread_stack_space_get(cmd_thr, &unused);
	CHECK(unused >= 1024, "명령 스레드(4 KiB) 남은 스택 %zu B", unused);
	hubml_ledger_read(1, 100, dump_cb, NULL);
	printk(ok ? "=== Lm9 PASS\n" : "=== Lm9 FAIL\n");
	return 0;
}
