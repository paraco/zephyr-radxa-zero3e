/*
 * hub_fs — 2단계: Zephyr 가 SD 카드의 BLOB 파티션(FAT32)을 직접 읽는다 (2a: 읽기 전용).
 *
 *   sd ls [<디렉터리>]           /0:/<디렉터리> (기본 models) 목록과 크기
 *   sd cmp [<MiB>]               지금 올라 있는 모델 파일(models/<hub_loaded>)을 SD 에서 읽어
 *                                U-Boot 가 RAM(0x58000000)에 올린 것과 바이트 비교 + 읽기 속도.
 *                                드라이버가 맞는지 보는 가장 확실한 시험 (같은 파일 두 경로)
 *
 * 마운트는 처음 쓸 때 한 번. 카드는 GPT 라 FatFs 의 VolToPart 로 두 번째 "Microsoft Basic Data"
 * 파티션(config 다음 = BLOB)을 고른다 (FF_LBA64 · FF_MULTI_PARTITION).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/devicetree.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include "hub_relay.h"
#include "hub.h"

/* 볼륨 0("SD") → 물리 드라이브 0 의 두 번째 MS Basic Data 파티션 */
PARTITION VolToPart[FF_VOLUMES] = { { 0, 2 } };

static FATFS fat;
static struct fs_mount_t mnt = {
	.type = FS_FATFS,
	.mnt_point = "/0:",	/* MULTI_PARTITION 에서는 번호로 (Kconfig.fatfs 도움말) */
	.fs_data = &fat,
	.flags = FS_MOUNT_FLAG_NO_FORMAT,	/* 2b: 쓰기도 한다 (로그) — 포맷은 절대 하지 않는다 */
};
static bool mounted;
static K_MUTEX_DEFINE(mnt_lock);	/* 셸 스레드(sd …)와 LLM 스레드(hub load)가 함께 부른다 */
/* FatFs 는 FF_FS_REENTRANT 없이 빌드된다 — 파일 접근은 한 번에 하나만 (hub load 중 sd … 는 busy) */
static K_MUTEX_DEFINE(io_lock);

/* sh 가 NULL 이면 조용히 (LLM 스레드에서) */
static int ensure_mount(const struct shell *sh)
{
	k_mutex_lock(&mnt_lock, K_FOREVER);
	int rc = 0;
	if (!mounted) {
		const int64_t t0 = k_uptime_get();
		rc = fs_mount(&mnt);
		if (rc == 0) { mounted = true; }
		if (sh && rc) {
			shell_print(sh, "HUBLLM/1 err id=0 code=sd_mount msg=%d", rc);
		} else if (sh) {
			shell_print(sh, "HUBLLM/1 sd mounted=/0: ms=%lld", (long long)(k_uptime_get() - t0));
		}
	}
	k_mutex_unlock(&mnt_lock);
	return rc;
}

static void model_path(char *p, size_t n, const char *name)
{
	snprintf(p, n, "/0:/models/%s", name);
}

static int model_size_locked(const char *name, size_t *size)
{
	int rc = ensure_mount(NULL);
	if (rc) { return rc; }
	char path[96];
	model_path(path, sizeof path, name);
	struct fs_dirent e;
	rc = fs_stat(path, &e);
	if (rc) { return rc; }
	if (e.type != FS_DIR_ENTRY_FILE) { return -EISDIR; }
	*size = e.size;
	return 0;
}

int hub_fs_model_size(const char *name, size_t *size)
{
	k_mutex_lock(&io_lock, K_FOREVER);
	const int rc = model_size_locked(name, size);
	k_mutex_unlock(&io_lock);
	return rc;
}

