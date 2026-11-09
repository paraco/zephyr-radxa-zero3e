/* hubml 배포 — include/hubml/deploy.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/deploy.h>
#include <hubml/sha256.h>

LOG_MODULE_DECLARE(hubml);

#define WIN 4
#define RTO_TICKS 3
#define MAX_RESEND 200
#define MAXF 16384

static struct dep {
	bool used;
	uint64_t uid;
	uint16_t sid;
	uint32_t addr;
	uint16_t xfer;
	struct hubml_dep_status st;
	uint8_t sha[32];
	uint32_t num;
	int wait;		/* 응답 기다린 틱 */
	uint8_t file[MAXF];
} D[4];
static K_MUTEX_DEFINE(dmtx);
static uint16_t xfer_next = 1;
static uint32_t cmd_seq;

static struct dep *find(uint64_t uid, bool make)
{
	for (int i = 0; i < ARRAY_SIZE(D); i++) {
		if (D[i].used && D[i].uid == uid) {
			return &D[i];
		}
	}
	if (!make) {
		return NULL;
	}
	for (int i = 0; i < ARRAY_SIZE(D); i++) {
		if (!D[i].used || D[i].st.state == HUBML_DEP_DONE || D[i].st.state == HUBML_DEP_FAIL) {
			memset(&D[i].st, 0, sizeof D[i].st);
			D[i].used = true;
			D[i].uid = uid;
			return &D[i];
		}
	}
	return NULL;
}

static void send_cmd(struct dep *d, uint8_t op, const uint8_t *body, size_t n)
{
	static uint8_t pl[MCOL_MAX_PAYLOAD], out[MCOL_MAX_DGRAM];
	pl[0] = op;
	pl[1] = 0;
	sys_put_le16(d->xfer, pl + 2);
	if (n) {
		memcpy(pl + MCOL_CMD_HDR, body, n);
	}
	struct mcol_hdr h = { .type = MCOL_T_CMD, .edge_sid = d->sid, .stream = MCOL_S_CTRL, .seq = cmd_seq++,
			      .edge_ms = (uint64_t)hubml_time_now(), .len = (uint16_t)(MCOL_CMD_HDR + n) };
	const int m = mcol_encode(out, sizeof out, &h, pl);
	if (m > 0) {
		hubml_ingest_send(d->addr, out, (size_t)m);
	}
}

static void send_begin(struct dep *d)
{
	uint8_t b[44] = { 0 };
	b[0] = d->st.slot;
	b[1] = d->file[5];
	sys_put_le32(d->st.size, b + 4);
	sys_put_le32(d->num, b + 8);
	memcpy(b + 12, d->sha, 32);
	send_cmd(d, MCOL_OP_BEGIN, b, sizeof b);
}

static void send_window(struct dep *d)
{
	uint8_t b[4 + MCOL_CHUNK];
	uint32_t off = d->st.acked;
	for (int k = 0; k < WIN && off < d->st.size; k++) {
		const uint32_t n = MIN((uint32_t)MCOL_CHUNK, d->st.size - off);
		sys_put_le32(off, b);
		memcpy(b + 4, d->file + off, n);
		send_cmd(d, MCOL_OP_CHUNK, b, 4 + n);
		d->st.sent_chunks++;
		off += n;
	}
}

static void hexs(const uint8_t *s, char *o)
{
	static const char h[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		o[2 * i] = h[s[i] >> 4];
		o[2 * i + 1] = h[s[i] & 15];
	}
	o[64] = 0;
}

