/*
 * hub_relay — 1단계 "모델 고르기 · 로그 남기기": SD 카드는 U-Boot 가 읽고 쓴다.
 *
 * 재부팅을 건너 DRAM 이 남는 것(OTA 가 이미 쓰는 길)을 이용한다. sdcard/boot.cmd 와 같은 주소:
 *   0x510000F0 'HLOG'(0x474f4c48) · 0x510000F4 길이 · 0x51100000 로그 글 (≤ 1 MiB)
 *   0x51000300 "hub_loglen=<16진>\nhub_logseq=<다음 번호>\n" — U-Boot 는 env import -t 로 읽는다.
 *       ⚠ 이 U-Boot 에서는 setexpr 가 동작하지 않는다(길이도 번호 +1 도 빈 값·그대로) —
 *         그래서 다음 번호도 hubllm 이 계산해 넘긴다 (10진).
 *       (setexpr.l *주소 로 읽는 방식은 이 U-Boot 에서 빈 값을 냈다 — fatwrite 사용법 안내, 2026-11-08)
 *       → 다음 부팅 때 U-Boot 가 BLOB 파티션 logs/hub-<seq>.txt 로 쓴다 (seq 는 16진수)
 *   0x510000F8 'HENV'(0x564e4548) · 0x51000100 "hub_req=<파일>\n[hub_save=1\n]" (NUL 끝)
 *       → U-Boot 가 models/<파일> 을 올린다. save 면 hub.conf 의 hub_model 도 바꾼다
 *   0x510001F8 'MODL'(0x4c444f4d) · 0x51000200 "hub_loaded=…\nhub_logseq=…\n" (U-Boot 가 씀)
 *
 * ⚠ 전원을 끄면 RAM 이 지워져 그때까지의 로그는 사라진다 — 카드에 남기려면 hub reboot.
 *   실시간 기록은 지금처럼 USB(HUBLLM/1 done)로 호스트가 받는다. 2단계(Zephyr SD)에서 바로 쓴다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/arch/arm64/arm-smccc.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "hub_relay.h"

#define R            ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(hub_relay)))
#define OFF_LOGMAG   0xF0
#define OFF_LOGLEN   0xF4
#define OFF_ENVMAG   0xF8
#define OFF_ENV      0x100
#define OFF_MODMAG   0x1F8
#define OFF_MOD      0x200
#define OFF_LENTXT   0x300
#define OFF_LOG      0x100000
#define LOG_MAX      (1u << 20)
#define MAG_LOG      0x474f4c48u
#define MAG_ENV      0x564e4548u
#define MAG_MOD      0x4c444f4du

static char loaded[64] = "-", logseq[16] = "-";
static size_t loglen;
static unsigned next_seq = 1;	/* 이번 부팅 로그가 저장될 번호 = hub.conf 의 번호 + 1 */
static bool truncated;
static struct k_spinlock lk;

const char *hub_relay_loaded(void) { return loaded; }
const char *hub_relay_logseq(void) { return logseq; }
size_t hub_log_len(void) { return loglen; }

static void get_val(const char *txt, size_t n, const char *key, char *out, size_t outn)
{
	const size_t kl = strlen(key);
	for (size_t i = 0; i + kl < n && txt[i]; i++) {
		if ((i == 0 || txt[i - 1] == '\n') && !strncmp(txt + i, key, kl) && txt[i + kl] == '=') {
			size_t j = 0;
			for (const char *p = txt + i + kl + 1; *p && *p != '\n' && j + 1 < outn; p++) { out[j++] = *p; }
			out[j] = 0;
			return;
		}
	}
}

void hub_log(const char *line)
{
	k_spinlock_key_t k = k_spin_lock(&lk);
	const size_t n = strlen(line);
	if (loglen + n + 1 > LOG_MAX) {
		if (!truncated) {
			static const char t[] = "# … 로그 1 MiB 가 차서 뒤는 버렸다\n";
			if (loglen + sizeof(t) <= LOG_MAX) {
				memcpy((void *)(R + OFF_LOG + loglen), t, sizeof(t) - 1);
				loglen += sizeof(t) - 1;
			}
			truncated = true;
		}
	} else {
		memcpy((void *)(R + OFF_LOG + loglen), line, n);
		*(volatile char *)(R + OFF_LOG + loglen + n) = '\n';
		loglen += n + 1;
	}
	sys_write32((uint32_t)loglen, R + OFF_LOGLEN);
	{
		char t[64];
		const int k2 = snprintk(t, sizeof t, "hub_loglen=%x\nhub_logseq=%u\n", (unsigned)loglen,
					next_seq);
		memcpy((void *)(R + OFF_LENTXT), t, (size_t)k2 + 1);	/* NUL 까지 */
	}
	sys_write32(MAG_LOG, R + OFF_LOGMAG);
	k_spin_unlock(&lk, k);
}

int hub_relay_name_ok(const char *file)
{
	const size_t n = strlen(file);
	if (n == 0 || n > 60) { return -EINVAL; }
	for (size_t i = 0; i < n; i++) {
		const char c = file[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '.' || c == '_' || c == '-')) {
			return -EINVAL;	/* 경로·공백 금지 — U-Boot 명령에 그대로 들어간다 */
		}
	}
	return 0;
}

void hub_relay_set_loaded(const char *file)
{
	strncpy(loaded, file, sizeof loaded - 1);
	loaded[sizeof loaded - 1] = 0;
}

int hub_relay_request_model(const char *file, bool save)
{
	if (hub_relay_name_ok(file)) { return -EINVAL; }
	char *e = (char *)(R + OFF_ENV);
	memset(e, 0, 0x100);
	strcpy(e, "hub_req=");
	strcat(e, file);
	strcat(e, save ? "\nhub_save=1\n" : "\n");
	sys_write32(MAG_ENV, R + OFF_ENVMAG);
	return 0;
}

void hub_relay_reboot(void)
{
	struct arm_smccc_res res = { 0 };
	log_panic();
	k_msleep(50);
	__asm__ volatile("dsb sy" ::: "memory");
	arm_smccc_smc(0x84000009UL /* PSCI_SYSTEM_RESET */, 0, 0, 0, 0, 0, 0, 0, &res);
}

static int hub_relay_init(void)
{
	if (sys_read32(R + OFF_MODMAG) == MAG_MOD) {
		const char *t = (const char *)(R + OFF_MOD);
		get_val(t, 0x100, "hub_loaded", loaded, sizeof loaded);
		get_val(t, 0x100, "hub_logseq", logseq, sizeof logseq);
		next_seq = (unsigned)strtoul(logseq, NULL, 10) + 1U;
	}
	sys_write32(0, R + OFF_MODMAG);
	/* 앞 부팅의 로그는 U-Boot 가 이미 저장했다(boot.cmd 가 HLOG 를 지운다). 새로 시작 */
	sys_write32(0, R + OFF_LOGMAG);
	loglen = 0;
	return 0;
}
SYS_INIT(hub_relay_init, POST_KERNEL, 3);