/* 1 MiB 씩 — FatFs 가 클러스터가 이어진 구간은 사용자 버퍼로 바로 여러 블록을 읽는다 */
int hub_fs_read_model(const char *name, void *dst, size_t max, size_t *got,
		      int (*prog)(size_t done, size_t total))
{
	*got = 0;
	size_t total = 0;
	k_mutex_lock(&io_lock, K_FOREVER);
	int rc = model_size_locked(name, &total);
	if (rc == 0 && total > max) { rc = -EFBIG; }
	if (rc) { k_mutex_unlock(&io_lock); return rc; }
	char path[96];
	model_path(path, sizeof path, name);
	struct fs_file_t f;
	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc) { k_mutex_unlock(&io_lock); return rc; }
	uint8_t *d = (uint8_t *)dst;
	const size_t CH = 1024 * 1024;
	while (*got < total) {
		const size_t want = MIN(CH, total - *got);
		const ssize_t n = fs_read(&f, d + *got, want);
		if (n < 0) { rc = (int)n; break; }
		if (n == 0) { rc = -EIO; break; }	/* 크기보다 짧다 */
		*got += (size_t)n;
		if (prog && prog(*got, total)) { rc = -ECANCELED; break; }
	}
	fs_close(&f);
	k_mutex_unlock(&io_lock);
	return rc;
}

static int busy(const struct shell *sh)
{
	if (k_mutex_lock(&io_lock, K_NO_WAIT)) {
		shell_print(sh, "HUBLLM/1 err id=0 code=busy msg=sd_in_use");
		return -EBUSY;
	}
	return 0;
}

static int do_ls(const struct shell *sh, size_t argc, char **argv);
static int do_sdcmp(const struct shell *sh, size_t argc, char **argv);

static int cmd_hub_ls(const struct shell *sh, size_t argc, char **argv)
{
	if (busy(sh)) { return -EBUSY; }
	const int rc = do_ls(sh, argc, argv);
	k_mutex_unlock(&io_lock);
	return rc;
}

static int cmd_hub_sdcmp(const struct shell *sh, size_t argc, char **argv)
{
	if (busy(sh)) { return -EBUSY; }
	const int rc = do_sdcmp(sh, argc, argv);
	k_mutex_unlock(&io_lock);
	return rc;
}

static int do_ls(const struct shell *sh, size_t argc, char **argv)
{
	if (ensure_mount(sh)) { return -EIO; }
	char path[96];
	snprintf(path, sizeof path, "/0:/%s", argc > 1 ? argv[1] : "models");
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, path);
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=sd_opendir msg=%d", rc);
		return rc;
	}
	struct fs_dirent e;
	int n = 0;
	while (fs_readdir(&dir, &e) == 0 && e.name[0]) {
		shell_print(sh, "HUBLLM/1 ls %s %s size=%zu", e.type == FS_DIR_ENTRY_DIR ? "dir" : "file",
			    e.name, e.size);
		n++;
	}
	fs_closedir(&dir);
	shell_print(sh, "HUBLLM/1 ls_done path=%s n=%d", path, n);
	return 0;
}

static uint8_t buf[256 * 1024] __aligned(64);

static int do_sdcmp(const struct shell *sh, size_t argc, char **argv)
{
	const size_t mib = argc > 1 ? (size_t)atoi(argv[1]) : 64;
	if (ensure_mount(sh)) { return -EIO; }
	const char *name = hub_relay_loaded();
	if (!strcmp(name, "-") || strchr(name, ':')) {
		shell_print(sh, "HUBLLM/1 err id=0 code=sdcmp msg=no_loaded_model_name");
		return -EINVAL;
	}
	char path[96];
	model_path(path, sizeof path, name);
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_READ);
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=sd_open msg=%d", rc);
		return rc;
	}
	const uint8_t *ram = (const uint8_t *)DT_REG_ADDR(DT_NODELABEL(model_blob));
	size_t off = 0, bad = 0, first_bad = (size_t)-1;
	const int64_t t0 = k_uptime_get();
	while (off < mib * 1024 * 1024) {
		const ssize_t n = fs_read(&f, buf, sizeof buf);
		if (n <= 0) { break; }
		if (memcmp(buf, ram + off, (size_t)n)) {
			for (ssize_t i = 0; i < n; i++) {
				if (buf[i] != ram[off + i]) { bad++; if (first_bad == (size_t)-1) { first_bad = off + i; } }
			}
		}
		off += (size_t)n;
	}
	const int64_t ms = k_uptime_get() - t0;
	fs_close(&f);
	shell_print(sh, "HUBLLM/1 sdcmp file=%s bytes=%zu ms=%lld mbps=%.2f bad=%zu first_bad=%ld",
		    name, off, (long long)ms, ms ? (double)off / 1048576.0 * 1000.0 / (double)ms : 0.0,
		    bad, bad ? (long)first_bad : -1L);
	return 0;
}

