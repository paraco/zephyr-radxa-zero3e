/* hubml 학습 — include/hubml/train.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <hubml/ingest.h>
#include <hubml/train.h>

#define NF HUBML_NFEAT

/* ── 데이터 읽기 ──────────────────────────────────────────────────────── */
struct each_ctx {
	hubml_vec_cb cb;
	void *user;
	uint32_t idx;
};

static void each_rec(const struct mcol_hdr *h, const uint8_t *pl, void *user)
{
	struct each_ctx *c = user;
	if (h->type != MCOL_T_FEAT || h->len < MCOL_FEAT_HDR) {
		return;
	}
	const int n = pl[0];
	for (int i = 0; i < n; i++) {
		float x[NF];
		uint8_t reg, q;
		if (mcol_feat_get(pl, h->len, i, x, NF, &reg, &q) || pl[1] != NF || q != 0) {
			continue;
		}
		c->cb(x, c->idx++, c->user);
	}
}

/* ⚠ hubml_store_scan 은 파일 안 순서로 넘긴다. 같은 파일 안에서 seq 가 뒤섞일 수 있지만
 *   (재전송) 하루 단위 학습·평가 분할에는 영향이 작다 — 시각 순서가 꼭 필요하면 정렬 단계를 더한다. */
int hubml_feat_each(const char *root, uint64_t uid, const char days[][9], int ndays, hubml_vec_cb cb, void *user)
{
	struct each_ctx c = { .cb = cb, .user = user };
	for (int d = 0; d < ndays; d++) {
		const int rc = hubml_store_scan(root, uid, days[d], MCOL_S_FEAT, each_rec, &c, NULL);
		if (rc < 0 && rc != -ENOENT) {
			return rc;
		}
	}
	return (int)c.idx;
}

/* ── 작은 난수 (결정적) ───────────────────────────────────────────────── */
static uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static double urand(uint32_t *s) { return (xs32(s) >> 8) * (1.0 / 16777216.0); }

/* ── 통계 기준선 ─────────────────────────────────────────────────────── */
int hubml_baseline_regime(const struct hubml_baseline *bl, const float *x)
{
	int r = 0;
	while (r < bl->nreg - 1 && x[HUBML_REG_FEAT] >= bl->reg_edge[r]) {
		r++;
	}
	return r;
}

/* 촐레스키 (하삼각 L, LLᵀ = A). 대각이 작으면 능선을 더한다 */
static int cholesky(double *A, int n)
{
	for (int j = 0; j < n; j++) {
		double s = A[j * n + j];
		for (int k = 0; k < j; k++) {
			s -= A[j * n + k] * A[j * n + k];
		}
		if (s <= 0) {
			return -EDOM;
		}
		A[j * n + j] = sqrt(s);
		for (int i = j + 1; i < n; i++) {
			double t = A[i * n + j];
			for (int k = 0; k < j; k++) {
				t -= A[i * n + k] * A[j * n + k];
			}
			A[i * n + j] = t / A[j * n + j];
		}
		for (int k = j + 1; k < n; k++) {
			A[j * n + k] = 0;
		}
	}
	return 0;
}

double hubml_baseline_d2(const struct hubml_baseline *bl, const float *x, int *zmax_idx)
{
	const int r = hubml_baseline_regime(bl, x);
	const double *L = bl->reg[r].L;
	double y[NF], zmax = -1;
	/* L y = (x - μ) → d² = |y|² */
	double d2 = 0;
	for (int i = 0; i < NF; i++) {
		double t = (double)x[i] - bl->reg[r].mean[i];
		const double z = bl->reg[r].sd[i] > 0 ? fabs(t) / bl->reg[r].sd[i] : 0;
		if (zmax_idx && z > zmax) {
			zmax = z;
			*zmax_idx = i;
		}
		for (int k = 0; k < i; k++) {
			t -= L[i * NF + k] * y[k];
		}
		y[i] = t / L[i * NF + i];
		d2 += y[i] * y[i];
	}
	return d2;
}

/* ── Isolation Forest ────────────────────────────────────────────────── */
static double c_n(double n)
{
	if (n <= 1) {
		return 0;
	}
	if (n == 2) {
		return 1;
	}
	return 2.0 * (log(n - 1) + 0.5772156649) - 2.0 * (n - 1) / n;
}

