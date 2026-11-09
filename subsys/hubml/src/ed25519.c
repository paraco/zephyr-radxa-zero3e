/* ed25519 검증 + SHA-512 — include/hubml/ed25519.h. SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <string.h>
#include <hubml/ed25519.h>

/* ── SHA-512 (FIPS 180-4) ────────────────────────────────────────────── */
#include "sha512_consts.inc"
#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static uint64_t be64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}
static void blk(uint64_t h[8], const uint8_t *p)
{
	uint64_t w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = be64(p + 8 * i);
	}
	for (int i = 16; i < 80; i++) {
		const uint64_t s0 = ROR(w[i - 15], 1) ^ ROR(w[i - 15], 8) ^ (w[i - 15] >> 7);
		const uint64_t s1 = ROR(w[i - 2], 19) ^ ROR(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
	for (int i = 0; i < 80; i++) {
		const uint64_t t1 = k + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
		const uint64_t t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
struct s512 { uint64_t h[8]; uint8_t b[128]; size_t n; uint64_t len; };
static void s_init(struct s512 *s) { memcpy(s->h, H512, sizeof s->h); s->n = 0; s->len = 0; }
static void s_upd(struct s512 *s, const uint8_t *m, size_t n)
{
	s->len += n;
	while (n) {
		const size_t k = 128 - s->n < n ? 128 - s->n : n;
		memcpy(s->b + s->n, m, k);
		s->n += k; m += k; n -= k;
		if (s->n == 128) { blk(s->h, s->b); s->n = 0; }
	}
}
static void s_fin(struct s512 *s, uint8_t out[64])
{
	const uint64_t bits = s->len * 8;
	uint8_t pad = 0x80;
	s_upd(s, &pad, 1);
	pad = 0;
	while (s->n != 112) { s_upd(s, &pad, 1); }
	uint8_t l[16] = { 0 };
	for (int i = 0; i < 8; i++) { l[15 - i] = (uint8_t)(bits >> (8 * i)); }
	s_upd(s, l, 16);
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 8; j++) { out[8 * i + j] = (uint8_t)(s->h[i] >> (56 - 8 * j)); }
	}
}
void hubml_sha512(const void *m, size_t n, uint8_t out[64])
{
	struct s512 s;
	s_init(&s);
	s_upd(&s, m, n);
	s_fin(&s, out);
}

/* ── GF(2^255−19): 16 × 16 비트 (TweetNaCl 계열) ─────────────────────── */
typedef int64_t gf[16];
static const gf gf0 = { 0 }, gf1 = { 1 };
static gf D, D2, I, BX, BY;
static bool consts_ready;

static void set(gf r, const gf a) { for (int i = 0; i < 16; i++) { r[i] = a[i]; } }
static void car(gf o)
{
	for (int i = 0; i < 16; i++) {
		o[i] += (1LL << 16);
		const int64_t c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c << 16;
	}
}
static void sel(gf p, gf q, int b)
{
	const int64_t c = ~(b - 1);
	for (int i = 0; i < 16; i++) {
		const int64_t t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}
static void pack(uint8_t o[32], const gf n)
{
	gf m, t;
	set(t, n);
	car(t); car(t); car(t);
	for (int j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (int i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		const int b = (int)((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		sel(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = (uint8_t)(t[i] & 0xff);
		o[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}
static int neq(const gf a, const gf b)
{
	uint8_t c[32], d[32];
	pack(c, a);
	pack(d, b);
	return memcmp(c, d, 32) != 0;
}
static int par(const gf a) { uint8_t d[32]; pack(d, a); return d[0] & 1; }
static void unpack(gf o, const uint8_t n[32])
{
	for (int i = 0; i < 16; i++) { o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8); }
	o[15] &= 0x7fff;
}
static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) { o[i] = a[i] + b[i]; } }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) { o[i] = a[i] - b[i]; } }
static void M(gf o, const gf a, const gf b)
{
	int64_t t[31] = { 0 };
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 16; j++) { t[i + j] += a[i] * b[j]; }
	}
	for (int i = 0; i < 15; i++) { t[i] += 38 * t[i + 16]; }
	for (int i = 0; i < 16; i++) { o[i] = t[i]; }
	car(o);
	car(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void inv(gf o, const gf i)
{
	gf c;
	set(c, i);
	for (int a = 253; a >= 0; a--) {
		S(c, c);
		if (a != 2 && a != 4) { M(c, c, i); }
	}
	set(o, c);
}
static void pow2523(gf o, const gf i)
{
	gf c;
	set(c, i);
	for (int a = 250; a >= 0; a--) {
		S(c, c);
		if (a != 1) { M(c, c, i); }
	}
	set(o, c);
}
static void small(gf o, uint32_t v) { set(o, gf0); o[0] = v & 0xffff; o[1] = v >> 16; }

/* x 를 y·부호에서 되살린다 (x² = (y²−1)/(d·y²+1)). 0 = 정상 */
static int recover_x(gf x, const gf y, int sign)
{
	gf num, den, den2, den4, den6, t, chk;
	S(num, y);
	M(den, num, D);
	Z(num, num, gf1);
	A(den, gf1, den);
	S(den2, den); S(den4, den2); M(den6, den4, den2);
	M(t, den6, num); M(t, t, den);
	pow2523(t, t);
	M(t, t, num); M(t, t, den); M(t, t, den);
	M(x, t, den);
	S(chk, x); M(chk, chk, den);
	if (neq(chk, num)) { M(x, x, I); }
	S(chk, x); M(chk, chk, den);
	if (neq(chk, num)) { return -1; }
	if (par(x) != sign) { Z(x, gf0, x); }
	return 0;
}

static void init_consts(void)
{
	gf a, b;
	/* d = −121665 / 121666 */
	small(a, 121665); small(b, 121666);
	inv(b, b);
	M(D, a, b);
	Z(D, gf0, D);
	A(D2, D, D);
	/* √−1 = 2^((p−1)/4) — 지수 (p−1)/4 = 2^253 − 5 : 2 를 제곱하며 곱한다 */
	gf two, r;
	small(two, 2);
	set(r, gf1);
	uint8_t e[32];
	memset(e, 0xff, 32);	/* 2^253 − 5 = 0x1fff…fffb (리틀엔디언) */
	e[0] = 0xfb;
	e[31] = 0x1f;
	for (int i = 255; i >= 0; i--) {
		S(r, r);
		if ((e[i / 8] >> (i & 7)) & 1) { M(r, r, two); }
	}
	set(I, r);
	/* 기준점: y = 4/5, x 짝수 */
	small(a, 4); small(b, 5);
	inv(b, b);
	M(BY, a, b);
	(void)recover_x(BX, BY, 0);
	consts_ready = true;
}

/* 확장 좌표 점 덧셈 (TweetNaCl add) */
static void padd(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;
	Z(a, p[1], p[0]); Z(t, q[1], q[0]); M(a, a, t);
	A(b, p[0], p[1]); A(t, q[0], q[1]); M(b, b, t);
	M(c, p[3], q[3]); M(c, c, D2);
	M(d, p[2], q[2]); A(d, d, d);
	Z(e, b, a); Z(f, d, c); A(g, d, c); A(h, b, a);
	M(p[0], e, f); M(p[1], h, g); M(p[2], g, f); M(p[3], e, h);
}
static void cswap(gf p[4], gf q[4], int b) { for (int i = 0; i < 4; i++) { sel(p[i], q[i], b); } }
static void ppack(uint8_t r[32], gf p[4])
{
	gf tx, ty, zi;
	inv(zi, p[2]);
	M(tx, p[0], zi);
	M(ty, p[1], zi);
	pack(r, ty);
	r[31] ^= (uint8_t)(par(tx) << 7);
}
static void smult(gf p[4], gf q[4], const uint8_t s[32])
{
	set(p[0], gf0); set(p[1], gf1); set(p[2], gf1); set(p[3], gf0);
	for (int i = 255; i >= 0; i--) {
		const int b = (s[i / 8] >> (i & 7)) & 1;
		cswap(p, q, b);
		padd(q, p);
		padd(p, p);
		cswap(p, q, b);
	}
}
static void sbase(gf p[4], const uint8_t s[32])
{
	gf q[4];
	set(q[0], BX); set(q[1], BY); set(q[2], gf1); M(q[3], BX, BY);
	smult(p, q, s);
}

/* ── mod L ────────────────────────────────────────────────────────────── */
static const int64_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
			       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
static void modL(uint8_t r[32], int64_t x[64])
{
	int64_t carry;
	for (int i = 63; i >= 32; i--) {
		carry = 0;
		int j;
		for (j = i - 32; j < i - 12; j++) {
			x[j] += carry - 16 * x[i] * L[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry << 8;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	for (int j = 0; j < 32; j++) {
		x[j] += carry - (x[31] >> 4) * L[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (int j = 0; j < 32; j++) { x[j] -= carry * L[j]; }
	for (int i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = (uint8_t)(x[i] & 255);
	}
}
static bool s_lt_L(const uint8_t s[32])
{
	for (int i = 31; i >= 0; i--) {
		if (s[i] < L[i]) { return true; }
		if (s[i] > L[i]) { return false; }
	}
	return false;
}

int hubml_ed25519_verify(const uint8_t sig[64], const void *msg, size_t n, const uint8_t pk[32])
{
	if (!consts_ready) {
		init_consts();
	}
	if (!s_lt_L(sig + 32)) {
		return -2;
	}
	/* −A */
	gf q[4], p[4];
	unpack(q[1], pk);
	if (recover_x(q[0], q[1], pk[31] >> 7)) {
		return -1;
	}
	Z(q[0], gf0, q[0]);
	set(q[2], gf1);
	M(q[3], q[0], q[1]);
	/* h = SHA512(R ‖ A ‖ M) mod L */
	struct s512 s;
	uint8_t h[64], hr[32];
	s_init(&s);
	s_upd(&s, sig, 32);
	s_upd(&s, pk, 32);
	s_upd(&s, msg, n);
	s_fin(&s, h);
	int64_t x[64];
	for (int i = 0; i < 64; i++) { x[i] = h[i]; }
	modL(hr, x);
	/* [h](−A) + [s]B 가 R 인가 */
	smult(p, q, hr);
	gf b[4];
	sbase(b, sig + 32);
	padd(p, b);
	uint8_t t[32];
	ppack(t, p);
	return memcmp(t, sig, 32) ? -3 : 0;
}

#ifdef HUBML_ED25519_SIGN
static void reduce64(uint8_t r[32], const uint8_t h[64])
{
	int64_t x[64];
	for (int i = 0; i < 64; i++) { x[i] = h[i]; }
	modL(r, x);
}

void hubml_ed25519_sign(uint8_t sig[64], const void *msg, size_t n, const uint8_t seed[32], uint8_t pk[32])
{
	if (!consts_ready) {
		init_consts();
	}
	uint8_t d[64], r[32], k[32], hh[64];
	hubml_sha512(seed, 32, d);
	d[0] &= 248;
	d[31] &= 127;
	d[31] |= 64;
	gf p[4];
	sbase(p, d);
	ppack(pk, p);
	struct s512 s;
	s_init(&s);
	s_upd(&s, d + 32, 32);
	s_upd(&s, msg, n);
	s_fin(&s, hh);
	reduce64(r, hh);
	sbase(p, r);
	ppack(sig, p);
	s_init(&s);
	s_upd(&s, sig, 32);
	s_upd(&s, pk, 32);
	s_upd(&s, msg, n);
	s_fin(&s, hh);
	reduce64(k, hh);
	int64_t x[64] = { 0 };
	for (int i = 0; i < 32; i++) { x[i] = r[i]; }
	for (int i = 0; i < 32; i++) {
		for (int j = 0; j < 32; j++) { x[i + j] += (int64_t)k[i] * d[j]; }
	}
	modL(sig + 32, x);
}
#endif