/* ── 2b: SD 쓰기 ─────────────────────────────────────────────────────────────
 *
 * 실시간 로그: hub_log 가 RAM 중계 구역(0x51100000)에 쌓는 글을 2 초마다 logs/hub-<N>.txt 에
 * 덧붙이고 fs_sync 한다. N = max(logs/ 의 가장 큰 번호 + 1, U-Boot 가 넘긴 다음 번호).
 * hub.conf 는 쓰지 않는다 — 번호는 logs/ 를 훑어 정한다.
 * 쓰는 동안 'HLOG' 를 지워 U-Boot 는 저장하지 않는다. 쓰기가 실패하면 'HLOG' 를 다시 세워
 * 재부팅 때 U-Boot 가 같은 번호로 전체를 쓴다(대비).
 *
 * ⚠ 전원이 끊기면 마지막 동기화(≤ 2 s) 뒤의 줄은 잃고, FAT 갱신 도중이면 메타데이터가 어긋날 수 있다.
 * ⚠ 중계 구역이 1 MiB 라 파일도 1 MiB 에서 멈춘다.
 */
#ifndef HUB_SDLOG_AUTO
/* 부팅하면 스스로 켠다. 2026-11-09 실기에서 sd wtest 1·16 MiB 가 bad 0, 쓴 뒤 U-Boot 가 모델·로그를
 * 정상으로 읽고 써서(FAT 이상 없음) 켰다. 끄려면 sd log off (이번 부팅만) */
#define HUB_SDLOG_AUTO 1
#endif

enum { LW_OFF, LW_ON, LW_FAIL };
static struct fs_file_t lf;
static volatile int lw_state = LW_OFF;
static volatile bool lw_want = HUB_SDLOG_AUTO;
static size_t lw_written;
static int lw_err, lw_writes, lw_last_ms;
static char lname[32] = "-";
static K_SEM_DEFINE(lw_kick, 0, 1);

const char *hub_fs_log_name(void) { return lw_state == LW_ON ? lname : "-"; }

static int mkdir_logs(void)
{
	const int rc = fs_mkdir("/0:/logs");
	return (rc == -EEXIST) ? 0 : rc;
}

/* io_lock 을 쥔 채 */
static int log_start(void)
{
	int rc = ensure_mount(NULL);
	if (rc == 0) { rc = mkdir_logs(); }
	if (rc) { return rc; }
	unsigned maxn = 0;
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, "/0:/logs") == 0) {
		struct fs_dirent e;
		while (fs_readdir(&dir, &e) == 0 && e.name[0]) {
			unsigned n;
			char tail[8];
			if (sscanf(e.name, "hub-%u.%7s", &n, tail) == 2 && n > maxn) { maxn = n; }
		}
		fs_closedir(&dir);
	}
	const unsigned seq = MAX(maxn + 1, hub_relay_next_seq());
	snprintf(lname, sizeof lname, "logs/hub-%u.txt", seq);
	char path[48];
	snprintf(path, sizeof path, "/0:/%s", lname);
	fs_file_t_init(&lf);
	rc = fs_open(&lf, path, FS_O_CREATE | FS_O_WRITE);
	if (rc) { return rc; }
	lw_written = 0;
	hub_relay_set_next_seq(seq);
	hub_relay_set_uboot_save(false);
	lw_state = LW_ON;
	return 0;
}

static int log_flush_locked(void)
{
	const size_t len = hub_log_len();
	if (len <= lw_written) { return 0; }
	const int64_t t0 = k_uptime_get();
	const ssize_t n = fs_write(&lf, hub_log_buf() + lw_written, len - lw_written);
	if (n < 0) { return (int)n; }
	if ((size_t)n != len - lw_written) { return -ENOSPC; }
	const int rc = fs_sync(&lf);
	if (rc) { return rc; }
	lw_written = len;
	lw_writes++;
	lw_last_ms = (int)(k_uptime_get() - t0);
	return 0;
}

