/*
 * ed25519 서명 검증 (RFC 8032, 검증만) + SHA-512 — 허브 원장 승인(mlledger/1 v0.2 §4) 용.
 * 외부 암호 모듈 없이 둔다(Zephyr PSA 에 EdDSA 구현이 없다). 구조는 TweetNaCl 계열(체 원소 16 × 16 비트),
 * 상수 d · √−1 · 기준점은 처음 쓸 때 정의식에서 계산하고, SHA-512 상수는 파이썬 decimal 로 만든 표를 쓴다.
 * 공개 데이터만 다루므로(검증) 상수 시간일 필요는 없다. s < L 검사로 서명 변형(malleability)을 막는다.
 * 정확성: QEMU Lm8 이 Mac `cryptography` 가 만든 키·서명과 대조한다.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void hubml_sha512(const void *m, size_t n, uint8_t out[64]);
/** 0 = 서명이 맞다 · -1 공개키 이상 · -2 s 범위 밖 · -3 서명 다름 */
int hubml_ed25519_verify(const uint8_t sig[64], const void *msg, size_t n, const uint8_t pk[32]);

#ifdef HUBML_ED25519_SIGN
/** 시험 빌드에만 — 서명(RFC 8032). seed = 비밀 32 B. 허브는 서명하지 않는다(검증만) */
void hubml_ed25519_sign(uint8_t sig[64], const void *msg, size_t n, const uint8_t seed[32], uint8_t pk_out[32]);
#endif

#ifdef __cplusplus
}
#endif
