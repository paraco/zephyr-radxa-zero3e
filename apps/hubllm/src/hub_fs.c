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
	.flags = FS_MOUNT_FLAG_READ_ONLY | FS_MOUNT_FLAG_NO_FORMAT,
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sd,
	SHELL_CMD_ARG(ls, NULL, "ls [<디렉터리>] — SD BLOB 파티션 목록 (기본 models)", cmd_hub_ls, 1, 1),
	SHELL_CMD_ARG(cmp, NULL, "cmp [<MiB>] — 올린 모델을 SD 에서 읽어 RAM 과 비교 (기본 64)", cmd_hub_sdcmp, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sd, &sub_sd, "SD 카드 (Zephyr 직접 · 읽기 전용)", NULL);
