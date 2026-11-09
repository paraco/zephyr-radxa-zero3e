/* hubml 실행부 — include/hubml/hubml.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
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

static void loop_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	int k = 0;
	for (;;) {
		k_msleep(500);
		hubml_deploy_tick();
		if (++k % 4 == 0) {	/* 2 초마다 기록 → ACK */
			(void)hubml_ingest_commit();
		}
	}
}
K_THREAD_STACK_DEFINE(ml_stack, 16384);
static struct k_thread ml_thr;

int hubml_start(const char *root, const char *hub_id)
{
	k_mutex_lock(&start_mtx, K_FOREVER);
	int rc = 0;
	if (started) {
		goto out;
	}
	rc = hubml_storage_ready();
	if (rc) {
		goto out;
	}
	strncpy(root_s, root, sizeof root_s - 1);
	strncpy(hub_s, hub_id, sizeof hub_s - 1);
	rc = hubml_ingest_init(root, &usb_xp);
	if (!rc) {
		rc = hubml_ledger_init(root, hub_id);
	}
	if (!rc) {
		k_thread_create(&ml_thr, ml_stack, K_THREAD_STACK_SIZEOF(ml_stack), loop_fn, NULL, NULL, NULL,
				K_PRIO_PREEMPT(9), 0, K_NO_WAIT);
		k_thread_name_set(&ml_thr, "hubml");
		started = true;
	}
out:
	k_mutex_unlock(&start_mtx);
	return rc;
}
