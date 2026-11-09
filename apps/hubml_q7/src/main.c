/*
 * hubml_q7 (Lm7) — `ml train` 을 QEMU 가짜 셸로. 가짜 정상 하루(86,400 벡터, 부하 0.4/0.8 이 2 시간마다)를 수집 엔진으로
 * 쌓고 → ml train → 출력(verdict · far · inject) · 원장 4 사건(dataset·model·eval·alias) · merkle(시험이 따로 계산) ·
 * 모델 파일 = Edge 가 받는 kind 3 (xmd_parse) · ml model ls 의 challenger.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <ff.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/hubml.h>
#include <hubml/sha256.h>
#include <hubml/xmb.h>

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)
#define UID 0x4d50444d000000d7ull
#define NF 34
static const struct shell *sh;
static char out[65536];
/* 명령은 실기 셸처럼 4 KiB 스택 스레드에서 친다 */
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

int main(void)
{
	printk("=== Lm7 ml train (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm7 FAIL\n"); return 0; }
	fs_mkdir("/RAM:/ml");
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);
	run("ml time set 1794186000000");	/* 2026-11-09 01:00 UTC — 시작 */
	/* 데이터: 엔진 API 로 직접 (ml rx 10,800 번은 느리다 — 그 길은 Lm6 이 시험했다) */
	uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM];
	struct mcol_hello hm = { .uid = UID, .fw = 0x0100, .contract = NF, .nfeat = NF, .schema_hash = 0x4d463334 };
	mcol_hello_put(pl, &hm);
	struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN };
	hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &h, pl));
	static float xs[8 * NF];
	for (uint32_t t = 0, sq = 0; t < 86400; t += 8, sq++) {
		for (int i = 0; i < 8; i++) { gen_vec(t + (uint32_t)i, &xs[i * NF]); }
		const int L = mcol_feat_put(pl, sizeof pl, NF, 8, xs, NULL);
		struct mcol_hdr f = { .type = MCOL_T_FEAT, .edge_sid = 1, .stream = MCOL_S_FEAT, .seq = sq, .len = (uint16_t)L };
		hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &f, pl));
		if (sq % 64 == 0) { hubml_ingest_commit(); }
	}
	hubml_ingest_commit();
	char day[9];
	hubml_day_str(hubml_time_now(), day);

	char cmd[96];
	snprintf(cmd, sizeof cmd, "ml train uid=4d50444d000000d7 days=%s", day);
	const int64_t t0 = k_uptime_get();
	run(cmd);
	printk("  %s  (%lld ms)\n", out, k_uptime_get() - t0);
	CHECK(strstr(out, "HUBML/1 train uid=4d50444d000000d7") && strstr(out, "model_id=4d50444d000000d7:1") &&
	      strstr(out, "vectors=86400") && strstr(out, "verdict=pass") && strstr(out, "rc=0"), "ml train → verdict=pass");
	run("ml model ls uid=4d50444d000000d7");
	CHECK(strstr(out, "alias=challenger model_id=4d50444d000000d7:1") && strstr(out, "alias=champion model_id=-"),
	      "ml model ls → challenger 제안 · champion 없음 (사람 승인 전)");

	/* 원장 4 사건 */
	run("ml ledger since=1 max=10");
	CHECK(strstr(out, "HUBML/1 ledger_done last=5") && strstr(out, "total=5 ok=1"), "원장 5 줄(time_set + 4 사건) · 사슬 정상");
	static char lines[16384];
	int li = 0;
	struct ctx { char *b; int *i; } c = { lines, &li };
	(void)c;
	/* merkle 을 시험이 따로 계산: ds/<uid>/<day>/s1.mcs\t<sha256>\n 의 sha256 */
	char p[128];
	snprintf(p, sizeof p, "/RAM:/ml/ds/4d50444d000000d7/%s/s1.mcs", day);
	struct fs_file_t f;
	fs_file_t_init(&f);
	struct sha256 s, top;
	sha256_init(&s);
	static uint8_t buf[4096];
	ssize_t n;
	fs_open(&f, p, FS_O_READ);
	while ((n = fs_read(&f, buf, sizeof buf)) > 0) { sha256_update(&s, buf, (size_t)n); }
	fs_close(&f);
	uint8_t dg[32];
	char hx[65], line[160], mk[65];
	sha256_final(&s, dg);
	for (int i = 0; i < 32; i++) { snprintf(hx + 2 * i, 3, "%02x", dg[i]); }
	const int ll = snprintf(line, sizeof line, "ds/4d50444d000000d7/%s/s1.mcs\t%s\n", day, hx);
	sha256_init(&top);
	sha256_update(&top, line, (size_t)ll);
	sha256_final(&top, dg);
	for (int i = 0; i < 32; i++) { snprintf(mk + 2 * i, 3, "%02x", dg[i]); }
	char want[100];
	snprintf(want, sizeof want, "\"merkle\":\"%s\"", mk);
	/* ledger 출력은 b64 — 원장 파일을 직접 본다 */
	fs_file_t_init(&f);
	static char lg[16384];
	fs_open(&f, "/RAM:/ml/ledger.jsonl", FS_O_READ);
	n = fs_read(&f, lg, sizeof lg - 1);
	fs_close(&f);
	lg[n > 0 ? n : 0] = 0;
	CHECK(strstr(lg, want) != NULL, "dataset merkle = 시험이 따로 계산한 값 (%.16s…)", mk);
	CHECK(strstr(lg, "\"kind\":\"dataset\"") && strstr(lg, "\"kind\":\"model\"") && strstr(lg, "\"type\":\"baseline_stats\"") &&
	      strstr(lg, "\"kind\":\"eval\"") && strstr(lg, "\"verdict\":\"pass\"") && strstr(lg, "\"reason\":\"propose\""),
	      "사건: dataset · model(baseline_stats) · eval(pass) · alias(challenger propose)");
	/* 모델 파일 = Edge 가 받는 kind 3 */
	static uint8_t mf[16384];
	static struct xmd_model md;
	const int ms = hubml_model_load(UID, "4d50444d000000d7:1", mf, sizeof mf);
	CHECK(ms > 0 && xmd_parse(mf, (size_t)ms, &md, 1e-5f) == 0, "models/…/1.xmb = kind 3 · 검사 통과 (%d B)", ms);
	printk("%s\n", lg);
	size_t unused = 0;
	k_thread_stack_space_get(cmd_thr, &unused);
	CHECK(unused >= 1024, "명령을 친 스레드(4 KiB, 실기 셸과 같음) 남은 스택 %zu B ≥ 1 KiB", unused);
	printk(ok ? "=== Lm7 PASS\n" : "=== Lm7 FAIL\n");
	return 0;
}
