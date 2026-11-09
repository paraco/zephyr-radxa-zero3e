/*
 * hubml_q8 (Lm8) — ed25519 검증 · SHA-512 (QEMU A55). 기대값은 Mac `cryptography` 50 · hashlib.
 *  SHA-512 12 길이 (블록 경계 111·112·127·128·129 포함) · 서명 32 개 정상 · 각각 서명 1 비트 · 메시지 1 비트 ·
 *  공개키 1 비트 변조 거부 · s 에 L 을 더한 변형(s ≥ L) 거부.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <hubml/ed25519.h>

static const struct { uint32_t n; uint8_t d[64]; } SV[] = {
#include "sha512_vectors.inc"
};
static const struct { uint8_t seed[32], pk[32], sig[64]; uint32_t n; uint8_t m[256]; } SG[] = {
#include "ed25519_sign_vectors.inc"
};
static const struct { uint8_t pk[32], sig[64]; uint32_t n; uint8_t m[1000]; } EV[] = {
#include "ed25519_vectors.inc"
};

int main(void)
{
	printk("=== Lm8 ed25519 · SHA-512 (QEMU) ===\n");
	bool ok = true;
	int sha_ok = 0;
	static uint8_t m[1000];
	for (int i = 0; i < ARRAY_SIZE(SV); i++) {
		for (uint32_t k = 0; k < SV[i].n; k++) { m[k] = (uint8_t)((k * 7 + 3) & 0xff); }
		uint8_t d[64];
		hubml_sha512(m, SV[i].n, d);
		sha_ok += !memcmp(d, SV[i].d, 64);
	}
	printk("SHA-512 %d/%d 일치\n", sha_ok, ARRAY_SIZE(SV));
	if (sha_ok != ARRAY_SIZE(SV)) { ok = false; }

	int good = 0, rej_sig = 0, rej_msg = 0, rej_pk = 0, rej_mal = 0, n = ARRAY_SIZE(EV);
	const int64_t t0 = k_uptime_get();
	for (int i = 0; i < n; i++) {
		good += hubml_ed25519_verify(EV[i].sig, EV[i].m, EV[i].n, EV[i].pk) == 0;
		uint8_t s[64], pk[32];
		memcpy(s, EV[i].sig, 64);
		s[i % 64] ^= (uint8_t)(1u << (i % 8));
		rej_sig += hubml_ed25519_verify(s, EV[i].m, EV[i].n, EV[i].pk) != 0;
		if (EV[i].n) {
			memcpy(m, EV[i].m, EV[i].n);
			m[i % EV[i].n] ^= 0x01;
			rej_msg += hubml_ed25519_verify(EV[i].sig, m, EV[i].n, EV[i].pk) != 0;
		} else {
			rej_msg += hubml_ed25519_verify(EV[i].sig, "x", 1, EV[i].pk) != 0;
		}
		memcpy(pk, EV[i].pk, 32);
		pk[i % 31] ^= 0x02;
		rej_pk += hubml_ed25519_verify(EV[i].sig, EV[i].m, EV[i].n, pk) != 0;
		/* s + L (같은 점, 다른 바이트) — 변형 서명은 거부해야 한다 */
		static const uint8_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
					       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
		memcpy(s, EV[i].sig, 64);
		unsigned c = 0;
		for (int k = 0; k < 32; k++) { c += s[32 + k] + L[k]; s[32 + k] = (uint8_t)c; c >>= 8; }
		rej_mal += hubml_ed25519_verify(s, EV[i].m, EV[i].n, EV[i].pk) == -2;
	}
	const int64_t ms = k_uptime_get() - t0;
	printk("ed25519: 정상 %d/%d · 서명 변조 거부 %d/%d · 메시지 변조 거부 %d/%d · 공개키 변조 거부 %d/%d · s+L 거부 %d/%d · %lld ms (QEMU)\n",
	       good, n, rej_sig, n, rej_msg, n, rej_pk, n, rej_mal, n, ms);
	if (good != n || rej_sig != n || rej_msg != n || rej_pk != n || rej_mal != n) { ok = false; }
	int sg = 0;
	for (int i = 0; i < ARRAY_SIZE(SG); i++) {
		uint8_t sig[64], pk[32];
		hubml_ed25519_sign(sig, SG[i].m, SG[i].n, SG[i].seed, pk);
		sg += !memcmp(pk, SG[i].pk, 32) && !memcmp(sig, SG[i].sig, 64) &&
		      hubml_ed25519_verify(sig, SG[i].m, SG[i].n, pk) == 0;
	}
	printk("서명(시험 빌드): 공개키·서명 바이트가 Mac 과 같고 검증 통과 %d/%d\n", sg, ARRAY_SIZE(SG));
	if (sg != ARRAY_SIZE(SG)) { ok = false; }
	printk(ok ? "=== Lm8 PASS\n" : "=== Lm8 FAIL\n");
	return 0;
}
