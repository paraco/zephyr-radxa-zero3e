/* hubml Edge 쪽 모델 받기 — include/hubml/edge.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <hubml/mcol.h>
#include <hubml/edge.h>
#include <hubml/sha256.h>

void hubml_edge_init(struct hubml_edge *e, uint16_t contract)
{
	memset(e, 0, sizeof(*e));
	e->contract = contract;
}

static int reply_put(struct hubml_edge *e, uint8_t *r, uint8_t op, int8_t st, uint16_t xfer, uint8_t slot)
{
	memset(r, 0, MCOL_CMD_ACK_LEN);
	r[0] = op;
	r[1] = (uint8_t)st;
	sys_put_le16(xfer, r + 2);
	sys_put_le32(e->rx_on ? e->rx_off : 0, r + 4);
	r[8] = slot;
	if (slot < 2 && e->slot[slot].valid) {
		sys_put_le32(e->slot[slot].num, r + 12);
		memcpy(r + 16, e->slot[slot].sha, 32);
	}
	return MCOL_CMD_ACK_LEN;
}

int hubml_edge_on_cmd(struct hubml_edge *e, const uint8_t *pl, uint16_t len, uint8_t *r)
{
	if (len < MCOL_CMD_HDR) {
		return reply_put(e, r, 0, -1, 0, 0xff);
	}
	const uint8_t op = pl[0];
	const uint16_t xfer = sys_get_le16(pl + 2);
	if (xfer && xfer == e->done_xfer && !(e->rx_on && e->rx_xfer == xfer)) {
		/* 이미 끝낸 전송에 관한 메시지 — 그 결과를 END(또는 ACTIVATE) 응답으로 다시 알린다 */
		return reply_put(e, r, op == MCOL_OP_ACTIVATE ? MCOL_OP_ACTIVATE : MCOL_OP_END, e->done_status, xfer,
				 e->done_slot);
	}
	switch (op) {
	case MCOL_OP_BEGIN: {
		if (len < MCOL_CMD_HDR + 44 || pl[4] > 1) {
			return reply_put(e, r, op, -1, xfer, 0xff);
		}
		const uint32_t size = sys_get_le32(pl + 8);
		if (size > sizeof(e->buf) || size < XMB_HDR_LEN) {
			return reply_put(e, r, op, -2, xfer, pl[4]);
		}
		if (!(e->rx_on && e->rx_xfer == xfer)) {	/* 같은 BEGIN 이 다시 오면(응답 유실) 처음부터 다시 하지 않는다 */
			e->rx_on = true;
			e->rx_xfer = xfer;
			e->rx_slot = pl[4];
			e->rx_size = size;
			e->rx_num = sys_get_le32(pl + 12);
			e->rx_off = 0;
			memcpy(e->rx_sha, pl + 16, 32);
		}
		return reply_put(e, r, op, 0, xfer, e->rx_slot);
	}
	case MCOL_OP_CHUNK: {
		if (!e->rx_on || e->rx_xfer != xfer || len < MCOL_CMD_HDR + 4) {
			return reply_put(e, r, op, -1, xfer, 0xff);
		}
		const uint32_t off = sys_get_le32(pl + 4), n = len - MCOL_CMD_HDR - 4;
		/* go-back-N: 기다리는 자리만 받는다 (앞선 것은 버리고 next_off 를 알려 준다) */
		if (off == e->rx_off && off + n <= e->rx_size) {
			memcpy(e->buf + off, pl + 8, n);
			e->rx_off += n;
		}
		return reply_put(e, r, op, 0, xfer, e->rx_slot);
	}
	case MCOL_OP_END: {
		if (!e->rx_on || e->rx_xfer != xfer) {
			return reply_put(e, r, op, -1, xfer, 0xff);
		}
		if (e->rx_off != e->rx_size) {
			return reply_put(e, r, op, -1, xfer, e->rx_slot);
		}
		uint8_t d[32];
		struct sha256 s;
		sha256_init(&s);
		sha256_update(&s, e->buf, e->rx_size);
		sha256_final(&s, d);
		int8_t st = 0;
		struct hubml_edge_slot *sl = &e->slot[e->rx_slot];
		if (memcmp(d, e->rx_sha, 32)) {
			st = -3;
		} else if (e->buf[5] != 3) {
			st = -4;	/* Edge 는 kind 3(통계 기준선)만 운영한다 */
		} else {
			static struct xmd_model tmp;	/* 검사가 끝난 뒤에만 슬롯을 바꾼다 */
			if (xmd_parse(e->buf, e->rx_size, &tmp, 1e-5f)) {
				st = -4;
			} else if (tmp.contract != e->contract) {
				st = -5;
			} else {
				sl->m = tmp;
				sl->valid = true;
				sl->num = e->rx_num;
				sl->size = e->rx_size;
				memcpy(sl->sha, d, 32);
			}
		}
		e->rx_on = false;
		e->done_xfer = xfer;
		e->done_status = st;
		e->done_slot = e->rx_slot;
		st ? e->n_fail++ : e->n_ok++;
		return reply_put(e, r, op, st, xfer, e->rx_slot);
	}
	case MCOL_OP_ACTIVATE: {
		if (!e->slot[1].valid) {
			return reply_put(e, r, op, -6, xfer, 0);
		}
		/* 맞바꾼다 — 옛 champion 이 challenger 로 남아 되돌리기(다시 ACTIVATE)에 쓰인다 */
		static struct hubml_edge_slot t;
		t = e->slot[0];
		e->slot[0] = e->slot[1];
		e->slot[1] = t;
		e->done_xfer = xfer;	/* 다시 온 ACTIVATE 로 두 번 맞바꾸지 않게 */
		e->done_status = 0;
		e->done_slot = 0;
		return reply_put(e, r, op, 0, xfer, 0);
	}
	default:
		return reply_put(e, r, op, -1, xfer, 0xff);
	}
}

void hubml_edge_score(const struct hubml_edge *e, const float *x, float *champ, float *chall)
{
	*champ = e->slot[0].valid ? xmd_score(&e->slot[0].m, x, NULL) : -1.0f;
	*chall = e->slot[1].valid ? xmd_score(&e->slot[1].m, x, NULL) : -1.0f;
}
