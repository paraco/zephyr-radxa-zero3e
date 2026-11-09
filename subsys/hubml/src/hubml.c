/* hubml 실행부 — include/hubml/hubml.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(hubml);
#include <hubml/hubml.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/deploy.h>

static bool started;
static char root_s[48], hub_s[40];
const struct shell *hubml_tx_shell;	/* 마지막으로 ml 을 친 셸 — tx 줄을 여기에 찍는다 */
static K_MUTEX_DEFINE(start_mtx);

__weak int hubml_storage_ready(void) { return 0; }
bool hubml_started(void) { return started; }
const char *hubml_root(void) { return root_s; }
const char *hubml_hub_id(void) { return hub_s; }

static int usb_send(uint32_t addr, const uint8_t *b, size_t n)
{
	const struct shell *sh = hubml_tx_shell;
	if (!sh) {
		return -ENODEV;
	}
	char line[1700];
	size_t ol = 0;
	if (base64_encode((uint8_t *)line, sizeof line, &ol, b, n)) {
		return -EMSGSIZE;
	}
	line[ol] = 0;
	shell_print(sh, "HUBML/1 tx addr=%u b64=%s", addr, line);
	return 0;
}
static const struct hubml_xport usb_xp = { .send = usb_send };

/* 한 번에 한 일 — 셸이 넣고 기다린다 */
static K_MUTEX_DEFINE(call_mtx);
static K_SEM_DEFINE(job_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 1);
static int (*job_fn)(void *);
static void *job_arg;
static int job_rc;
static volatile long long free_kib = -1;
long long hubml_free_kib(void) { return free_kib; }

static void measure_free(void)
{
	struct fs_statvfs v;
	const int64_t t0 = k_uptime_get();
	hubml_fs_lock();
	const int r = fs_statvfs(root_s, &v);
	hubml_fs_unlock();
	free_kib = r ? -2 : (long long)v.f_bfree * v.f_frsize / 1024;
	LOG_INF("저장소 여유 %lld KiB (%lld ms)", free_kib, k_uptime_get() - t0);
}

static void loop_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	int k = 0;
	bool measured = false;
	for (;;) {
		if (k_sem_take(&job_sem, K_MSEC(500)) == 0) {
			job_rc = job_fn(job_arg);
			k_sem_give(&done_sem);
			continue;
		}
		if (!started) {
			continue;
		}
		if (!measured) {
			measured = true;
			measure_free();
		}
		hubml_deploy_tick();
		if (++k % 4 == 0) {	/* 2 초마다 기록 → ACK */
			(void)hubml_ingest_commit();
		}
	}
}
K_THREAD_STACK_DEFINE(ml_stack, 16384);
static struct k_thread ml_thr;

static bool thr_up;

int hubml_call(int (*fn)(void *arg), void *arg)
{
	k_mutex_lock(&call_mtx, K_FOREVER);
	if (!thr_up) {
		k_thread_create(&ml_thr, ml_stack, K_THREAD_STACK_SIZEOF(ml_stack), loop_fn, NULL, NULL, NULL,
				K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
		k_thread_name_set(&ml_thr, "hubml");
		thr_up = true;
	}
	job_fn = fn;
	job_arg = arg;
	k_sem_give(&job_sem);
	k_sem_take(&done_sem, K_FOREVER);
	const int rc = job_rc;
	k_mutex_unlock(&call_mtx);
	return rc;
}

struct start_arg {
	const char *root, *hub;
};

static int start_fn(void *p)
{
	const struct start_arg *a = p;
	int rc = hubml_storage_ready();
	if (rc) {
		return rc;
	}
	strncpy(root_s, a->root, sizeof root_s - 1);
	strncpy(hub_s, a->hub, sizeof hub_s - 1);
	rc = hubml_ingest_init(a->root, &usb_xp);
	if (!rc) {
		rc = hubml_ledger_init(a->root, a->hub);
	}
	if (!rc) {
		started = true;
	}
	return rc;
}

/* hubml 스레드 안에서 연다 (ml 셸 본체가 거기서 돈다 — hubml_call 을 다시 부르면 자기를 기다린다) */
int hubml_start_here(const char *root, const char *hub_id)
{
	struct start_arg a = { root, hub_id };
	return started ? 0 : start_fn(&a);
}

int hubml_start(const char *root, const char *hub_id)
{
	k_mutex_lock(&start_mtx, K_FOREVER);
	struct start_arg a = { root, hub_id };
	const int rc = started ? 0 : hubml_call(start_fn, &a);
	k_mutex_unlock(&start_mtx);
	return rc;
}
