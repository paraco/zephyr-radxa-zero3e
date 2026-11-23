/*
 * MCOL/1 — Edge → HUB AI 수집 규약 (HaLow UDP 데이터그램 하나 = 레코드 하나).
 *
 *   [머리 24 B][본문 len B][CRC32 4 B]   리틀엔디안 · CRC-32/IEEE(머리+본문)
 *
 *   0  'M' 'C'      2   ver(1)   3 type
 *   4  edge_sid u16 (허브가 HELLO_ACK 로 정해 준 번호, HELLO 는 0)
 *   6  stream u8    7 flags u8
 *   8  seq u32      (흐름별, Edge 재부팅에도 이어진다 — 보낼 함에 저장)
 *   12 edge_ms u64  (Edge 단조 시각)
 *   20 len u16      22 boot_id u16
 *
 * 한 데이터그램은 1,200 B 이하 — NRC SoftAP_RPC 의 udp_rx base64 버퍼(1,600 B) 한도.
 * 설계: docs/hub-ml-plan.md (수집 영역).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCOL_VER         1
#define MCOL_HDR_LEN     24
#define MCOL_CRC_LEN     4
#define MCOL_MAX_DGRAM   1200
#define MCOL_MAX_PAYLOAD (MCOL_MAX_DGRAM - MCOL_HDR_LEN - MCOL_CRC_LEN)

enum mcol_type {
	MCOL_T_HELLO = 1,
	MCOL_T_HELLO_ACK,
	MCOL_T_FEAT,
	MCOL_T_RAW_HDR,
	MCOL_T_RAW_CHUNK,
	MCOL_T_EVT,
	MCOL_T_HB,
	MCOL_T_ACK,
	MCOL_T_CMD,
	MCOL_T_CMD_ACK,		/* Edge → 허브: CMD 의 결과 (제어 흐름, 순번 추적 없음 — 허브가 다시 보낸다) */
	MCOL_T_SCORE,		/* Edge → 허브: 시범 운영(shadow) 점수 — 흐름 MCOL_S_EVT (= 11) */
};

/* SCORE 본문 (LE) — FEAT 묶음마다 두 슬롯(0 champion = 경보 · 1 challenger = 시범, 점수만)의 점수. 흐름 MCOL_S_EVT.
 *   머리 16 B: feat_seq u32 · n u8 · flags u8(0) · rsv u16 · champ_num u32 · chal_num u32
 *   그 뒤 n × 9 B: { champ f32 · chal f32 · vflags u8 }       n = 1..8 (FEAT 묶음 상한), 본문 길이 = 16 + 9n 정확히
 *   vflags bit0 champ_valid · bit1 chal_valid · bit2 champ_alarm · bit3 chal_alarm   (bit4~7 은 앞으로 늘 비트 — 읽을 때 무시)
 *   빈 슬롯 → valid 0 · num 0 · 점수칸 0.0f. quality≠0 벡터 → 두 valid 0 (점수 무의미).
 *   데이터그램 머리 edge_ms · boot_id 는 그 FEAT 묶음의 값. feat_seq 는 짝 확인용(허브 요약은 짝을 따지지 않는다).
 *   읽는 쪽 규칙: n 이 0·9 이상이거나 길이가 어긋나면 깨진 레코드(mcol_score_get 이 거부) · flags·rsv 는 무시 ·
 *   점수가 NaN/inf 면 valid 비트가 켜져 있어도 무효(경보 비트도 무시). */
#define MCOL_SCORE_HDR   16
#define MCOL_SCORE_ENT   9
#define MCOL_SCORE_MAX_N 8
#define MCOL_SV_CHAMP_VALID 0x01
#define MCOL_SV_CHAL_VALID  0x02
#define MCOL_SV_CHAMP_ALARM 0x04
#define MCOL_SV_CHAL_ALARM  0x08

/* CMD 본문: op u8 · rsv u8 · xfer u16 · … (허브 → Edge, 모델 배포)
 *   BEGIN    slot u8 · kind u8 · rsv u16 · size u32 · num u32 · sha256[32]          (44 B)
 *   CHUNK    off u32 · 데이터 (≤ MCOL_CHUNK)
 *   END      —
 *   ACTIVATE — (challenger 슬롯을 champion 으로 — 승격. 옛 champion 은 challenger 로 내려 되돌리기에 쓴다)
 * CMD_ACK 본문 (48 B): op u8 · status i8 · xfer u16 · next_off u32 · slot u8 · rsv[3] · num u32 · sha256[32]
 *   status 0 정상 · -1 순서/번호 · -2 너무 큼 · -3 sha256 다름 · -4 파일 검사 실패 · -5 계약 다름 · -6 슬롯 비었음 */
enum mcol_op { MCOL_OP_BEGIN = 1, MCOL_OP_CHUNK, MCOL_OP_END, MCOL_OP_ACTIVATE };
#define MCOL_CHUNK       1024
#define MCOL_CMD_HDR     4
#define MCOL_CMD_ACK_LEN 48

