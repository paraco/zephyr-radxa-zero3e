/* .xmb 쓰기·읽기·평가 — include/hubml/xmb.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <hubml/xmb.h>

/* 펌웨어 sigmoidf_safe 와 같다 (넘침을 피하는 분기) */
static float sigmoidf_safe(float z)
{
	if (z >= 0.0f) {
		const float e = expf(-z);
		return 1.0f / (1.0f + e);
	}
	const float e = expf(z);
	return e / (1.0f + e);
}

float xmb_predict(const struct xmb_model *m, const float *f)
{
	float s = 0.0f;
	for (uint16_t t = 0; t < m->ntree; t++) {
		int32_t idx = (int32_t)m->root[t];
		int depth = 0;
		while (idx >= 0 && idx < (int32_t)m->nnode && depth < 64) {
			const struct xmb_node *n = &m->node[idx];
			if (n->is_leaf) {
				s += n->val;
				break;
			}
			const float x = (n->feat >= 0 && n->feat < m->contract) ? f[n->feat] : 0.0f;
			idx = (x < n->thr) ? n->left : n->right;	/* strict < */
			depth++;
		}
	}
	const float bl = logf(m->base / (1.0f - m->base + 1e-7f));
	return sigmoidf_safe(bl + s);
}

static void putf(uint8_t *p, float v) { uint32_t u; memcpy(&u, &v, 4); sys_put_le32(u, p); }
static float getf(const uint8_t *p) { const uint32_t u = sys_get_le32(p); float v; memcpy(&v, &u, 4); return v; }

int xmb_size(const struct xmb_model *m)
{
	return XMB_HDR_LEN + 4 * m->ntree + 16 * (int)m->nnode + XMB_NGOLDEN * (m->nfeat * 4 + 4) + 4;
}

int xmb_write(const struct xmb_model *m, uint8_t *out, size_t cap)
{
	const int sz = xmb_size(m);
	if (m->ntree > XMB_MAX_TREE || m->nnode > XMB_MAX_NODE || m->nfeat > XMB_MAX_FEAT || (size_t)sz > cap) {
		return -EMSGSIZE;
	}
	memset(out, 0, XMB_HDR_LEN);
	memcpy(out, "XMB1", 4);
	out[4] = 1;
	out[5] = m->kind;
	out[6] = m->nfeat;
	out[7] = m->target;
	sys_put_le16(m->ntree, out + 8);
	sys_put_le32(m->nnode, out + 10);
	putf(out + 14, m->base);
	putf(out + 18, m->thr);
	sys_put_le16(m->contract, out + 22);
	sys_put_le32(m->train_ts, out + 24);
	memcpy(out + 28, m->meta, 24);
	/* 52·56 veto = 0 (끔) */
	sys_put_le32(crc32_ieee(out, 60), out + 60);
	uint8_t *p = out + XMB_HDR_LEN;
	for (uint16_t t = 0; t < m->ntree; t++, p += 4) {
		sys_put_le32(m->root[t], p);
	}
	for (uint32_t i = 0; i < m->nnode; i++, p += 16) {
		const struct xmb_node *n = &m->node[i];
		sys_put_le16((uint16_t)n->feat, p);
		sys_put_le16((uint16_t)n->left, p + 2);
		sys_put_le16((uint16_t)n->right, p + 4);
		p[6] = n->is_leaf;
		p[7] = 0;
		putf(p + 8, n->thr);
		putf(p + 12, n->val);
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		for (int k = 0; k <= m->nfeat; k++, p += 4) {
			putf(p, m->golden[g][k]);
		}
	}
	sys_put_le32(crc32_ieee(out + XMB_HDR_LEN, (size_t)(p - out - XMB_HDR_LEN)), p);
	return sz;
}

