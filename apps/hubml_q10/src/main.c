/*
 * hubml_q10 (Lm10) — `ml drift`: 날 A 정상으로 학습 → 날 B 에 1X 오더가 1.0→1.5 배로 서서히 커진다 → 날짜별 경보율·주범 특징·판정.
 * (아래는 Lm7 에서 가져온 틀) 가짜 정상 하루(86,400 벡터, 부하 0.4/0.8 이 2 시간마다)를 수집 엔진으로
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
#include <stdlib.h>
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
#define UID 0x4d50444d000000aaull
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


static void feed(uint32_t base_t, int ramp)
{
	uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM];
	static float xs[8 * NF];
	static uint32_t sq;
	for (uint32_t t = 0; t < 86400; t += 8, sq++) {
		for (int i = 0; i < 8; i++) {
			gen_vec(t + (uint32_t)i, &xs[i * NF]);
			if (ramp) { xs[i * NF + 6] *= 1.0f + 0.5f * (float)(t + (uint32_t)i) / 86400.0f; }	/* 1X 오더 서서히 ↑ */
		}
		const int L = mcol_feat_put(pl, sizeof pl, NF, 8, xs, NULL);
		struct mcol_hdr f = { .type = MCOL_T_FEAT, .edge_sid = 1, .stream = MCOL_S_FEAT, .seq = sq, .len = (uint16_t)L };
		hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &f, pl));
		if (sq % 64 == 0) { hubml_ingest_commit(); }
	}
	hubml_ingest_commit();
	ARG_UNUSED(base_t);
}

int main(void)
{
	printk("=== Lm10 ml drift (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm10 FAIL\n"); return 0; }
	fs_mkdir("/RAM:/ml");
	sh = shell_backend_dummy_get_ptr();
	k_msleep(100);
	run("ml time set 1794186000000");
	uint8_t pl[MCOL_MAX_PAYLOAD], d[MCOL_MAX_DGRAM];
	struct mcol_hello hm = { .uid = UID, .fw = 0x0100, .contract = NF, .nfeat = NF, .schema_hash = 0x4d463334 };
	mcol_hello_put(pl, &hm);
	struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN };
	hubml_ingest_rx(1, d, (size_t)mcol_encode(d, sizeof d, &h, pl));
	feed(0, 0);
	char dayA[9], dayB[9], cmd[128];
	hubml_day_str(hubml_time_now(), dayA);
	snprintf(cmd, sizeof cmd, "ml train uid=4d50444d000000aa days=%s", dayA);
	run(cmd);
	CHECK(strstr(out, "verdict=pass"), "날 A 로 학습 (%.100s)", strstr(out, "model_id=") ? strstr(out, "model_id=") : out);
	run("ml time set 1794272400000");	/* 다음 날 01:00 UTC */
	feed(0, 1);
	hubml_day_str(hubml_time_now(), dayB);
	snprintf(cmd, sizeof cmd, "ml drift uid=4d50444d000000aa days=%s,%s model=4d50444d000000aa:1", dayA, dayB);
	run(cmd);
	printk("%s", out);
	const char *a = strstr(out, dayA), *b = strstr(out, dayB);
	float ra = 1, rb = 0;
	if (a) { const char *q = strstr(a, "alarm_rate="); if (q) { ra = strtof(q + 11, NULL); } }
	if (b) { const char *q = strstr(b, "alarm_rate="); if (q) { rb = strtof(q + 11, NULL); } }
	CHECK(ra < 0.015f, "날 A (학습한 날) 경보율 %.4f < 1.5 %%", (double)ra);
	CHECK(rb > 0.30f, "날 B (1X 1.0→1.5 배) 경보율 %.4f > 30 %%", (double)rb);
	CHECK(b && strstr(b, "top=ord_1x:"), "날 B 주범 특징 = ord_1x");
	CHECK(strstr(out, "verdict=alarm"), "판정 alarm");
	run("ml drift uid=4d50444d000000aa days=20261109");
	CHECK(strstr(out, "code=not_found"), "v0 별칭 없이 model= 도 없으면 → not_found");
	size_t unused = 0;
	k_thread_stack_space_get(cmd_thr, &unused);
	CHECK(unused >= 1024, "명령 스레드(4 KiB) 남은 스택 %zu B", unused);
	printk(ok ? "=== Lm10 PASS\n" : "=== Lm10 FAIL\n");
	return 0;
}
