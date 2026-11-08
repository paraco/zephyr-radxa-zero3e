/*
 * hubllm 셸 명령 — `hub gen|abort|info`. 규약은 hub.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/base64.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/app_version.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "hub.h"
#include <rk3566_tsadc.h>
#include <rk3566_boot_slot.h>

static struct hub_req req;	/* 셸 스레드 하나만 쓴다 — submit 이 복사한다 */

static int cmd_hub_gen(const struct shell *sh, size_t argc, char **argv)
{
	memset(&req, 0, sizeof(req));
	req.kind = HUB_GEN;
	req.max_gen = 64;
	req.topp = 1.0f;
	req.rep = 1.0f;
	req.last_n = 64;
	bool have_in = false;

	for (size_t i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strncmp(a, "id=", 3)) {
			req.id = strtoul(a + 3, NULL, 0);
		} else if (!strncmp(a, "max=", 4)) {
			req.max_gen = atoi(a + 4);
		} else if (!strncmp(a, "temp=", 5)) {
			req.temp = strtof(a + 5, NULL);
		} else if (!strncmp(a, "topk=", 5)) {
			req.topk = atoi(a + 5);
		} else if (!strncmp(a, "topp=", 5)) {
			req.topp = strtof(a + 5, NULL);
		} else if (!strncmp(a, "seed=", 5)) {
			req.seed = strtoul(a + 5, NULL, 0);
		} else if (!strncmp(a, "pres=", 5)) {
			req.pres = strtof(a + 5, NULL);
		} else if (!strncmp(a, "freq=", 5)) {
			req.freq = strtof(a + 5, NULL);
		} else if (!strncmp(a, "rep=", 4)) {
			req.rep = strtof(a + 4, NULL);
		} else if (!strncmp(a, "last_n=", 7)) {
			req.last_n = atoi(a + 7);
		} else if (!strncmp(a, "keep=", 5)) {
			req.keep = atoi(a + 5) != 0;
		} else if (!strncmp(a, "special=", 8)) {
			req.special = atoi(a + 8) != 0;
		} else if (!strncmp(a, "b64=", 4)) {
			size_t olen = 0;
			if (base64_decode((uint8_t *)req.text, sizeof(req.text) - 1, &olen,
					  (const uint8_t *)a + 4, strlen(a + 4)) != 0) {
				shell_print(sh, "HUBLLM/1 err id=%u code=bad_b64 msg=decode", req.id);
				return -EINVAL;
			}
			req.text_len = (int)olen;
			req.text[olen] = '\0';
			req.use_ids = false;
			have_in = true;
		} else if (!strncmp(a, "ids=", 4)) {
			char *p = a + 4;
			req.n_ids = 0;
			while (*p && req.n_ids < HUB_MAX_PROMPT_IDS) {
				req.ids[req.n_ids++] = (int32_t)strtol(p, &p, 10);
				if (*p == ',') { p++; } else if (*p) { break; }
			}
			req.use_ids = true;
			have_in = true;
		} else {
			shell_print(sh, "HUBLLM/1 err id=%u code=bad_arg msg=%s", req.id, a);
			return -EINVAL;
		}
	}
	if (!have_in || req.max_gen < 1) {
		shell_print(sh, "HUBLLM/1 err id=%u code=bad_arg msg=need_b64_or_ids_and_max", req.id);
		return -EINVAL;
	}
	const int rc = hub_llm_submit(sh, &req);
	if (rc == -EBUSY) {
		shell_print(sh, "HUBLLM/1 err id=%u code=busy msg=generating", req.id);
	} else if (rc == -EAGAIN) {
		shell_print(sh, "HUBLLM/1 err id=%u code=not_ready msg=%s", req.id, hub_llm_state());
	}
	return rc;
}

static int submit_simple(const struct shell *sh, int kind, int n)
{
	memset(&req, 0, sizeof(req));
	req.kind = kind;
	req.ctx_n = n;
	const int rc = hub_llm_submit(sh, &req);
	if (rc) {
		shell_print(sh, "HUBLLM/1 err id=0 code=%s msg=%s", rc == -EBUSY ? "busy" : "not_ready",
			    hub_llm_state());
	}
	return rc;
}

static int cmd_hub_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return submit_simple(sh, HUB_RESET, 0);
}

static int cmd_hub_ctx(const struct shell *sh, size_t argc, char **argv)
{
	const int n = (argc > 1 && !strncmp(argv[1], "n=", 2)) ? atoi(argv[1] + 2) : 0;
	if (n < 64 || n > 8192) {
		shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=ctx_n_64..8192");
		return -EINVAL;
	}
	return submit_simple(sh, HUB_CTX, n);
}

static int cmd_hub_clk(const struct shell *sh, size_t argc, char **argv)
{
	const int mhz = argc > 1 ? atoi(argv[1]) : 0;
	if (mhz != 1416 && mhz != 1104) {
		shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=clk_1416_or_1104");
		return -EINVAL;
	}
	return submit_simple(sh, HUB_CLK, mhz);
}

static int cmd_hub_temp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int c0 = 0, c1 = 0;
	const int e0 = rk3566_tsadc_get_mc(0, &c0), e1 = rk3566_tsadc_get_mc(1, &c1);
	const struct rk3566_tsadc_regs *b = rk3566_tsadc_boot_regs();
	shell_print(sh, "HUBLLM/1 temp cpu_mc=%d gpu_mc=%d err=%d,%d raw=%u,%u tshut_mc=%d "
		    "boot_auto_con=0x%08x boot_int_en=0x%08x boot_shut=0x%x,0x%x boot_data=0x%x,0x%x",
		    e0 ? -999999 : c0, e1 ? -999999 : c1, e0, e1,
		    rk3566_tsadc_raw(0), rk3566_tsadc_raw(1), rk3566_tsadc_tshut_mc(),
		    b ? b->auto_con : 0, b ? b->int_en : 0, b ? b->comp_shut0 : 0,
		    b ? b->comp_shut1 : 0, b ? b->data0 : 0, b ? b->data1 : 0);
	return 0;
}