int xmb_parse(const uint8_t *in, size_t n, struct xmb_model *m, float tol)
{
	if (n < XMB_HDR_LEN + 4 || memcmp(in, "XMB1", 4)) {
		return -1;
	}
	if (crc32_ieee(in, 60) != sys_get_le32(in + 60)) {
		return -2;
	}
	m->kind = in[5];
	m->nfeat = in[6];
	m->target = in[7];
	m->ntree = sys_get_le16(in + 8);
	m->nnode = sys_get_le32(in + 10);
	m->base = getf(in + 14);
	m->thr = getf(in + 18);
	m->contract = sys_get_le16(in + 22);
	m->train_ts = sys_get_le32(in + 24);
	memcpy(m->meta, in + 28, 24);
	if (m->kind != 1) {
		return -3;
	}
	if (m->nfeat != m->contract || m->nfeat > XMB_MAX_FEAT) {
		return -4;
	}
	if (m->ntree > XMB_MAX_TREE || m->nnode > XMB_MAX_NODE) {
		return -5;
	}
	if ((size_t)xmb_size(m) != n) {
		return -5;
	}
	const uint8_t *p = in + XMB_HDR_LEN;
	const size_t body = n - XMB_HDR_LEN - 4;
	if (crc32_ieee(p, body) != sys_get_le32(in + n - 4)) {
		return -6;
	}
	for (uint16_t t = 0; t < m->ntree; t++, p += 4) {
		m->root[t] = sys_get_le32(p);
	}
	for (uint32_t i = 0; i < m->nnode; i++, p += 16) {
		struct xmb_node *d = &m->node[i];
		d->feat = (int16_t)sys_get_le16(p);
		d->left = (int16_t)sys_get_le16(p + 2);
		d->right = (int16_t)sys_get_le16(p + 4);
		d->is_leaf = p[6];
		d->thr = getf(p + 8);
		d->val = getf(p + 12);
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		for (int k = 0; k <= m->nfeat; k++, p += 4) {
			m->golden[g][k] = getf(p);
		}
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		const float got = xmb_predict(m, m->golden[g]);
		if (fabsf(got - m->golden[g][m->nfeat]) > tol) {
			return -7;
		}
	}
	return 0;
}

/* ── kind=3 통계 기준선 ───────────────────────────────────────────────── */
#define TRI(n) ((n) * ((n) + 1) / 2)

float xmd_score(const struct xmd_model *m, const float *x, int *zmax)
{
	int r = 0;
	while (r < m->nreg - 1 && x[m->reg_feat] >= m->edges[r]) {
		r++;
	}
	const int n = m->nfeat;
	const float *L = m->reg[r].L;
	float y[XMB_MAX_FEAT];
	float d2 = 0.0f, zm = -1.0f;
	int o = 0;	/* 행 i 의 시작 = i(i+1)/2 */
	for (int i = 0; i < n; i++) {
		float t = x[i] - m->reg[r].mean[i];
		if (zmax && m->reg[r].sd[i] > 0.0f) {
			const float z = fabsf(t) / m->reg[r].sd[i];
			if (z > zm) {
				zm = z;
				*zmax = i;
			}
		}
		for (int k = 0; k < i; k++) {
			t -= L[o + k] * y[k];
		}
		y[i] = t / L[o + i];
		d2 += y[i] * y[i];
		o += i + 1;
	}
	return d2 / m->reg[r].thr_d2;
}

int xmd_size(const struct xmd_model *m)
{
	const int n = m->nfeat;
	return XMB_HDR_LEN + 4 + 4 * (XMD_MAX_REG - 1) + m->nreg * 4 * (2 * n + TRI(n) + 1) +
	       XMB_NGOLDEN * 4 * (n + 1) + 4;
}