static void log_stop(int err)
{
	(void)fs_close(&lf);
	lw_state = err ? LW_FAIL : LW_OFF;
	lw_err = err;
	hub_relay_set_uboot_save(true);	/* U-Boot 가 재부팅 때 전체를 같은 번호로 쓴다 */
	if (err) { printk("hub_fs: SD 로그 쓰기 실패 %d — U-Boot 저장으로 되돌린다\n", err); }
}

void hub_fs_log_flush(int wait_ms)
{
	if (lw_state != LW_ON || k_mutex_lock(&io_lock, K_MSEC(wait_ms))) { return; }
	if (lw_state == LW_ON) {
		const int rc = log_flush_locked();
		if (rc) { log_stop(rc); }
	}
	k_mutex_unlock(&io_lock);
}

static void lw_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&lw_kick, K_MSEC(2000));
		if (lw_state == LW_FAIL) { continue; }
		/* hub load 가 카드를 1 분 넘게 쥔다 — 기다리지 않고 다음 차례에 (글은 RAM 에 남아 있다) */
		if (k_mutex_lock(&io_lock, K_MSEC(100))) { continue; }
		int rc = 0;
		if (lw_want && lw_state == LW_OFF) { rc = log_start(); }
		if (rc == 0 && lw_state == LW_ON) { rc = log_flush_locked(); }
		if (rc) {
			if (lw_state == LW_ON) { log_stop(rc); } else { lw_state = LW_FAIL; lw_err = rc; }
		} else if (!lw_want && lw_state == LW_ON) {
			log_stop(0);
		}
		k_mutex_unlock(&io_lock);
	}
}
K_THREAD_DEFINE(hub_lw, 4096, lw_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(14), 0, 3000);

static int cmd_sd_log(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (!strcmp(argv[1], "on")) {
			if (lw_state == LW_FAIL) { lw_state = LW_OFF; }	/* 다시 시도 */
			lw_want = true;
		} else if (!strcmp(argv[1], "off")) {
			lw_want = false;
		} else {
			shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=on_or_off");
			return -EINVAL;
		}
		k_sem_give(&lw_kick);
		k_msleep(300);
	}
	static const char *const st[] = { "off", "on", "fail" };
	shell_print(sh, "HUBLLM/1 sdlog state=%s want=%d file=%s written=%zu len=%zu writes=%d last_ms=%d "
		    "err=%d seq=%u", st[lw_state], lw_want ? 1 : 0, lname, lw_written, hub_log_len(),
		    lw_writes, lw_last_ms, lw_err, hub_relay_next_seq());
	return 0;
}

/* sd wtest [MiB] — 쓰기 경로 첫 시험: logs/wtest.bin 에 무늬를 쓰고 다시 읽어 비교 */
static int do_wtest(const struct shell *sh, size_t argc, char **argv)
{
	const size_t mib = argc > 1 ? (size_t)atoi(argv[1]) : 1;
	if (mib < 1 || mib > 64) {
		shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=wtest_1..64");
		return -EINVAL;
	}
	int rc = ensure_mount(sh);
	if (rc == 0) { rc = mkdir_logs(); }
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (rc == 0) { rc = fs_open(&f, "/0:/logs/wtest.bin", FS_O_CREATE | FS_O_RDWR); }
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=wtest_open msg=%d", rc);
		return rc;
	}
	rc = fs_truncate(&f, 0);
	const size_t total = mib * 1024 * 1024;
	uint32_t *w = (uint32_t *)buf;
	int64_t t0 = k_uptime_get();
	size_t off = 0;
	while (rc == 0 && off < total) {
		for (size_t i = 0; i < sizeof buf / 4; i++) { w[i] = (uint32_t)((off / 4 + i) * 2654435761u); }
		const ssize_t n = fs_write(&f, buf, sizeof buf);
		if (n != (ssize_t)sizeof buf) { rc = n < 0 ? (int)n : -ENOSPC; break; }
		off += sizeof buf;
	}
	if (rc == 0) { rc = fs_sync(&f); }
	const int64_t wms = k_uptime_get() - t0;
	fs_close(&f);
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=wtest_write msg=%d_at_%zu", rc, off);
		return rc;
	}
	fs_file_t_init(&f);
	rc = fs_open(&f, "/0:/logs/wtest.bin", FS_O_READ);
	size_t bad = 0, got = 0;
	t0 = k_uptime_get();
	while (rc == 0 && got < total) {
		const ssize_t n = fs_read(&f, buf, sizeof buf);
		if (n <= 0) { break; }
		for (size_t i = 0; i < (size_t)n / 4; i++) {
			if (w[i] != (uint32_t)((got / 4 + i) * 2654435761u)) { bad++; }
		}
		got += (size_t)n;
	}
	const int64_t rms = k_uptime_get() - t0;
	fs_close(&f);
	shell_print(sh, "HUBLLM/1 wtest bytes=%zu write_ms=%lld write_mbps=%.2f read=%zu read_ms=%lld "
		    "bad_words=%zu rc=%d", total, (long long)wms,
		    wms ? (double)total / 1048576.0 * 1000.0 / (double)wms : 0.0, got, (long long)rms,
		    bad, rc);
	return 0;
}

