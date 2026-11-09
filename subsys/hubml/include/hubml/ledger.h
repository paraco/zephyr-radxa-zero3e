/*
 * hubml 원장 — 정본 규격 MoruLab_MLOps design/MLCC_LEDGER_SPEC.md `mlledger/1` (Studio 와 합의).
 *
 *   <root>/ledger.jsonl  한 줄(≤ 3,000 B) = 사건 하나:
 *     {"v":1,"seq":N,"prev":"<앞 줄 sha256>","ts":"YYYY-MM-DDTHH:MM:SSZ"|"T+<초>s","kind":"<사건>",
 *      "hub":"<칩 ID>","actor":"hub|studio:<키>|edge:<uid>",<칸…>[,"sig":{…}]}
 *   줄 해시 = sha256(저장된 줄 바이트, 끝 \n 제외). 첫 줄 prev = 0×64, seq 는 1 부터.
 *   사건: dataset · normal_gate · model · eval · alias · deploy · feedback · maintenance · ledger_repair
 *   모델: model_id "<uid>:<n>" (장비별 1 부터) · 파일 models/<uid>/<n>.xmb
 *
 *   시작할 때 사슬을 처음부터 검사한다. 끝 줄이 반쪽(전원 끊김)이면 잘라 내고 ledger_repair 를 남긴다.
 *   중간이 깨졌으면 head.ok = false · bad_seq — 덧붙이기를 거부한다(사람이 봐야 한다).
 *   별칭(v0 · champion · challenger · shadow)은 alias 사건을 다시 따라가 복원한다.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUBML_LEDGER_LINE_MAX 3000
#define HUBML_MODEL_ID_LEN    24	/* "<uid16>:<n>" */

struct hubml_ledger_head {
	bool ok;
	uint32_t next_seq;	/* 다음에 쓸 번호 (= 줄 수 + 1) */
	char hash[65];		/* 마지막 줄 해시 */
	uint32_t bad_seq;	/* 사슬이 깨진 첫 번호 (ok=false 일 때) */
	uint32_t torn_bytes;	/* 시작할 때 잘라 낸 반쪽 꼬리 */
};

enum hubml_alias { HUBML_A_V0, HUBML_A_CHAMPION, HUBML_A_CHALLENGER, HUBML_A_SHADOW, HUBML_NALIAS };
extern const char *const hubml_alias_name[HUBML_NALIAS];

/** hub = 허브 칩 ID (FW_INFO deviceid) */
int hubml_ledger_init(const char *root, const char *hub);
void hubml_ledger_head(struct hubml_ledger_head *out);
/** fields = 앞뒤 중괄호 없는 JSON 조각 (줄바꿈 금지, NULL 가능). sig = JSON 객체 문자열(NULL = 칸 없음).
 *  반환 0, seq_out = 번호 */
int hubml_ledger_append(const char *kind, const char *actor, const char *fields, const char *sig, uint32_t *seq_out);
typedef void (*hubml_ledger_cb)(uint32_t seq, const char *line, size_t n, void *user);
/** since 번호부터 최대 max 줄. 반환 = 넘긴 줄 수 */
int hubml_ledger_read(uint32_t since, int max, hubml_ledger_cb cb, void *user);

/** 모델 파일을 원장에 올린다 → 관문(CRC·상한·골든) → models/<uid>/<n>.xmb + model 사건.
 *  extra = 추가 칸(train_ds · schema_hash · params · generation …, NULL 가능) */
int hubml_model_register(uint64_t uid, const uint8_t *xmb, size_t n, const char *actor, const char *extra,
			 char id_out[HUBML_MODEL_ID_LEN + 1]);
/** 별칭 옮기기 → alias 사건. reason = promote|rollback|retire|propose.
 *  champion 은 승인 서명(sig 객체)이 있어야 한다 — 자동 승격 없음. (서명 검증은 mlledger §4, 다음 단계) */
int hubml_alias_set(uint64_t uid, enum hubml_alias a, const char *model_id, const char *reason, const char *actor,
		    const char *sig);
/** 지금 별칭이 가리키는 모델 ("" = 없음) */
const char *hubml_alias_get(uint64_t uid, enum hubml_alias a);
/** 모델 파일 읽기 (model_id "<uid>:<n>") — 반환 = 크기 */
int hubml_model_load(uint64_t uid, const char *id, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