struct build {
	struct xmb_model *m;
	float (*smp)[NF];	/* 이 트리의 표본 */
	int *ix;
	uint32_t seed;
	double leaf_scale;	/* k / T */
	double cpsi;
	int maxd;
	int err;
};

static int new_node(struct build *b)
{
	if (b->m->nnode >= XMB_MAX_NODE) {
		b->err = -ENOSPC;
		return -1;
	}
	return (int)b->m->nnode++;
}

static int grow(struct build *b, int *ix, int n, int depth)
{
	const int id = new_node(b);
	if (id < 0) {
		return -1;
	}
	struct xmb_node *nd = &b->m->node[id];
	memset(nd, 0, sizeof(*nd));
	nd->feat = -1;
	nd->left = nd->right = -1;
	if (depth < b->maxd && n > 1) {
		/* 값이 갈리는 특징을 무작위로 고른다 (최대 NF 번 시도) */
		for (int tries = 0; tries < 2 * NF; tries++) {
			const int f = (int)(xs32(&b->seed) % NF);
			float lo = b->smp[ix[0]][f], hi = lo;
			for (int i = 1; i < n; i++) {
				const float v = b->smp[ix[i]][f];
				lo = v < lo ? v : lo;
				hi = v > hi ? v : hi;
			}
			if (!(hi > lo)) {
				continue;
			}
			float thr = (float)(lo + urand(&b->seed) * (hi - lo));
			if (!(thr > lo)) {
				thr = nextafterf(lo, hi);	/* strict < 로 왼쪽이 비지 않게 */
			}
			int nl = 0;
			for (int i = 0; i < n; i++) {
				if (b->smp[ix[i]][f] < thr) {
					const int t = ix[i];
					ix[i] = ix[nl];
					ix[nl++] = t;
				}
			}
			if (nl == 0 || nl == n) {
				continue;
			}
			nd->feat = (int16_t)f;
			nd->thr = thr;
			const int l = grow(b, ix, nl, depth + 1);
			const int r = grow(b, ix + nl, n - nl, depth + 1);
			nd = &b->m->node[id];
			nd->left = (int16_t)l;
			nd->right = (int16_t)r;
			return id;
		}
	}
	nd->is_leaf = 1;
	nd->val = (float)(b->leaf_scale * (b->cpsi - (depth + c_n(n))));
	return id;
}

/* ── 학습 패스 ────────────────────────────────────────────────────────── */
struct p1 {
	const struct hubml_train_cfg *c;
	uint32_t n_train;
	struct hubml_baseline *bl;
	double (*sxx)[NF * NF];		/* 구간별 교차곱 합 */
	float (*res)[NF];		/* 저수지 표본 */
	uint32_t nres, seen;
	uint32_t seed;
};

static void p1_cb(const float *x, uint32_t idx, void *u)
{
	struct p1 *p = u;
	if (idx >= p->n_train) {
		return;
	}
	const int r = hubml_baseline_regime(p->bl, x);
	struct hubml_baseline *bl = p->bl;
	bl->reg[r].n++;
	for (int i = 0; i < NF; i++) {
		bl->reg[r].mean[i] += x[i];
		for (int j = 0; j <= i; j++) {
			p->sxx[r][i * NF + j] += (double)x[i] * x[j];
		}
	}
	/* 저수지 표본 (알고리즘 R) */
	p->seen++;
	if (p->seen <= p->nres) {
		memcpy(p->res[p->seen - 1], x, sizeof(float) * NF);
	} else {
		const uint32_t j = xs32(&p->seed) % p->seen;
		if (j < p->nres) {
			memcpy(p->res[j], x, sizeof(float) * NF);
		}
	}
}

#define HB 4096
struct p2 {
	uint32_t n_train;
	const struct hubml_baseline *bl;
	const struct xmb_model *xm;
	uint32_t hd2[HUBML_NREG][HB];	/* log2 d² 히스토그램 */
	uint32_t hp[HB];		/* 확률 히스토그램 */
	uint32_t nreg[HUBML_NREG];
	float gold[XMB_NGOLDEN][NF];
	uint32_t gold_at[XMB_NGOLDEN];
};