static int cmd_sd_wtest(const struct shell *sh, size_t argc, char **argv)
{
	if (busy(sh)) { return -EBUSY; }
	const int rc = do_wtest(sh, argc, argv);
	k_mutex_unlock(&io_lock);
	return rc;
}

/* sd cat <경로> [바이트] — 파일 끝 쪽 (기본 1536 B) */
static int do_cat(const struct shell *sh, size_t argc, char **argv)
{
	const size_t want = argc > 2 ? (size_t)atoi(argv[2]) : 1536;
	int rc = ensure_mount(sh);
	char path[96];
	snprintf(path, sizeof path, "/0:/%s", argv[1]);
	struct fs_dirent e;
	if (rc == 0) { rc = fs_stat(path, &e); }
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (rc == 0) { rc = fs_open(&f, path, FS_O_READ); }
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=cat_open msg=%d", rc);
		return rc;
	}
	const size_t n0 = MIN(want, MIN(e.size, sizeof buf - 1));
	(void)fs_seek(&f, (off_t)(e.size - n0), FS_SEEK_SET);
	const ssize_t n = fs_read(&f, buf, n0);
	fs_close(&f);
	buf[n > 0 ? n : 0] = 0;
	shell_print(sh, "HUBLLM/1 cat path=%s size=%zu tail=%zd", argv[1], e.size, n);
	char *p = (char *)buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl) { *nl = 0; }
		shell_print(sh, "| %s", p);
		if (!nl) { break; }
		p = nl + 1;
	}
	return 0;
}

static int cmd_sd_cat(const struct shell *sh, size_t argc, char **argv)
{
	if (busy(sh)) { return -EBUSY; }
	const int rc = do_cat(sh, argc, argv);
	k_mutex_unlock(&io_lock);
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sd,
	SHELL_CMD_ARG(ls, NULL, "ls [<디렉터리>] — SD BLOB 파티션 목록 (기본 models)", cmd_hub_ls, 1, 1),
	SHELL_CMD_ARG(cmp, NULL, "cmp [<MiB>] — 올린 모델을 SD 에서 읽어 RAM 과 비교 (기본 64)", cmd_hub_sdcmp, 1, 1),
	SHELL_CMD_ARG(wtest, NULL, "wtest [MiB] — logs/wtest.bin 쓰고 다시 읽어 비교 (기본 1)", cmd_sd_wtest, 1, 1),
	SHELL_CMD_ARG(cat, NULL, "cat <경로> [바이트] — 파일 끝 쪽 보기 (예: logs/hub-9.txt)", cmd_sd_cat, 2, 1),
	SHELL_CMD_ARG(log, NULL, "log [on|off] — SD 실시간 로그 상태 / 켜기·끄기", cmd_sd_log, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sd, &sub_sd, "SD 카드 (Zephyr 직접 — 읽기 · 로그 쓰기)", NULL);