static void finish(struct dep *d, bool ok, int code)
{
	d->st.state = ok ? HUBML_DEP_DONE : HUBML_DEP_FAIL;
	d->st.code = code;
	char sh[65], fl[400];
	hexs(d->st.edge_sha, sh);
	snprintf(fl, sizeof fl, "\"uid\":\"%016llx\",\"model_id\":\"%s\",\"slot\":\"%s\",\"op\":\"%s\",\"result\":\"%s\","
		 "\"code\":%d,\"edge_model_sha256\":\"%s\",\"resends\":%u",
		 (unsigned long long)d->uid, d->st.model_id, d->st.slot ? "challenger" : "champion",
		 d->st.state == HUBML_DEP_DONE && d->st.size == 0 ? "activate" : "load", ok ? "ok" : "fail", code, sh,
		 d->st.resends);
	(void)hubml_ledger_append("deploy", "hub", fl, NULL, NULL);
	LOG_INF("배포 %s %s → %s (code %d)", d->st.model_id, d->st.slot ? "challenger" : "champion", ok ? "ok" : "fail", code);
}

int hubml_deploy_start(uint64_t uid, const char *model_id, uint8_t slot)
{
	if (slot > 1) {
		return -EINVAL;
	}
	if (slot == 0 && strcmp(hubml_alias_get(uid, HUBML_A_CHAMPION), model_id)) {
		return -EACCES;	/* champion 슬롯에 직접 넣으려면 champion 별칭(사람 서명)이 그 모델이어야 한다 */
	}
	k_mutex_lock(&dmtx, K_FOREVER);
	struct dep *d = find(uid, true);
	int rc = 0;
	if (!d) {
		rc = -EBUSY;
		goto out;
	}
	if (hubml_ingest_find(uid, &d->sid, &d->addr)) {
		d->used = false;
		rc = -ENOENT;
		goto out;
	}
	const int n = hubml_model_load(uid, model_id, d->file, sizeof d->file);
	if (n <= 0) {
		d->used = false;
		rc = n ? n : -ENOENT;
		goto out;
	}
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, d->file, (size_t)n);
	sha256_final(&s, d->sha);
	const char *c = strchr(model_id, ':');
	d->num = c ? (uint32_t)strtoul(c + 1, NULL, 10) : 0;
	strncpy(d->st.model_id, model_id, sizeof d->st.model_id - 1);
	d->st.slot = slot;
	d->st.size = (uint32_t)n;
	d->xfer = xfer_next++;
	d->st.state = HUBML_DEP_BEGIN;
	d->wait = 0;
	send_begin(d);
out:
	k_mutex_unlock(&dmtx);
	return rc;
}

int hubml_deploy_activate(uint64_t uid)
{
	k_mutex_lock(&dmtx, K_FOREVER);
	struct dep *d = find(uid, true);
	int rc = 0;
	if (!d || hubml_ingest_find(uid, &d->sid, &d->addr)) {
		rc = d ? -ENOENT : -EBUSY;
		goto out;
	}
	strncpy(d->st.model_id, hubml_alias_get(uid, HUBML_A_CHAMPION), sizeof d->st.model_id - 1);
	if (!d->st.model_id[0]) {
		rc = -EACCES;	/* champion 별칭이 없다 — 사람 승인 전 */
		d->used = false;
		goto out;
	}
	d->st.slot = 0;
	d->st.size = 0;
	d->xfer = xfer_next++;
	d->st.state = HUBML_DEP_ACT;
	d->wait = 0;
	send_cmd(d, MCOL_OP_ACTIVATE, NULL, 0);
out:
	k_mutex_unlock(&dmtx);
	return rc;
}