int xmd_write(const struct xmd_model *m, uint8_t *out, size_t cap)
{
	const int sz = xmd_size(m), n = m->nfeat;
	if (n > XMB_MAX_FEAT || m->nreg < 1 || m->nreg > XMD_MAX_REG || (size_t)sz > cap) {
		return -EMSGSIZE;
	}
	memset(out, 0, XMB_HDR_LEN);
	memcpy(out, "XMB1", 4);
	out[4] = 1;
	out[5] = 3;
	out[6] = (uint8_t)n;
	out[7] = m->target;
	putf(out + 18, 1.0f);
	sys_put_le16(m->contract, out + 22);
	sys_put_le32(m->train_ts, out + 24);
	memcpy(out + 28, m->meta, 24);
	sys_put_le32(crc32_ieee(out, 60), out + 60);
	uint8_t *p = out + XMB_HDR_LEN;
	p[0] = m->nreg;
	p[1] = m->reg_feat;
	p[2] = p[3] = 0;
	p += 4;
	for (int i = 0; i < XMD_MAX_REG - 1; i++, p += 4) {
		putf(p, m->edges[i]);
	}
	for (int r = 0; r < m->nreg; r++) {
		for (int i = 0; i < n; i++, p += 4) { putf(p, m->reg[r].mean[i]); }
		for (int i = 0; i < n; i++, p += 4) { putf(p, m->reg[r].sd[i]); }
		for (int i = 0; i < TRI(n); i++, p += 4) { putf(p, m->reg[r].L[i]); }
		putf(p, m->reg[r].thr_d2);
		p += 4;
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		for (int k = 0; k <= n; k++, p += 4) {
			putf(p, m->golden[g][k]);
		}
	}
	sys_put_le32(crc32_ieee(out + XMB_HDR_LEN, (size_t)(p - out - XMB_HDR_LEN)), p);
	return sz;
}

int xmd_parse(const uint8_t *in, size_t n0, struct xmd_model *m, float tol)
{
	if (n0 < XMB_HDR_LEN + 8 || memcmp(in, "XMB1", 4)) {
		return -1;
	}
	if (crc32_ieee(in, 60) != sys_get_le32(in + 60)) {
		return -2;
	}
	if (in[5] != 3) {
		return -3;
	}
	m->nfeat = in[6];
	m->target = in[7];
	m->contract = sys_get_le16(in + 22);
	m->train_ts = sys_get_le32(in + 24);
	memcpy(m->meta, in + 28, 24);
	const uint8_t *p = in + XMB_HDR_LEN;
	m->nreg = p[0];
	m->reg_feat = p[1];
	if (m->nfeat != m->contract || m->nfeat > XMB_MAX_FEAT || m->reg_feat >= m->nfeat) {
		return -4;
	}
	if (m->nreg < 1 || m->nreg > XMD_MAX_REG || (size_t)xmd_size(m) != n0) {
		return -5;
	}
	if (crc32_ieee(p, n0 - XMB_HDR_LEN - 4) != sys_get_le32(in + n0 - 4)) {
		return -6;
	}
	const int n = m->nfeat;
	p += 4;
	for (int i = 0; i < XMD_MAX_REG - 1; i++, p += 4) {
		m->edges[i] = getf(p);
	}
	for (int r = 0; r < m->nreg; r++) {
		for (int i = 0; i < n; i++, p += 4) { m->reg[r].mean[i] = getf(p); }
		for (int i = 0; i < n; i++, p += 4) { m->reg[r].sd[i] = getf(p); }
		for (int i = 0; i < TRI(n); i++, p += 4) { m->reg[r].L[i] = getf(p); }
		m->reg[r].thr_d2 = getf(p);
		p += 4;
		if (!(m->reg[r].thr_d2 > 0.0f)) {
			return -5;
		}
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		for (int k = 0; k <= n; k++, p += 4) {
			m->golden[g][k] = getf(p);
		}
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		const float got = xmd_score(m, m->golden[g], NULL);
		if (fabsf(got - m->golden[g][n]) > tol * fmaxf(1.0f, fabsf(got))) {
			return -7;
		}
	}
	return 0;
}
