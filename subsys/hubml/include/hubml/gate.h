/*
 * 정상 관문(normal_gate) — 원장의 승인된 사건 하나가 정한 include/exclude 구간으로 학습 데이터를 거른다.
 * 정본: MoruLab_MLOps design/MLCC_LEDGER_SPEC.md (normal_gate · §4). 구간 = [측정 UTC 날짜, "HH:MM", "HH:MM") — 끝 제외,
 * 끝에만 "24:00" 허용, 시작 < 끝. exclude 는 4번째 칸에 사유가 붙는다. uid 는 ds_id 의 `ds-<16hex>-` 에서만 읽는다.
 *
 * 벡터 측정 UTC (Studio MlccAnalysis readDaySeries 와 같은 규칙):
 *   앵커 = 그 .mcs 파일이 있는 날짜 폴더 anchor.jsonl 중 time_set=1 · boot_id 같음 · |edge_ms 차| 최소(같으면 앞 줄)
 *   utc0 = anchor.hub_ms + (레코드 edge_ms − anchor.edge_ms), 벡터 i>0 이면 utc += dt_ms[i] 누적(dt_ms[0] 은 더하지 않음)
 *   ⚠ Studio 와 다른 점: 앵커 없는 벡터는 Studio 가 수집 순서로 시각을 추정해 쓰지만, 여기서는 뺀다(unanchored).
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUBML_GATE_MAX_IV    32	/* include · exclude 각각 구간 수 상한 */
#define HUBML_GATE_MAX_FDAYS (HUBML_GATE_MAX_IV * 3)	/* 읽을 파일 날짜 (D · D+1 · D+2 의 합집합) */
#define HUBML_ANCHOR_MAX     1024	/* 폴더당 앵커 상한 (넘으면 앞 것만 쓰고 anchor_over 로 센다) */

struct hubml_gate_iv {
	char day[9];		/* 측정 UTC 날짜 yyyymmdd */
	int32_t dn;		/* 1970-01-01 부터의 일 수 */
	uint16_t a, b;		/* 하루 안 분 [a, b) — b 는 1440 까지 */
};

struct hubml_gate {
	uint32_t seq;
	char ds_id[96];
	int ninc, nexc;
	struct hubml_gate_iv inc[HUBML_GATE_MAX_IV], exc[HUBML_GATE_MAX_IV];
	int nidays;			/* include 날짜 (정렬 · 중복 없음) */
	char idays[HUBML_GATE_MAX_IV][9];
	int nfdays;			/* 읽을 파일 날짜 (정렬 · 중복 없음) */
	char fdays[HUBML_GATE_MAX_FDAYS][9];
};

/** 한 패스를 거른 셈 (같은 관문 · 같은 데이터면 패스마다 같다) */
struct hubml_gate_counts {
	uint32_t unanchored;	/* 앵커가 없어 시각을 모르는 벡터 (뺌) */
	uint32_t outside;	/* include 밖 */
	uint32_t excluded;	/* include 안이지만 exclude */
	uint32_t bad;		/* 관문 경로: quality≠0 + 비유한(NaN/Inf) 특징 벡터 (dataset counts.bad_quality) */
	uint32_t anchor_over;	/* 앵커 상한 넘어 못 쓴 줄 */
};

struct hubml_anchor {
	uint16_t boot;
	int64_t edge_ms;
	int64_t hub_ms;
};

/** 원장 seq 줄을 읽어 검사하고 g 를 채운다. 0 = 정상.
 *  실패: *code = "not_found" | "bad_arg", *why = "gate_<이유>" (HUBML/1 err code= msg=) */
int hubml_gate_load(uint32_t seq, const char *uid16, struct hubml_gate *g, const char **code, const char **why);
/** uid 의 가장 최근(seq 가 가장 큰) normal_gate 중 decision=accept · sig 객체 있음 · ds_id 의 uid 일치. 0 = 찾음 */
int hubml_gate_latest(const char *uid16, uint32_t *seq);
/** 원장 줄 하나를 normal_gate 로 해석 (시험용으로도 공개). 모양 검사 포함 */
int hubml_gate_parse(const char *line, size_t n, const char *uid16, struct hubml_gate *g, const char **code,
		     const char **why);
/** anchor.jsonl 한 줄. 0 = 정상. *time_set 에 time_set 값 */
int hubml_anchor_parse(const char *line, size_t n, struct hubml_anchor *a, bool *time_set);
/** 측정 UTC(ms) 하나가 관문을 통과하는가: 0 = 남김 · 1 = include 밖 · 2 = exclude */
int hubml_gate_check(const struct hubml_gate *g, int64_t utc_ms);

#ifdef __cplusplus
}
#endif