void hubml_on_ctrl(uint16_t sid, const struct mcol_hdr *h, const uint8_t *pl)
{
	if (h->type != MCOL_T_CMD_ACK || h->len < MCOL_CMD_ACK_LEN) {
		return;
	}
	const uint8_t op = pl[0];
	const int8_t st = (int8_t)pl[1];
	const uint16_t xfer = sys_get_le16(pl + 2);
	const uint32_t next = sys_get_le32(pl + 4);
	k_mutex_lock(&dmtx, K_FOREVER);
	struct dep *d = NULL;
	for (int i = 0; i < ARRAY_SIZE(D); i++) {
		if (D[i].used && D[i].sid == sid && D[i].xfer == xfer) {
			d = &D[i];
		}
	}
	if (!d) {
		goto out;
	}
	d->wait = 0;
	/* 오류는 지금 기다리는 단계의 응답이거나 Edge 가 알린 최종 결과(END·ACTIVATE)일 때만 받는다 —
	 * 늦게 도착한 앞 단계 응답의 오류로 전송을 끝내지 않는다 */
	const bool final_op = (op == MCOL_OP_END || op == MCOL_OP_ACTIVATE);
	const bool stage_op = (d->st.state == HUBML_DEP_BEGIN && op == MCOL_OP_BEGIN) ||
			      (d->st.state == HUBML_DEP_SEND && op == MCOL_OP_CHUNK);
	if (st && (final_op || stage_op)) {
		if (d->st.state != HUBML_DEP_DONE && d->st.state != HUBML_DEP_FAIL) {
			memcpy(d->st.edge_sha, pl + 16, 32);
			finish(d, false, st);
		}
		goto out;
	}
	if (st) {
		goto out;
	}
	switch (d->st.state) {
	case HUBML_DEP_BEGIN:
		if (op == MCOL_OP_BEGIN) {
			d->st.state = HUBML_DEP_SEND;
			d->st.acked = 0;
			send_window(d);
		}
		break;
	case HUBML_DEP_SEND:
		if (op == MCOL_OP_CHUNK && next > d->st.acked) {
			d->st.acked = next;
			if (d->st.acked >= d->st.size) {
				d->st.state = HUBML_DEP_END;
				send_cmd(d, MCOL_OP_END, NULL, 0);
			} else {
				send_window(d);
			}
		}
		break;
	case HUBML_DEP_END:
		if (op == MCOL_OP_END) {
			memcpy(d->st.edge_sha, pl + 16, 32);
			/* Edge 가 들고 있는 것이 우리가 보낸 것인가 — sha256 으로 끝까지 확인 */
			finish(d, !memcmp(d->st.edge_sha, d->sha, 32), memcmp(d->st.edge_sha, d->sha, 32) ? -3 : 0);
		}
		break;
	case HUBML_DEP_ACT:
		if (op == MCOL_OP_ACTIVATE) {
			memcpy(d->st.edge_sha, pl + 16, 32);
			d->st.state = HUBML_DEP_DONE;
			finish(d, true, 0);
		}
		break;
	default:
		break;
	}
out:
	k_mutex_unlock(&dmtx);
}

void hubml_deploy_tick(void)
{
	k_mutex_lock(&dmtx, K_FOREVER);
	for (int i = 0; i < ARRAY_SIZE(D); i++) {
		struct dep *d = &D[i];
		if (!d->used || d->st.state == HUBML_DEP_IDLE || d->st.state == HUBML_DEP_DONE ||
		    d->st.state == HUBML_DEP_FAIL) {
			continue;
		}
		if (++d->wait < RTO_TICKS) {
			continue;
		}
		d->wait = 0;
		if (++d->st.resends > MAX_RESEND) {
			finish(d, false, -ETIMEDOUT);
			continue;
		}
		switch (d->st.state) {
		case HUBML_DEP_BEGIN: send_begin(d); break;
		case HUBML_DEP_SEND: send_window(d); break;
		case HUBML_DEP_END: send_cmd(d, MCOL_OP_END, NULL, 0); break;
		case HUBML_DEP_ACT: send_cmd(d, MCOL_OP_ACTIVATE, NULL, 0); break;
		default: break;
		}
	}
	k_mutex_unlock(&dmtx);
}

int hubml_deploy_status(uint64_t uid, struct hubml_dep_status *out)
{
	k_mutex_lock(&dmtx, K_FOREVER);
	struct dep *d = find(uid, false);
	if (d) {
		*out = d->st;
	}
	k_mutex_unlock(&dmtx);
	return d ? 0 : -ENOENT;
}
