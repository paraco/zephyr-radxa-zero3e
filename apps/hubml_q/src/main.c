/*
 * hubml_q (Lm1) — 수집 엔진을 QEMU 에서 시험한다. 실기 없이 끝까지 돌릴 수 있게 결정적인 단일 스레드 모의.
 *
 *  가짜 Edge 20 대 · 흐름 4 개(FEAT 8 s · HB 5 s · EVT · RAW 10 분마다 51 조각) · 1 틱 = 1 초
 *  채널: Edge→허브 손실 5 % · 중복 3 % · 0~3 틱 지연(순서 뒤바뀜) / 허브→Edge(ACK) 손실 5 %
 *  Edge: 흐름별 창 16 · 60 틱 동안 ACK 가 없으면 다시 보냄 · 오래 진전이 없으면 HELLO 다시
 *  중간(1200 틱)에 허브를 강제로 재시작(대기열·상태 버림 → 상태 파일에서 이어 받기)
 *  1800 틱에는 데이터만 쓰고 상태 파일 전에 끊는다 → 파일에 같은 레코드가 두 번 → 읽을 때 거른다
 *  허브 시각을 2026-11-09 23:59:59 로 두어 시험 중에 날짜가 바뀐다(파일이 이틀로 나뉜다)
 *
 *  판정: 모든 장비·흐름에서 만든 순번 0..n-1 이 **모두 한 번씩**, 내용이 같게 읽히면 PASS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>

#define NE      20
#define WIN     16
#define RTO     60
#define GEN_T   2400	/* 40 분 동안 만든다 */
#define CRASH_T  1201	/* 기록 사이 — 대기열에 받은 것이 남아 있다 */
#define CRASH2_T 1800	/* 데이터만 쓰고 상태 파일 전에 끊긴다 */
#define MAX_T   (GEN_T + 4000)
#define COMMIT_EVERY 2
#define ROOT "/RAM:/ml"

static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };

/* ── 결정적 난수 ─────────────────────────────────────────────────────── */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }

static uint16_t pl_len(uint8_t stream, uint32_t seq)
{
	switch (stream) {
	case MCOL_S_FEAT: return 4 + 8 * 140;	/* 1124 */
	case MCOL_S_RAW:  return (seq % 51) == 0 ? 64 : 1100;
	case MCOL_S_EVT:  return 32;
	default:          return 24;	/* HB */
	}
}
static uint8_t pl_type(uint8_t stream, uint32_t seq)
{
	switch (stream) {
	case MCOL_S_FEAT: return MCOL_T_FEAT;
	case MCOL_S_RAW:  return (seq % 51) == 0 ? MCOL_T_RAW_HDR : MCOL_T_RAW_CHUNK;
	case MCOL_S_EVT:  return MCOL_T_EVT;
	default:          return MCOL_T_HB;
	}
}
static void pl_fill(uint8_t *p, uint16_t n, uint64_t uid, uint8_t stream, uint32_t seq)
{
	uint32_t x = (uint32_t)(uid ^ (uid >> 32)) * 2654435761u ^ (stream * 40503u) ^ (seq * 2246822519u);
	if (!x) { x = 1; }
	for (uint16_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[i] = (uint8_t)x; }
}

/* ── 채널 ─────────────────────────────────────────────────────────────── */
struct pkt { bool used; bool to_hub; uint32_t addr; int64_t due; uint16_t n; uint8_t b[MCOL_MAX_DGRAM]; };
#define NPKT 4096
static struct pkt pool[NPKT];
static int64_t now_t;
static uint32_t drops, dups_inj, pool_full;

static void chan_put(bool to_hub, uint32_t addr, const uint8_t *b, size_t n, int delay)
{
	for (int i = 0; i < NPKT; i++) {
		if (!pool[i].used) {
			pool[i] = (struct pkt){ .used = true, .to_hub = to_hub, .addr = addr,
						.due = now_t + delay, .n = (uint16_t)n };
			memcpy(pool[i].b, b, n);
			return;
		}
	}
	pool_full++;
}

static void edge_to_hub(uint32_t addr, const uint8_t *b, size_t n)
{
	if (chance(5)) { drops++; return; }
	chan_put(true, addr, b, n, (int)(rnd() % 4));
	if (chance(3)) { dups_inj++; chan_put(true, addr, b, n, (int)(rnd() % 4)); }
}

static int hub_send(uint32_t addr, const uint8_t *b, size_t n)
{
	if (chance(5)) { drops++; return 0; }
	chan_put(false, addr, b, n, 1);
	return 0;
}
static const struct hubml_xport xp = { .send = hub_send };