static int d2_bin(double d2) { int b = (int)((log2(d2 + 1e-12) + 10.0) * 100.0); return b < 0 ? 0 : (b >= HB ? HB - 1 : b); }
static double d2_of_bin(int b) { return pow(2.0, (b + 1) / 100.0 - 10.0); }

static void p2_cb(const float *x, uint32_t idx, void *u)
{
	struct p2 *p = u;
	if (idx >= p->n_train) {
		return;
	}
	const int r = hubml_baseline_regime(p->bl, x);
	p->hd2[r][d2_bin(hubml_baseline_d2(p->bl, x, NULL))]++;
	p->nreg[r]++;
	const float pr = xmb_predict(p->xm, x);
	int b = (int)(pr * (HB - 1));
	p->hp[b < 0 ? 0 : (b >= HB ? HB - 1 : b)]++;
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		if (idx == p->gold_at[g]) {
			memcpy(p->gold[g], x, sizeof(float) * NF);
		}
	}
}

static int quant_bin(const uint32_t *h, uint32_t n, float q)
{
	const uint64_t want = (uint64_t)ceil((double)n * q);
	uint64_t acc = 0;
	for (int b = 0; b < HB; b++) {
		acc += h[b];
		if (acc >= want) {
			return b;
		}
	}
	return HB - 1;
}

static void count_cb(const float *x, uint32_t idx, void *u) { ARG_UNUSED(x); ARG_UNUSED(idx); (*(uint32_t *)u)++; }

int hubml_train(const char *root, const struct hubml_train_cfg *c, struct hubml_baseline *bl,
		struct xmb_model *xm, struct hubml_train_result *r)
{
	const int64_t t0 = k_uptime_get();
	memset(r, 0, sizeof(*r));
	if (c->trees == 0 || c->trees > XMB_MAX_TREE || c->psi < 2) {
		return -EINVAL;
	}

	/* 0) 개수 */
	uint32_t n = 0;
	int rc = hubml_feat_each(root, c->uid, c->days, c->ndays, count_cb, &n);
	if (rc < 0) {
		return rc;
	}
	if (n < 1000) {
		return -ENODATA;
	}
	r->n_total = n;
	r->n_train = (uint32_t)(n * (1.0f - c->holdout));
	r->n_hold = n - r->n_train;

	/* 1) 구간 경계 — load_ratio 를 4 등분 (0.25 · 0.5 · 0.75). 데이터 기반 경계는 다음 판 */
	memset(bl, 0, sizeof(*bl));
	bl->nreg = HUBML_NREG;
	for (int i = 0; i < HUBML_NREG - 1; i++) {
		bl->reg_edge[i] = (float)(i + 1) / HUBML_NREG;
	}

	/* 2) 1패스: 평균·교차곱 + 저수지 표본 */
	const uint32_t nres = (uint32_t)c->trees * c->psi;
	struct p1 *p = k_calloc(1, sizeof(*p));
	double (*sxx)[NF * NF] = k_calloc(HUBML_NREG, sizeof(*sxx));
	float (*res)[NF] = k_calloc(nres, sizeof(*res));
	if (!p || !sxx || !res) {
		rc = -ENOMEM;
		goto out;
	}
	*p = (struct p1){ .c = c, .n_train = r->n_train, .bl = bl, .sxx = sxx, .res = res, .nres = nres,
			  .seed = c->seed ^ 0x9e3779b9u };
	rc = hubml_feat_each(root, c->uid, c->days, c->ndays, p1_cb, p);
	if (rc < 0) {
		goto out;
	}
	const int64_t t1 = k_uptime_get();
	for (int g = 0; g < HUBML_NREG; g++) {
		const double m = bl->reg[g].n;
		if (m < 20 * NF) {
			/* 표본 부족 — 34 차원 공분산을 믿을 수 없다 (2026-11-09 Lm2: 198 개로 추정한 구간이 있었다).
			 * 이 구간은 가장 가까운 구간을 빌린다 (아래) */
			bl->reg[g].n = 0;
			continue;
		}
		double *mu = bl->reg[g].mean, *L = bl->reg[g].L;
		for (int i = 0; i < NF; i++) {
			mu[i] /= m;
		}
		double tr = 0;
		for (int i = 0; i < NF; i++) {
			for (int j = 0; j <= i; j++) {
				const double v = sxx[g][i * NF + j] / m - mu[i] * mu[j];
				L[i * NF + j] = L[j * NF + i] = v;
			}
			bl->reg[g].sd[i] = sqrt(L[i * NF + i] > 0 ? L[i * NF + i] : 0);
			tr += L[i * NF + i];
		}
		const double ridge = 1e-6 * (tr / NF + 1e-12);
		for (int i = 0; i < NF; i++) {
			L[i * NF + i] += ridge;
		}
		if (cholesky(L, NF)) {
			rc = -EDOM;
			goto out;
		}
	}
	for (int g = 0; g < HUBML_NREG; g++) {
		bl->reg[g].borrowed = -1;
	}
	/* 비어 있는 구간은 이웃 구간 값을 빌린다 (문턱도 아래에서 빌린다) */
	for (int g = 0; g < HUBML_NREG; g++) {
		if (bl->reg[g].n) {
			continue;
		}
		for (int d = 1; d < HUBML_NREG; d++) {
			const int a = g - d, b = g + d;
			const int s = (a >= 0 && bl->reg[a].n) ? a : ((b < HUBML_NREG && bl->reg[b].n) ? b : -1);
			if (s >= 0) {
				bl->reg[g] = bl->reg[s];
				bl->reg[g].n = 0;
				bl->reg[g].borrowed = (int8_t)s;
				break;
			}
		}
	}

