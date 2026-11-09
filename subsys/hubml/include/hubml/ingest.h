/*
 * hubml 수집 엔진 — MCOL/1 데이터그램을 받아 SD(FatFs)에 쌓고, **기록(fs_sync)한 뒤에** ACK 한다.
 *
 *   rx → 검사(CRC) → 장비 표(HELLO) → 흐름별 순번 창(64) · 중복 거르기 → RAM 대기열
 *   commit(2 s 마다) → 대기열을 (장비,흐름,날짜) 파일에 덧붙임 → 닫기(동기화) → 상태 파일 → ACK
 *
 * 저장: <root>/ds/<uid 16진>/<yyyymmdd>/s<흐름>.mcs   (머리 16 B + [u16 길이][데이터그램 원본]…)
 *       <root>/ml_state.bin                           (장비 표 + 흐름별 기록 완료 순번)
 * 상태 파일은 데이터보다 늦게 쓰므로, 그 사이 꺼지면 같은 레코드가 파일에 두 번 있을 수 있다 —
 * 읽는 쪽(hubml_store_scan)이 (흐름, seq) 로 거른다. 잃는 일은 없다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <hubml/mcol.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HUBML_MAX_EDGES
#define HUBML_MAX_EDGES 32
#endif
#ifndef HUBML_QUEUE_BYTES
#define HUBML_QUEUE_BYTES (1024 * 1024)	/* 대기열 한 벌 (두 벌을 번갈아 쓴다) */
#endif
#define HUBML_WIN 64

/** 운반 — 허브 → Edge 송신 (HaLow 면 ap.udp_send, 시험이면 가짜 채널). addr = 보낸 쪽 주소 */
struct hubml_xport {
	int (*send)(uint32_t addr, const uint8_t *buf, size_t n);
};

struct hubml_edge_info {
	uint64_t uid;
	uint16_t sid;
	uint32_t addr;
	uint16_t boot_id;
	uint32_t fw;
	uint16_t contract;
	uint32_t schema_hash;
	int64_t last_rx_ms;
	uint32_t next[MCOL_NSTREAM];	/* 기록 완료(ACK 된) 다음 순번 */
	uint32_t rx, dup, oow, qfull;
};

struct hubml_stats {
	uint32_t rx, bad, dup, oow, qfull, nosid, accepted;
	uint32_t commits, written, acks, commit_err;
	uint32_t last_commit_ms, max_commit_ms;
	size_t queued;
};

/** root 예: "/0:/ml" (실기) · "/RAM:/ml" (QEMU). 상태 파일이 있으면 읽어 이어 간다 */
int hubml_ingest_init(const char *root, const struct hubml_xport *x);
void hubml_ingest_rx(uint32_t addr, const uint8_t *buf, size_t n);
/** 대기열을 기록하고 ACK 한다. 0 = 정상 */
int hubml_ingest_commit(void);
/** 시험용 — 전원이 나간 것처럼 RAM 의 대기열·상태를 버린다 (다시 init 해야 한다) */
void hubml_ingest_crash(void);
/** 시험용 — 다음 commit 에서 데이터만 쓰고 상태 파일 전에 "전원이 나간" 것처럼 멈춘다 (-ECANCELED) */
void hubml_ingest_test_cut_before_state(void);

void hubml_ingest_stats(struct hubml_stats *out);
int hubml_ingest_edges(struct hubml_edge_info *out, int max);

/** 허브 시각 (UTC ms). 기본은 0 + 가동 시간 — Studio 가 ml time set 으로 맞춘다 */
void hubml_time_set(int64_t epoch_ms);
int64_t hubml_time_now(void);

/** FatFs 를 다른 쓰는 곳(hub_fs 등)과 함께 쓸 때 잠금 — 기본은 내부 뮤텍스, 앱이 덮어쓴다 */
void hubml_fs_lock(void);
void hubml_fs_unlock(void);

/** 저장된 레코드를 (흐름, seq) 중복 없이 차례로 넘긴다. 반환 = 넘긴 수, 음수 = 오류.
 *  phys_dup 에는 파일에 두 번 있던 수를 더한다(있으면). */
typedef void (*hubml_rec_cb)(const struct mcol_hdr *h, const uint8_t *payload, void *user);
int hubml_store_scan(const char *root, uint64_t uid, const char *day, uint8_t stream,
		     hubml_rec_cb cb, void *user, uint32_t *phys_dup);
/** 날짜 문자열 yyyymmdd (허브 시각 기준) */
void hubml_day_str(int64_t epoch_ms, char out[9]);

#ifdef __cplusplus
}
#endif