static int cmd_hub_tshut(const struct shell *sh, size_t argc, char **argv)
{
	const int c = argc > 1 ? atoi(argv[1]) : -1;
	if (c != 0 && (c < 60 || c > 95)) {
		shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=tshut_0_or_60..95");
		return -EINVAL;
	}
	const int rc = rk3566_tsadc_set_tshut(c * 1000);
	shell_print(sh, "HUBLLM/1 tshut mc=%d rc=%d", rk3566_tsadc_tshut_mc(), rc);
	return rc;
}

static int cmd_hub_guard(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 4) {
		const int d = atoi(argv[1]), p = atoi(argv[2]), t = atoi(argv[3]);
		if (!(40 <= d && d < p && p < t && t <= 95)) {
			shell_print(sh, "HUBLLM/1 err id=0 code=bad_arg msg=40<=down<pause<stop<=95");
			return -EINVAL;
		}
		hub_llm_set_guard(d, p, t);
	}
	int d, p, t;
	hub_llm_get_guard(&d, &p, &t);
	shell_print(sh, "HUBLLM/1 guard down=%d pause=%d stop=%d tshut_mc=%d", d, p, t,
		    rk3566_tsadc_tshut_mc());
	return 0;
}

static int cmd_hub_abort(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	hub_llm_abort();
	shell_print(sh, "HUBLLM/1 abort_ack");
	return 0;
}

static int cpu_mc(void)
{
	int mc = 0;
	return rk3566_tsadc_get_mc(0, &mc) ? -999999 : mc;
}

static int cmd_hub_info(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t id[16];
	char hex[33] = "-";
	const ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	if (n > 0) {
		for (ssize_t i = 0; i < n && i < 16; i++) {
			static const char hx[] = "0123456789abcdef";
			hex[2 * i] = hx[id[i] >> 4];
			hex[2 * i + 1] = hx[id[i] & 15];
			hex[2 * i + 2] = '\0';
		}
	}
	const bool kv = (argc > 1 && !strcmp(argv[1], "--kv"));
	if (kv) {
		/* F3 — 모르는 값은 '-' (키는 빼지 않는다). model_hash·boot_slot 은 다음 판 */
		shell_print(sh, "FW_INFO fmt=1 deviceid=%s role=hub release=%s build=%s%s "
			    "variant=hubllm board=rk3566 model_hash=%s model_smp=%s boot_slot=%s "
			    "llm=%s n_ctx=%d ctx_kib=%d n_past=%d temp_mc=%d",
			    hex, APP_VERSION_STRING, STRINGIFY(APP_BUILD_VERSION), MORU_GIT_DIRTY,
			    hub_llm_model_hash(), hub_llm_model_smp(), rk3566_boot_slot(),
			    hub_llm_state(), hub_llm_n_ctx(), hub_llm_ctx_kib(), hub_llm_n_past(),
			    cpu_mc());
	} else {
		shell_print(sh, "hubllm %s (build %s%s) · 칩 %s · 부팅 슬롯 %s", APP_VERSION_STRING,
			    STRINGIFY(APP_BUILD_VERSION), MORU_GIT_DIRTY, hex, rk3566_boot_slot());
		shell_print(sh, "  LLM  %s · 적재 %d ms · n_ctx %d · 문맥 %d KiB · 대화 %d 토큰",
			    hub_llm_state(), hub_llm_load_ms(), hub_llm_n_ctx(), hub_llm_ctx_kib(),
			    hub_llm_n_past());
		shell_print(sh, "  모델 머리 %s", hub_llm_model_hash());
		shell_print(sh, "  모델 표본 %s", hub_llm_model_smp());
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_hub,
	SHELL_CMD_ARG(gen, NULL, "gen id=<n> max=<n> b64=<글> [special=1] | ids=<a,b,…>",
		      cmd_hub_gen, 2, 15),
	SHELL_CMD(reset, NULL, "KV(대화) 비우기", cmd_hub_reset),
	SHELL_CMD_ARG(ctx, NULL, "ctx n=<n> — 문맥 다시 만들기", cmd_hub_ctx, 2, 0),
	SHELL_CMD_ARG(clk, NULL, "clk 1416|1104 — 코어 클럭", cmd_hub_clk, 2, 0),
	SHELL_CMD(temp, NULL, "칩 온도 (TSADC) + 셧다운 문턱 + 부트로더가 남긴 설정", cmd_hub_temp),
	SHELL_CMD_ARG(tshut, NULL, "tshut <°C> — 하드웨어 셧다운 문턱 (0 끔, 60..95)", cmd_hub_tshut, 2, 0),
	SHELL_CMD_ARG(guard, NULL, "guard [down pause stop] — 소프트웨어 보호 (°C)", cmd_hub_guard, 1, 3),
	SHELL_CMD(abort, NULL, "생성 중단 (토큰 사이에서)", cmd_hub_abort),
	SHELL_CMD_ARG(info, NULL, "상태 · --kv 는 FW_INFO 한 줄", cmd_hub_info, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(hub, &sub_hub, "MoruLab 허브 LLM (HUBLLM/1)", NULL);