/* ── 가짜 Edge ────────────────────────────────────────────────────────── */
struct slot { uint32_t seq; int64_t at; bool valid; };
struct sedge {
	uint64_t uid;
	uint16_t sid;
	uint32_t gen[MCOL_NSTREAM], next[MCOL_NSTREAM], sack[MCOL_NSTREAM];
	struct slot sl[MCOL_NSTREAM][WIN];
	int64_t hello_at, progress_at;
};
static struct sedge E[NE];

static void edge_send_rec(struct sedge *e, int idx, uint8_t s, uint32_t seq)
{
	static uint8_t pl[MCOL_MAX_PAYLOAD], out[MCOL_MAX_DGRAM];
	const uint16_t n = pl_len(s, seq);
	pl_fill(pl, n, e->uid, s, seq);
	struct mcol_hdr h = { .type = pl_type(s, seq), .edge_sid = e->sid, .stream = s,
			      .seq = seq, .edge_ms = (uint64_t)now_t * 1000, .len = n, .boot_id = 1,
			      .flags = (seq + WIN < e->gen[s]) ? MCOL_F_BACKLOG : 0 };
	const int m = mcol_encode(out, sizeof out, &h, pl);
	if (m > 0) { edge_to_hub((uint32_t)idx + 1, out, (size_t)m); }
}

static void edge_hello(struct sedge *e, int idx)
{
	uint8_t pl[MCOL_HELLO_LEN], out[64];
	struct mcol_hello m = { .uid = e->uid, .fw = 0x0100, .contract = 34, .nfeat = 34, .sensor_hash = 0xabcd, .schema_hash = 0x4d463334 };
	mcol_hello_put(pl, &m);
	struct mcol_hdr h = { .type = MCOL_T_HELLO, .stream = MCOL_S_CTRL, .len = sizeof pl, .boot_id = 1 };
	const int n = mcol_encode(out, sizeof out, &h, pl);
	edge_to_hub((uint32_t)idx + 1, out, (size_t)n);
	e->hello_at = now_t;
}

static void edge_rx(struct sedge *e, const uint8_t *b, size_t n)
{
	struct mcol_hdr h;
	const uint8_t *pl;
	if (mcol_decode(b, n, &h, &pl)) { return; }
	if (h.type == MCOL_T_HELLO_ACK) {
		e->sid = sys_get_le16(pl);
		e->progress_at = now_t;
	} else if (h.type == MCOL_T_ACK) {
		for (size_t o = 0; o + MCOL_ACK_ENTRY_LEN <= h.len; o += MCOL_ACK_ENTRY_LEN) {
			const uint8_t s = pl[o];
			const uint32_t nx = sys_get_le32(pl + o + 1), sk = sys_get_le32(pl + o + 5);
			if (s >= MCOL_NSTREAM) { continue; }
			if ((int32_t)(nx - e->next[s]) > 0) { e->next[s] = nx; e->progress_at = now_t; }
			if (nx == e->next[s]) { e->sack[s] = sk; }
		}
	}
}

static bool edge_acked(const struct sedge *e, uint8_t s, uint32_t seq)
{
	if ((int32_t)(seq - e->next[s]) < 0) { return true; }
	const uint32_t d = seq - e->next[s];
	return d >= 1 && d <= 32 && ((e->sack[s] >> (d - 1)) & 1);
}

static void edge_tick(struct sedge *e, int idx)
{
	if (now_t < GEN_T) {
		if (now_t % 8 == 0)   { e->gen[MCOL_S_FEAT]++; }
		if (now_t % 5 == 0)   { e->gen[MCOL_S_HB]++; }
		if (now_t % 97 == 13) { e->gen[MCOL_S_EVT]++; }
		if (now_t % 600 == 30) { e->gen[MCOL_S_RAW] += 51; }
	}
	if (e->sid == 0) {
		if (now_t - e->hello_at >= 20 || e->hello_at == 0) { edge_hello(e, idx); }
		return;
	}
	bool pending = false;
	for (uint8_t s = 1; s < MCOL_NSTREAM; s++) {
		for (uint32_t seq = e->next[s]; seq < e->gen[s] && seq < e->next[s] + WIN; seq++) {
			if (edge_acked(e, s, seq)) { continue; }
			pending = true;
			struct slot *sl = &e->sl[s][seq % WIN];
			if (!sl->valid || sl->seq != seq || now_t - sl->at >= RTO) {
				edge_send_rec(e, idx, s, seq);
				*sl = (struct slot){ .seq = seq, .at = now_t, .valid = true };
			}
		}
	}
	/* 오래 진전이 없으면 허브가 나를 잊었을 수 있다 — HELLO 다시 */
	if (pending && now_t - e->progress_at > 3 * RTO && now_t - e->hello_at > 3 * RTO) {
		edge_hello(e, idx);
	}
}

