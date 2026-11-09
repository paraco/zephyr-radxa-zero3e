/* MCOL/1 부호화·복호 — include/hubml/mcol.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <hubml/mcol.h>

int mcol_encode(uint8_t *out, size_t cap, const struct mcol_hdr *h, const void *payload)
{
	const size_t total = MCOL_HDR_LEN + (size_t)h->len + MCOL_CRC_LEN;
	if (h->len > MCOL_MAX_PAYLOAD || total > cap) {
		return -EMSGSIZE;
	}
	out[0] = 'M';
	out[1] = 'C';
	out[2] = MCOL_VER;
	out[3] = h->type;
	sys_put_le16(h->edge_sid, out + 4);
	out[6] = h->stream;
	out[7] = h->flags;
	sys_put_le32(h->seq, out + 8);
	sys_put_le64(h->edge_ms, out + 12);
	sys_put_le16(h->len, out + 20);
	sys_put_le16(h->boot_id, out + 22);
	if (h->len) {
		memcpy(out + MCOL_HDR_LEN, payload, h->len);
	}
	sys_put_le32(crc32_ieee(out, MCOL_HDR_LEN + h->len), out + MCOL_HDR_LEN + h->len);
	return (int)total;
}

int mcol_decode(const uint8_t *in, size_t n, struct mcol_hdr *h, const uint8_t **payload)
{
	if (n < MCOL_HDR_LEN + MCOL_CRC_LEN || in[0] != 'M' || in[1] != 'C' || in[2] != MCOL_VER) {
		return -EBADMSG;
	}
	const uint16_t len = sys_get_le16(in + 20);
	if ((size_t)MCOL_HDR_LEN + len + MCOL_CRC_LEN != n || len > MCOL_MAX_PAYLOAD) {
		return -EBADMSG;
	}
	if (crc32_ieee(in, MCOL_HDR_LEN + len) != sys_get_le32(in + MCOL_HDR_LEN + len)) {
		return -EBADMSG;
	}
	h->type = in[3];
	h->edge_sid = sys_get_le16(in + 4);
	h->stream = in[6];
	h->flags = in[7];
	h->seq = sys_get_le32(in + 8);
	h->edge_ms = sys_get_le64(in + 12);
	h->len = len;
	h->boot_id = sys_get_le16(in + 22);
	if (h->stream >= MCOL_NSTREAM) {
		return -EBADMSG;
	}
	*payload = in + MCOL_HDR_LEN;
	return 0;
}

void mcol_hello_put(uint8_t *p, const struct mcol_hello *m)
{
	sys_put_le64(m->uid, p);
	sys_put_le32(m->fw, p + 8);
	sys_put_le16(m->contract, p + 12);
	p[14] = m->nfeat;
	p[15] = 0;
	sys_put_le32(m->sensor_hash, p + 16);
	sys_put_le32(m->schema_hash, p + 20);
}

void mcol_hello_get(const uint8_t *p, struct mcol_hello *m)
{
	m->uid = sys_get_le64(p);
	m->fw = sys_get_le32(p + 8);
	m->contract = sys_get_le16(p + 12);
	m->nfeat = p[14];
	m->sensor_hash = sys_get_le32(p + 16);
	m->schema_hash = sys_get_le32(p + 20);
}

int mcol_feat_get(const uint8_t *pl, uint16_t len, int i, float *x, int nmax, uint8_t *regime, uint8_t *quality)
{
	if (len < MCOL_FEAT_HDR) {
		return -EBADMSG;
	}
	const int n = pl[0], c = pl[1];
	const size_t vl = mcol_feat_vec_len(c);
	if (i >= n || c > nmax || MCOL_FEAT_HDR + (size_t)n * vl > len) {
		return -EINVAL;
	}
	const uint8_t *v = pl + MCOL_FEAT_HDR + (size_t)i * vl;
	for (int k = 0; k < c; k++) {
		const uint32_t u = sys_get_le32(v + 4 * k);
		memcpy(&x[k], &u, 4);
	}
	if (regime) {
		*regime = v[4 * c];
	}
	if (quality) {
		*quality = v[4 * c + 1];
	}
	return 0;
}

int mcol_feat_put(uint8_t *pl, size_t cap, int contract, int n, const float *xs, const uint8_t *quality)
{
	const size_t vl = mcol_feat_vec_len(contract);
	if (n > MCOL_FEAT_MAX_N || MCOL_FEAT_HDR + (size_t)n * vl > cap) {
		return -EMSGSIZE;
	}
	pl[0] = (uint8_t)n;
	pl[1] = (uint8_t)contract;
	pl[2] = pl[3] = 0;
	for (int i = 0; i < n; i++) {
		uint8_t *v = pl + MCOL_FEAT_HDR + (size_t)i * vl;
		for (int k = 0; k < contract; k++) {
			uint32_t u;
			memcpy(&u, &xs[i * contract + k], 4);
			sys_put_le32(u, v + 4 * k);
		}
		v[4 * contract] = 0;
		v[4 * contract + 1] = quality ? quality[i] : 0;
		sys_put_le16(1000, v + 4 * contract + 2);
	}
	return (int)(MCOL_FEAT_HDR + (size_t)n * vl);
}