enum mcol_stream {
	MCOL_S_CTRL = 0,
	MCOL_S_FEAT,
	MCOL_S_RAW,
	MCOL_S_EVT,
	MCOL_S_HB,
	MCOL_NSTREAM,
};

#define MCOL_F_BACKLOG 0x01	/* 밀린 데이터 (끊겼다 이어진 뒤) */

struct mcol_hdr {
	uint8_t type;
	uint16_t edge_sid;
	uint8_t stream;
	uint8_t flags;
	uint32_t seq;
	uint64_t edge_ms;
	uint16_t len;
	uint16_t boot_id;
};

/** HELLO 본문 (24 B): uid u64 · fw u32 · contract u16 · nfeat u8 · rsv u8 · sensor_hash u32 · schema_hash u32
 *  schema_hash = 특징 정의·순서·샘플링을 묶은 해시 (2026-11-09 Studio MLCC 요청 — 원장이 데이터↔스키마를 잇는다,
 *  모델에도 같은 값을 고정해 다르면 Edge 가 적재를 거부한다) */
struct mcol_hello {
	uint64_t uid;		/* 칩 고유번호 */
	uint32_t fw;		/* 펌웨어 판 */
	uint16_t contract;	/* 특징 계약 (MFEAT34 = 34) */
	uint8_t nfeat;
	uint32_t sensor_hash;	/* 센서 설정 해시 */
	uint32_t schema_hash;	/* 특징 스키마 해시 */
};
#define MCOL_HELLO_LEN 24

/** HELLO_ACK 본문 (16 B): sid u16 · rsv u16 · hub_ms u64 · raw_period_s u32 */
#define MCOL_HELLO_ACK_LEN 16

/** ACK 본문: 항목 n 개 × 9 B { stream u8 · next u32 · sack u32 }
 *  next = 이 번호 앞은 모두 허브 SD 에 기록됨. sack bit i = next+1+i 도 기록됨. */
#define MCOL_ACK_ENTRY_LEN 9

/** FEAT 본문: [n u8][contract u8][rsv u16] + n × { f32 × contract · regime u8 · quality u8 · dt_ms u16 }
 *  contract 34(MFEAT34)면 벡터당 140 B, 8 개 묶음 = 1,124 B. quality 0 = 정상 측정 */
#define MCOL_FEAT_MAX_N 8
#define MCOL_FEAT_HDR   4
static inline size_t mcol_feat_vec_len(int contract) { return (size_t)contract * 4 + 4; }
/** i 번째 벡터를 꺼낸다. 0 = 정상 */
int mcol_feat_get(const uint8_t *pl, uint16_t len, int i, float *x, int nmax, uint8_t *regime, uint8_t *quality);
/** i 번째 벡터의 dt_ms (앞 벡터와의 간격). 0 = 정상 */
int mcol_feat_dt(const uint8_t *pl, uint16_t len, int i, uint16_t *dt_ms);
/** 묶음 만들기 (Edge·시험용). 반환 = 본문 길이 */
int mcol_feat_put(uint8_t *pl, size_t cap, int contract, int n, const float *xs, const uint8_t *quality);

struct mcol_score_hdr {
	uint32_t feat_seq;
	uint8_t n;
	uint8_t flags;		/* 읽을 때만 — 쓸 때는 0 */
	uint32_t champ_num, chal_num;
};
struct mcol_score_ent {
	float champ, chal;
	uint8_t vflags;
};
static inline size_t mcol_score_len(int n) { return MCOL_SCORE_HDR + (size_t)n * MCOL_SCORE_ENT; }
/** SCORE 본문 만들기. n = h->n (1..8), vflags 는 하위 4 비트만 쓴다. 반환 = 본문 길이, 음수 = 오류(-EINVAL n · -EMSGSIZE cap) */
int mcol_score_put(uint8_t *pl, size_t cap, const struct mcol_score_hdr *h, const struct mcol_score_ent *ent);
/** 머리를 읽고 길이를 검사한다. 0 = 정상, -EBADMSG = 깨짐(n 이 1..8 밖이거나 len != 16 + 9n) */
int mcol_score_get(const uint8_t *pl, uint16_t len, struct mcol_score_hdr *h);
/** i 번째 항목 (mcol_score_get 이 0 을 준 본문에서). 점수는 그대로 — NaN/inf 판정은 부르는 쪽 */
void mcol_score_ent_get(const uint8_t *pl, int i, struct mcol_score_ent *e);

/** 반환 = 데이터그램 길이 (머리+본문+CRC), 음수 = 오류 */
int mcol_encode(uint8_t *out, size_t cap, const struct mcol_hdr *h, const void *payload);
/** 0 = 정상. -EBADMSG 매직·판·길이·CRC 이상 */
int mcol_decode(const uint8_t *in, size_t n, struct mcol_hdr *h, const uint8_t **payload);

void mcol_hello_put(uint8_t *p, const struct mcol_hello *m);
void mcol_hello_get(const uint8_t *p, struct mcol_hello *m);

#ifdef __cplusplus
}
#endif
