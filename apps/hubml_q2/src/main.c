/*
 * hubml_q2 (Lm2) — 학습 엔진을 QEMU 에서 시험한다.
 *
 *  1) 가짜 정상 데이터 하루(86,400 벡터, 1 Hz) — 부하 0.4 / 0.8 이 2 시간마다 바뀐다(운전 구간 2 개),
 *     잠재 요인 4 개로 특징들이 함께 움직인다. 수집 엔진(손실 없는 채널)으로 RAM 디스크에 쌓는다.
 *  2) 학습: 통계 기준선 + Isolation Forest(45 그루 × ψ 64) → .xmb, 끝 20 % 는 평가용
 *  3) .xmb 를 파일로 쓰고 다시 읽는다 (헤더·본문 CRC · 상한 · 골든 4 개)
 *  3b) Edge 용 kind=3(통계 기준선, float32)을 쓰고 다시 읽는다
 *  4) 오경보율 + 가짜 고장 민감도 (1X 오더 · 첨도 · 전류 슬립 측파대)
 *  5) 모델(base64)과 시험 벡터 64 개를 찍는다 → 호스트 tools/xmb_ref.py 가 파이썬으로 다시 계산해 대조
 *
 * 판정 (미리 정한 기준): 상한 안 · 왕복·골든 통과 · 오경보율 둘 다 ≤ 1.5 % ·
 *   가장 큰 고장(1X ×1.6)에서 통계 기준선 검출 ≥ 90 % ·
 *   Edge kind=3 오경보율 ≤ 1.5 %, 세 고장 모두 ×1.3 에서 검출 ≥ 90 % (Lm2 첫 실행 뒤 kind=3 을 더하며 정한 기준).  === Lm2 PASS|FAIL
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/base64.h>
#include <ff.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/train.h>
#include <hubml/xmb.h>

#define ROOT "/RAM:/ml"
#define NF HUBML_NFEAT
#define NVEC 86400
#define UID 0x4d50444d00000001ull

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };

static uint32_t rs = 777;
static double ur(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs >> 8) * (1.0 / 16777216.0) + 1e-9; }
static double gauss(void) { return sqrt(-2.0 * log(ur())) * cos(6.283185307179586 * ur()); }

static void gen_vec(uint32_t t, float *x)
{
	const double load = ((t / 7200) % 2 ? 0.8 : 0.4) + 0.02 * gauss();
	double z[4];
	for (int j = 0; j < 4; j++) { z[j] = gauss(); }
	for (int i = 0; i < NF; i++) {
		const double base = 1.0 + 0.1 * i / NF;
		double m = (i >= 16 && i <= 25) ? base * (0.5 + load) : base * (1.0 + 0.2 * load);
		for (int j = 0; j < 4; j++) { m += 0.05 * base * sin(i * (j + 1) + 1.0) * z[j]; }
		x[i] = (float)(m + 0.02 * base * gauss());
	}
	x[26] = (float)(1.0 + 0.005 * gauss());	/* rpm_norm */
	x[27] = (float)load;			/* load_ratio */
}

/* 수집 엔진으로 쌓는다 — 손실 없는 채널 (Lm1 이 손실 채널을 시험했다) */
static int nop_send(uint32_t a, const uint8_t *b, size_t n) { ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(n); return 0; }
static const struct hubml_xport xp = { .send = nop_send };

static int fill(void)
{
	uint8_t pl[MCOL_MAX_PAYLOAD], out[MCOL_MAX_DGRAM];
	struct mcol_hello hm = { .uid = UID, .fw = 1, .contract = NF, .nfeat = NF };
	mcol_hello_put(pl, &hm);
	struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN };
	int n = mcol_encode(out, sizeof out, &h, pl);
	hubml_ingest_rx(1, out, (size_t)n);
	static float xs[8 * NF];
	uint32_t seq = 0;
	for (uint32_t t = 0; t < NVEC; t += 8) {
		for (int i = 0; i < 8; i++) { gen_vec(t + i, &xs[i * NF]); }
		const int L = mcol_feat_put(pl, sizeof pl, NF, 8, xs, NULL);
		struct mcol_hdr f = { .type = MCOL_T_FEAT, .edge_sid = 1, .stream = MCOL_S_FEAT, .seq = seq++,
				      .edge_ms = (uint64_t)t * 1000, .len = (uint16_t)L };
		n = mcol_encode(out, sizeof out, &f, pl);
		hubml_ingest_rx(1, out, (size_t)n);
		if (seq % 64 == 0 && hubml_ingest_commit()) { return -EIO; }
	}
	return hubml_ingest_commit();
}

struct fault { int idx; float mul; };
static void xf(float *x, void *u) { const struct fault *f = u; x[f->idx] *= f->mul; }