	/* 3) Isolation Forest */
	memset(xm, 0, sizeof(*xm));
	xm->kind = 1;
	xm->nfeat = NF;
	xm->contract = NF;
	xm->base = 0.5f;
	xm->train_ts = c->train_ts;
	snprintf(xm->meta, sizeof xm->meta, "MPDM if T%u p%u", c->trees, c->psi);
	struct build b = { .m = xm, .smp = res, .seed = c->seed ? c->seed : 1,
			   .leaf_scale = (double)c->k / c->trees, .cpsi = c_n(c->psi) };
	b.maxd = (int)ceil(log2((double)c->psi));
	int *ix = k_calloc(c->psi, sizeof(int));
	if (!ix) {
		rc = -ENOMEM;
		goto out;
	}
	const uint32_t have = MIN(nres, p->seen);
	for (uint16_t t = 0; t < c->trees && !b.err; t++) {
		const int m = (int)MIN((uint32_t)c->psi, have);
		for (int i = 0; i < m; i++) {
			ix[i] = (int)((t * (uint32_t)c->psi + i) % have);
		}
		xm->root[t] = (uint32_t)grow(&b, ix, m, 0);
		xm->ntree = t + 1;
	}
	k_free(ix);
	if (b.err) {
		rc = b.err;
		goto out;
	}
	r->nodes = xm->nnode;
	const int64_t t2 = k_uptime_get();

	/* 4) 2패스: 문턱 (학습 구간 분위수) + 골든 4 개 */
	struct p2 *q2 = k_calloc(1, sizeof(*q2));
	if (!q2) {
		rc = -ENOMEM;
		goto out;
	}
	q2->n_train = r->n_train;
	q2->bl = bl;
	q2->xm = xm;
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		q2->gold_at[g] = (uint32_t)((uint64_t)r->n_train * (2 * g + 1) / (2 * XMB_NGOLDEN));
	}
	rc = hubml_feat_each(root, c->uid, c->days, c->ndays, p2_cb, q2);
	if (rc >= 0) {
		rc = 0;
		for (int g = 0; g < HUBML_NREG; g++) {
			bl->reg[g].thr_d2 = q2->nreg[g] ? d2_of_bin(quant_bin(q2->hd2[g], q2->nreg[g], c->q)) : 0;
		}
		/* 학습 벡터가 없던 구간은 빌려 온 구간의 문턱을 쓴다 — 1e30 이면 그 구간에서 영영 경보가 없다 */
		for (int g = 0; g < HUBML_NREG; g++) {
			if (bl->reg[g].thr_d2 == 0) {
				const int s = bl->reg[g].borrowed >= 0 ? bl->reg[g].borrowed : g;
				bl->reg[g].thr_d2 = bl->reg[s].thr_d2 > 0 ? bl->reg[s].thr_d2 : 1e30;
			}
		}
		const int pb = quant_bin(q2->hp, r->n_train, c->q);
		xm->thr = (float)(pb + 1) / (HB - 1);
		r->thr_prob = xm->thr;
		for (int g = 0; g < XMB_NGOLDEN; g++) {
			memcpy(xm->golden[g], q2->gold[g], sizeof(float) * NF);
			xm->golden[g][NF] = xmb_predict(xm, q2->gold[g]);
		}
	}
	k_free(q2);
	const int64_t t3 = k_uptime_get();
	r->ms_scan = (uint32_t)((t1 - t0) + (t3 - t2));
	r->ms_build = (uint32_t)(t2 - t1);
