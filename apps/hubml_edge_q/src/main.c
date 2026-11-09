/*
 * hubml_edge_q (Lm3) — Edge MCU (Cortex-M7 · M33, 32비트 Arm, M33 은 단정도 FPU) 에서 도는 쪽을 QEMU M33 으로 시험.
 *  1) 허브가 만든 kind 3(통계 기준선) · kind 1 .xmb 를 그대로 읽는다 (헤더·본문 CRC·골든)
 *  2) 시험 벡터 64 개의 점수가 허브(A55)·파이썬 값과 같은가 (허용 1e-5, 몇 개가 비트까지 같은지도 센다)
 *  3) MCOL/1 HELLO·FEAT 를 만들고 다시 풀기 (32비트에서 u64 · 정렬)
 *  4) 모델 RAM 크기 (XMB_MAX_FEAT=34)
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <math.h>
#include <string.h>
#include <hubml/mcol.h>
#include <hubml/xmb.h>
#include <zephyr/sys/crc.h>

static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static const uint8_t k1[] = {
#include "golden_kind1.xmb.inc"
};
static const uint32_t PV[][36] = {
#include "pv_vectors.inc"
};

static struct xmd_model md;
static struct xmb_model tm;

static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int main(void)
{
	bool ok = true;
	printk("=== Lm3 Edge 쪽 (Cortex-M33 QEMU, 포인터 %u B) ===\n", (unsigned)sizeof(void *));
	printk("모델 RAM: kind 3 %u B · kind 1 %u B (XMB_MAX_FEAT 34)\n", (unsigned)sizeof(md), (unsigned)sizeof(tm));

	const int r3 = xmd_parse(k3, sizeof k3, &md, 1e-5f);
	const int r1 = xmb_parse(k1, sizeof k1, &tm, 1e-6f);
	printk("kind 3 %u B 읽기 %d · kind 1 %u B 읽기 %d (0 = CRC·상한·골든 통과)\n", (unsigned)sizeof k3, r3,
	       (unsigned)sizeof k1, r1);
	if (r3 || r1) { ok = false; }

	float w3 = 0, w1 = 0;
	int same3 = 0, same1 = 0, alarm_ok = 0;
	const int n = ARRAY_SIZE(PV);
	const uint32_t c0 = k_cycle_get_32();
	for (int i = 0; i < n; i++) {
		float x[34];
		for (int k = 0; k < 34; k++) { x[k] = f_of(PV[i][k]); }
		const float s3 = xmd_score(&md, x, NULL), e3 = f_of(PV[i][35]);
		const float p1 = xmb_predict(&tm, x), e1 = f_of(PV[i][34]);
		w3 = fmaxf(w3, fabsf(s3 - e3) / fmaxf(1.0f, fabsf(e3)));
		w1 = fmaxf(w1, fabsf(p1 - e1));
		same3 += (memcmp(&s3, &e3, 4) == 0);
		same1 += (memcmp(&p1, &e1, 4) == 0);
		/* 앞 32 개 정상 · 뒤 32 개 1X ×1.6 — 경보 판정이 허브와 같은가 */
		alarm_ok += ((s3 > 1.0f) == (e3 > 1.0f));
	}
	const uint32_t cyc = k_cycle_get_32() - c0;
	printk("kind 3: 최대 상대 |Δ| %.3e · 비트까지 같음 %d/%d · 경보 판정 같음 %d/%d\n", (double)w3, same3, n, alarm_ok, n);
	printk("kind 1: 최대 |Δ| %.3e · 비트까지 같음 %d/%d\n", (double)w1, same1, n);
	printk("64 벡터 × 두 모델 %u 사이클 (QEMU — 실제 MCU 속도가 아니다)\n", cyc);
	if (w3 > 1e-5f || w1 > 1e-5f || alarm_ok != n) { ok = false; }

	/* MCOL/1 왕복 */
	uint8_t pl[MCOL_MAX_PAYLOAD], out[MCOL_MAX_DGRAM];
	struct mcol_hello h0 = { .uid = 0x0123456789abcdefull, .fw = 0x01020304, .contract = 34, .nfeat = 34,
				 .sensor_hash = 0xdeadbeef, .schema_hash = 0x4d463334 }, h1;
	mcol_hello_put(pl, &h0);
	struct mcol_hdr hd = { .type = MCOL_T_HELLO, .len = MCOL_HELLO_LEN, .edge_ms = 0x1122334455667788ull, .boot_id = 3 };
	int m = mcol_encode(out, sizeof out, &hd, pl);
	struct mcol_hdr d;
	const uint8_t *dp;
	int rc = mcol_decode(out, (size_t)m, &d, &dp);
	mcol_hello_get(dp, &h1);
	const bool hello_ok = !rc && d.edge_ms == hd.edge_ms && h1.uid == h0.uid && h1.schema_hash == h0.schema_hash &&
			      h1.sensor_hash == h0.sensor_hash;
	static float xs[8 * 34];
	for (int i = 0; i < 8 * 34; i++) { xs[i] = f_of(PV[i / 34][i % 34]); }
	const int fl = mcol_feat_put(pl, sizeof pl, 34, 8, xs, NULL);
	struct mcol_hdr fh = { .type = MCOL_T_FEAT, .edge_sid = 5, .stream = MCOL_S_FEAT, .seq = 0xfffffffe, .len = (uint16_t)fl };
	m = mcol_encode(out, sizeof out, &fh, pl);
	rc = mcol_decode(out, (size_t)m, &d, &dp);
	bool feat_ok = !rc && m == 24 + 1124 + 4 && d.seq == 0xfffffffe;
	for (int i = 0; i < 8 && feat_ok; i++) {
		float x[34];
		uint8_t reg, q;
		feat_ok = !mcol_feat_get(dp, d.len, i, x, 34, &reg, &q) && !memcmp(x, &xs[i * 34], sizeof x);
	}
	out[30] ^= 1;
	const bool crc_reject = mcol_decode(out, (size_t)m, &d, &dp) == -EBADMSG;
	printk("MCOL/1: HELLO 왕복 %s · FEAT %d B 왕복 %s · 1 비트 바꾸면 거부 %s\n", hello_ok ? "같음" : "다름", m,
	       feat_ok ? "같음" : "다름", crc_reject ? "예" : "아니오");
	if (!hello_ok || !feat_ok || !crc_reject) { ok = false; }

	/* 골든 기대값을 NaN 으로 바꾸고 CRC 를 다시 맞춘 파일 — 거부해야 한다 (예전 `> tol` 꼴은 통과시켰다) */
	{
		static uint8_t b[sizeof k3];
		memcpy(b, k3, sizeof k3);
		const size_t gold_end = sizeof k3 - 4;		/* 골든 4 × 35 f32 뒤 CRC */
		const uint32_t nan = 0x7fc00000;
		memcpy(b + gold_end - 4, &nan, 4);		/* 마지막 골든의 기대 점수 */
		const uint32_t c = crc32_ieee(b + XMB_HDR_LEN, gold_end - XMB_HDR_LEN);
		memcpy(b + gold_end, &c, 4);
		const int r = xmd_parse(b, sizeof b, &md, 1e-5f);
		printk("NaN 골든 + CRC 다시 맞춤 → %d (-7 = 골든 거부)\n", r);
		if (r != -7) { ok = false; }
		xmd_parse(k3, sizeof k3, &md, 1e-5f);
	}
	printk(ok ? "=== Lm3 PASS\n" : "=== Lm3 FAIL\n");
	return 0;
}