/* 호스트 대조용 시험 벡터: holdout 앞쪽 32 개(정상) + 같은 것에 1X ×1.6 */
struct pv { uint32_t from, n; };
static struct xmb_model xm, xm2;
static struct xmd_model xd, xd2;
static struct hubml_baseline bl;
static void pv_cb(const float *x, uint32_t idx, void *u)
{
	struct pv *p = u;
	if (idx < p->from || p->n >= 64) { return; }
	float v[NF];
	memcpy(v, x, sizeof v);
	if (p->n >= 32) { v[6] *= 1.6f; }
	printk("PV");
	for (int i = 0; i < NF; i++) { uint32_t b; memcpy(&b, &v[i], 4); printk(" %08x", b); }
	const float pr = xmb_predict(&xm, v);
	const float sc = xmd_score(&xd, v, NULL);
	uint32_t pb, sb; memcpy(&pb, &pr, 4); memcpy(&sb, &sc, 4);
	printk(" %08x %08x\n", pb, sb);
	p->n++;
	p->from += 97;	/* 띄엄띄엄 */
}

int main(void)
{
	printk("=== Lm2 hubml 학습 엔진 (QEMU) ===\n");
	bool ok = true;
	int rc = fs_mount(&mnt);
	if (rc) { printk("마운트 실패 %d\n=== Lm2 FAIL\n", rc); return 0; }
	const int64_t epoch0 = 1794182400000LL + 3600000LL;	/* 2026-11-09 01:00 UTC */
	hubml_time_set(epoch0);
	hubml_ingest_init(ROOT, &xp);
	int64_t t0 = k_uptime_get();
	rc = fill();
	printk("데이터 쌓기 %d · %lld ms (%d 벡터)\n", rc, k_uptime_get() - t0, NVEC);
	if (rc) { printk("=== Lm2 FAIL\n"); return 0; }

	char days[2][9];
	hubml_day_str(epoch0, days[0]);
	hubml_day_str(hubml_time_now(), days[1]);
	const struct hubml_train_cfg c = { .uid = UID, .days = days, .ndays = strcmp(days[0], days[1]) ? 2 : 1,
					   .holdout = 0.2f, .q = 0.995f, .trees = 45, .psi = 64, .k = 1.0f,
					   .seed = 20261109, .train_ts = (uint32_t)(epoch0 / 1000) };
	struct hubml_train_result r;
	rc = hubml_train(ROOT, &c, &bl, &xm, &r);
	printk("학습 %d · 벡터 %u (학습 %u · 평가 %u) · 트리 %u · 노드 %u · 문턱 확률 %.4f · "
	       "시간 %u ms (읽기 %u · 만들기 %u)\n", rc, r.n_total, r.n_train, r.n_hold, xm.ntree, xm.nnode,
	       (double)r.thr_prob, r.ms_total, r.ms_scan, r.ms_build);
	if (rc) { printk("=== Lm2 FAIL\n"); return 0; }
	for (int g = 0; g < HUBML_NREG; g++) {
		printk("  구간 %d: 학습 벡터 %u · d² 문턱 %.2f%s\n", g, bl.reg[g].n, bl.reg[g].thr_d2,
		       bl.reg[g].borrowed >= 0 ? " (학습하지 않은 구간 — 이웃 구간을 빌림)" : "");
		if (bl.reg[g].thr_d2 > 1e20) { printk("✗ 경보가 날 수 없는 구간\n"); ok = false; }
	}
	if (xm.ntree > XMB_MAX_TREE || xm.nnode > XMB_MAX_NODE) { printk("✗ 상한 넘음\n"); ok = false; }

	/* .xmb 쓰기 → 파일 → 다시 읽기 */
	static uint8_t buf[XMB_HDR_LEN + 4 * XMB_MAX_TREE + 16 * XMB_MAX_NODE + 4 * (4 * XMB_MAX_FEAT + 4) + 4];
	const int sz = xmb_write(&xm, buf, sizeof buf);
	struct fs_file_t f;
	fs_file_t_init(&f);
	rc = sz > 0 ? fs_open(&f, ROOT "/cand.xmb", FS_O_CREATE | FS_O_WRITE) : -EIO;
	if (!rc) { rc = fs_write(&f, buf, (size_t)sz) == sz ? 0 : -EIO; fs_close(&f); }
	memset(buf, 0, sizeof buf);
	fs_file_t_init(&f);
	ssize_t got = -1;
	if (!rc && !fs_open(&f, ROOT "/cand.xmb", FS_O_READ)) { got = fs_read(&f, buf, sizeof buf); fs_close(&f); }
	const int pr = (got == sz) ? xmb_parse(buf, (size_t)got, &xm2, 1e-6f) : -100;
	printk(".xmb %d B · 쓰기 %d · 다시 읽기 %d (0 = 헤더·본문 CRC·상한·골든 통과)\n", sz, rc, pr);
	if (pr) { ok = false; }
	/* 한 바이트를 바꾸면 거부해야 한다 */
	buf[XMB_HDR_LEN + 10] ^= 0x01;
	const int pr2 = xmb_parse(buf, (size_t)got, &xm2, 1e-6f);
	buf[XMB_HDR_LEN + 10] ^= 0x01;
	printk("본문 1 바이트 바꾸면: %d (-6 = 본문 CRC 거부)\n", pr2);
	if (pr2 != -6) { ok = false; }

	/* Edge 용 kind=3 (통계 기준선) — 쓰기 · 다시 읽기 · 골든 */
	{
		float gx[XMB_NGOLDEN][NF];
		for (int g = 0; g < XMB_NGOLDEN; g++) { memcpy(gx[g], xm.golden[g], sizeof(float) * NF); }
		hubml_baseline_export(&bl, c.train_ts, gx, &xd);
		static uint8_t b3[16384];
		const int s3 = xmd_write(&xd, b3, sizeof b3);
		const int p3 = s3 > 0 ? xmd_parse(b3, (size_t)s3, &xd2, 1e-5f) : -100;
		b3[XMB_HDR_LEN + 100] ^= 0x10;
		const int p3b = s3 > 0 ? xmd_parse(b3, (size_t)s3, &xd2, 1e-5f) : -100;
		b3[XMB_HDR_LEN + 100] ^= 0x10;
		printk("kind=3 기준선 %d B · 다시 읽기 %d · 1 바이트 바꾸면 %d\n", s3, p3, p3b);
		if (p3 || p3b != -6) { ok = false; }
		printk("XMD64_BEGIN %d\n", s3);
		for (int o = 0; o < s3; o += 57) {
			char line[80];
			size_t ol = 0;
			base64_encode((uint8_t *)line, sizeof line, &ol, b3 + o, (size_t)MIN(57, s3 - o));
			line[ol] = 0;
			printk("XMD64 %s\n", line);
		}
		printk("XMD64_END\n");
	}

	/* 평가 */
	printk("오경보율(평가 구간 %u): 통계 %.2f %% · 트리 %.2f %%\n", r.n_hold, (double)(100 * r.far_stats),
	       (double)(100 * r.far_tree));
	if (r.far_stats > 0.015f || r.far_tree > 0.015f) { printk("✗ 오경보율 1.5 %% 넘음\n"); ok = false; }
	static const struct { const char *name; int idx; float mul[3]; } F[] = {
		{ "1X 오더(불평형)", 6, { 1.1f, 1.3f, 1.6f } },
		{ "첨도(베어링)", 4, { 1.1f, 1.3f, 1.6f } },
		{ "전류 슬립 측파대(회전자 바)", 18, { 1.1f, 1.3f, 1.6f } },
	};
	{
		struct hubml_eval_out e0;
		hubml_eval_holdout(ROOT, &c, &r, &bl, &xm, &xd, NULL, NULL, &e0);
		printk("Edge kind=3(float32) 오경보율 %.2f %%\n", (double)(100 * e0.rate_edge));
		if (e0.rate_edge > 0.015f) { ok = false; }
	}
	printk("가짜 고장 검출률 (허브 통계 double / 트리 kind1 / Edge kind3 float32):\n");
	for (size_t i = 0; i < ARRAY_SIZE(F); i++) {
		printk("  %-26s", F[i].name);
		for (int k = 0; k < 3; k++) {
			struct fault fl = { .idx = F[i].idx, .mul = F[i].mul[k] };
			struct hubml_eval_out e;
			hubml_eval_holdout(ROOT, &c, &r, &bl, &xm, &xd, xf, &fl, &e);
			printk("  ×%.1f %5.1f/%5.1f/%5.1f %%", (double)F[i].mul[k], (double)(100 * e.rate_stats),
			       (double)(100 * e.rate_tree), (double)(100 * e.rate_edge));
			if (i == 0 && k == 2 && e.rate_stats < 0.9f) { printk(" ✗"); ok = false; }
			if (k == 1 && e.rate_edge < 0.9f) { printk(" ✗Edge"); ok = false; }
		}
		printk("\n");
	}

	/* 호스트 대조용 출력 */
	printk("XMB64_BEGIN %d\n", sz);
	for (int o = 0; o < sz; o += 57) {
		char line[80];
		size_t ol = 0;
		base64_encode((uint8_t *)line, sizeof line, &ol, buf + o, (size_t)MIN(57, sz - o));
		line[ol] = 0;
		printk("XMB64 %s\n", line);
	}
	printk("XMB64_END\n");
	struct pv p = { .from = r.n_train };
	hubml_feat_each(ROOT, UID, days, c.ndays, pv_cb, &p);
	printk("PV_END %u\n", p.n);

	printk(ok ? "=== Lm2 PASS\n" : "=== Lm2 FAIL\n");
	return 0;
}