out:
	k_free(p);
	k_free(sxx);
	k_free(res);
	r->ms_total = (uint32_t)(k_uptime_get() - t0);
	if (rc == 0) {
		struct hubml_eval_out e;
		if (hubml_eval_holdout(root, c, r, bl, xm, NULL, NULL, NULL, &e) == 0) {
			r->far_stats = e.rate_stats;
			r->far_tree = e.rate_tree;
		}
	}
	return rc;
}

/* ── holdout 평가 ─────────────────────────────────────────────────────── */
struct ev {
	uint32_t from;
	const struct hubml_baseline *bl;
	const struct xmb_model *xm;
	const struct xmd_model *xd;
	hubml_xform xf;
	void *xu;
	uint32_t n, a_s, a_t, a_e;
};

static void ev_cb(const float *x0, uint32_t idx, void *u)
{
	struct ev *e = u;
	if (idx < e->from) {
		return;
	}
	float x[NF];
	memcpy(x, x0, sizeof x);
	if (e->xf) {
		e->xf(x, e->xu);
	}
	const int r = hubml_baseline_regime(e->bl, x);
	if (hubml_baseline_d2(e->bl, x, NULL) > e->bl->reg[r].thr_d2) {
		e->a_s++;
	}
	if (xmb_predict(e->xm, x) > e->xm->thr) {
		e->a_t++;
	}
	if (e->xd && xmd_score(e->xd, x, NULL) > 1.0f) {
		e->a_e++;
	}
	e->n++;
}

int hubml_eval_holdout(const char *root, const struct hubml_train_cfg *c, const struct hubml_train_result *r,
		       const struct hubml_baseline *bl, const struct xmb_model *xm, const struct xmd_model *xd,
		       hubml_xform xf, void *xu, struct hubml_eval_out *o)
{
	struct ev e = { .from = r->n_train, .bl = bl, .xm = xm, .xd = xd, .xf = xf, .xu = xu };
	const int rc = hubml_feat_each(root, c->uid, c->days, c->ndays, ev_cb, &e);
	if (rc < 0) {
		return rc;
	}
	o->n = e.n;
	o->rate_stats = e.n ? (float)e.a_s / e.n : 0;
	o->rate_tree = e.n ? (float)e.a_t / e.n : 0;
	o->rate_edge = e.n ? (float)e.a_e / e.n : 0;
	return 0;
}

void hubml_baseline_export(const struct hubml_baseline *bl, uint32_t train_ts, const float golden_x[XMB_NGOLDEN][HUBML_NFEAT],
			   struct xmd_model *o)
{
	memset(o, 0, sizeof(*o));
	o->nfeat = NF;
	o->contract = NF;
	o->nreg = (uint8_t)bl->nreg;
	o->reg_feat = HUBML_REG_FEAT;
	o->train_ts = train_ts;
	snprintf(o->meta, sizeof o->meta, "MPDM md R%d", bl->nreg);
	for (int i = 0; i < bl->nreg - 1; i++) {
		o->edges[i] = bl->reg_edge[i];
	}
	for (int r = 0; r < bl->nreg; r++) {
		int t = 0;
		for (int i = 0; i < NF; i++) {
			o->reg[r].mean[i] = (float)bl->reg[r].mean[i];
			o->reg[r].sd[i] = (float)bl->reg[r].sd[i];
			for (int k = 0; k <= i; k++) {
				o->reg[r].L[t++] = (float)bl->reg[r].L[i * NF + k];
			}
		}
		o->reg[r].thr_d2 = (float)bl->reg[r].thr_d2;
	}
	for (int g = 0; g < XMB_NGOLDEN; g++) {
		memcpy(o->golden[g], golden_x[g], sizeof(float) * NF);
		o->golden[g][NF] = xmd_score(o, golden_x[g], NULL);
	}
}