static bool all_done(void)
{
	for (int i = 0; i < NE; i++) {
		for (int s = 1; s < MCOL_NSTREAM; s++) {
			if (E[i].next[s] != E[i].gen[s]) { return false; }
		}
	}
	return true;
}

/* ── 검증 ─────────────────────────────────────────────────────────────── */
struct vctx { uint64_t uid; uint8_t s; uint32_t bad; uint32_t cnt; uint32_t gen; };
static void vcb(const struct mcol_hdr *h, const uint8_t *pl, void *user)
{
	struct vctx *v = user;
	static uint8_t exp[MCOL_MAX_PAYLOAD];
	const uint16_t n = pl_len(v->s, h->seq);
	pl_fill(exp, n, v->uid, v->s, h->seq);
	if (h->seq >= v->gen || h->len != n || h->type != pl_type(v->s, h->seq) || memcmp(pl, exp, n)) {
		v->bad++;
	}
	v->cnt++;
}

int main(void)
{
	printk("=== Lm1 hubml 수집 엔진 (QEMU) ===\n");
	int rc = fs_mount(&mnt);
	if (rc) { printk("마운트 실패 %d\n=== Lm1 FAIL\n", rc); return 0; }

	/* 2026-11-09 23:59:59 UTC — 시험 도중(약 1 초 뒤) 날이 바뀐다 */
	const int64_t epoch0 = 1794268799000LL;
	hubml_time_set(epoch0);
	char day0[9], day1[9];
	hubml_day_str(epoch0, day0);
	hubml_day_str(epoch0 + 86400000LL, day1);

	rc = hubml_ingest_init(ROOT, &xp);
	if (rc) { printk("init 실패 %d\n=== Lm1 FAIL\n", rc); return 0; }
	for (int i = 0; i < NE; i++) { E[i].uid = 0x4d4f52550000ull + (uint64_t)i * 0x1111; }

	const int64_t w0 = k_uptime_get();
	for (now_t = 0; now_t < MAX_T; now_t++) {
		/* 채널 배달 */
		for (int i = 0; i < NPKT; i++) {
			if (pool[i].used && pool[i].due <= now_t) {
				pool[i].used = false;
				if (pool[i].to_hub) {
					hubml_ingest_rx(pool[i].addr, pool[i].b, pool[i].n);
				} else if (pool[i].addr >= 1 && pool[i].addr <= NE) {
					edge_rx(&E[pool[i].addr - 1], pool[i].b, pool[i].n);
				}
			}
		}
		for (int i = 0; i < NE; i++) { edge_tick(&E[i], i); }
		if (now_t == CRASH2_T) {
			hubml_ingest_test_cut_before_state();
			const int c = hubml_ingest_commit();
			printk("── %lld 틱: 데이터만 쓰고 상태 전에 끊김 (commit %d) → 재시작\n", now_t, c);
			hubml_ingest_crash();
			for (int i = 0; i < NPKT; i++) { if (pool[i].to_hub) { pool[i].used = false; } }
			rc = hubml_ingest_init(ROOT, &xp);
			if (rc) { printk("다시 init 실패 %d\n=== Lm1 FAIL\n", rc); return 0; }
		} else if (now_t % COMMIT_EVERY == 0) {
			if (hubml_ingest_commit()) { printk("commit 실패\n"); }
		}
		if (now_t == CRASH_T) {
			struct hubml_stats s0;
			hubml_ingest_stats(&s0);
			printk("── %lld 틱: 허브 강제 재시작 (대기열 %zu B 버림)\n", now_t, s0.queued);
			hubml_ingest_crash();
			for (int i = 0; i < NPKT; i++) { if (pool[i].to_hub) { pool[i].used = false; } }
			rc = hubml_ingest_init(ROOT, &xp);
			if (rc) { printk("다시 init 실패 %d\n=== Lm1 FAIL\n", rc); return 0; }
		}
		if (now_t > GEN_T && all_done()) { break; }
		if (now_t % 600 == 0) {
			struct hubml_stats s;
			hubml_ingest_stats(&s);
			printk("t=%lld rx=%u acc=%u dup=%u oow=%u nosid=%u commits=%u acks=%u max_commit_ms=%u\n",
			       now_t, s.rx, s.accepted, s.dup, s.oow, s.nosid, s.commits, s.acks, s.max_commit_ms);
		}
	}
	(void)hubml_ingest_commit();
	const int64_t wall = k_uptime_get() - w0;
	struct hubml_stats s;
	hubml_ingest_stats(&s);
	printk("끝: 틱 %lld · 실제 %lld ms · rx=%u acc=%u dup=%u oow=%u bad=%u nosid=%u qfull=%u written=%u "
	       "commits=%u acks=%u max_commit_ms=%u · 채널 손실 %u · 넣은 중복 %u · 풀 넘침 %u\n",
	       now_t, wall, s.rx, s.accepted, s.dup, s.oow, s.bad, s.nosid, s.qfull, s.written, s.commits,
	       s.acks, s.max_commit_ms, drops, dups_inj, pool_full);

	/* 검증 — 하루치 파일 둘을 모두 읽는다 */
	bool ok = all_done();
	uint32_t total = 0, bad = 0, phys_dup = 0;
	for (int i = 0; i < NE; i++) {
		for (uint8_t st = 1; st < MCOL_NSTREAM; st++) {
			struct vctx v = { .uid = E[i].uid, .s = st, .gen = E[i].gen[st] };
			const int a = hubml_store_scan(ROOT, E[i].uid, day0, st, vcb, &v, &phys_dup);
			const int b = hubml_store_scan(ROOT, E[i].uid, day1, st, vcb, &v, &phys_dup);
			(void)a; (void)b;
			if (v.cnt != E[i].gen[st] || v.bad) {
				printk("✗ 장비 %d 흐름 %u: 읽음 %u / 만듦 %u · 내용 다름 %u\n", i, st, v.cnt,
				       E[i].gen[st], v.bad);
				ok = false;
			}
			total += v.cnt;
			bad += v.bad;
		}
	}
	printk("검증: 레코드 %u · 내용 다름 %u · 파일 안 중복(읽을 때 거름) %u · 모두 ACK %s · 날짜 %s/%s\n",
	       total, bad, phys_dup, all_done() ? "예" : "아니오", day0, day1);
	/* 끊김 시험이 실제로 무언가를 만들었는지 — 아니면 시험이 아무것도 시험하지 않은 것이다 */
	if (phys_dup == 0) { printk("✗ 상태 전 끊김이 파일 안 중복을 만들지 않았다 — 시험이 약하다\n"); ok = false; }
	uint32_t d1 = 0;
	for (int i = 0; i < NE; i++) {
		struct vctx v = { .uid = E[i].uid, .s = MCOL_S_HB, .gen = E[i].gen[MCOL_S_HB] };
		if (hubml_store_scan(ROOT, E[i].uid, day1, MCOL_S_HB, vcb, &v, NULL) > 0) { d1++; }
	}
	printk("둘째 날 파일이 있는 장비 %u / %d\n", d1, NE);
	if (d1 == 0) { printk("✗ 날짜가 바뀌지 않았다 — 날짜 나눔을 시험하지 못했다\n"); ok = false; }
	/* 시각 앵커: 장비·날짜마다 있고, 같은 boot 안에서 edge_ms 가 줄지 않는다 */
	int anc_files = 0, anc_lines = 0, anc_bad = 0;
	for (int i = 0; i < NE; i++) {
		for (int dd = 0; dd < 2; dd++) {
			char p[96];
			static char buf[2048];
			snprintf(p, sizeof p, ROOT "/ds/%016llx/%s/anchor.jsonl", (unsigned long long)E[i].uid, dd ? day1 : day0);
			struct fs_file_t f;
			fs_file_t_init(&f);
			if (fs_open(&f, p, FS_O_READ)) { continue; }
			const ssize_t n = fs_read(&f, buf, sizeof buf - 1);
			fs_close(&f);
			buf[n > 0 ? n : 0] = 0;
			anc_files++;
			long long prev = -1;
			for (char *l = buf; *l; ) {
				char *nl = strchr(l, '\n');
				if (!nl) { break; }
				*nl = 0;
				char *e = strstr(l, "\"edge_ms\":");
				const long long em = e ? atoll(e + 10) : -2;
				if (!strstr(l, "\"boot_id\":1") || em < prev) { anc_bad++; }
				prev = em;
				anc_lines++;
				l = nl + 1;
			}
		}
	}
	printk("시각 앵커: 파일 %d · 줄 %d · 이상 %d\n", anc_files, anc_lines, anc_bad);
	if (anc_files < NE || anc_bad) { printk("✗ 앵커\n"); ok = false; }
	printk(ok ? "=== Lm1 PASS\n" : "=== Lm1 FAIL\n");
	return 0;
}
