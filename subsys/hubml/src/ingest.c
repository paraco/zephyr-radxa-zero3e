/* hubml 수집 엔진 — include/hubml/ingest.h. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <hubml/ingest.h>

LOG_MODULE_REGISTER(hubml, LOG_LEVEL_INF);

/* ── 잠금 ─────────────────────────────────────────────────────────────── */
static K_MUTEX_DEFINE(fs_mtx);
__weak void hubml_fs_lock(void) { k_mutex_lock(&fs_mtx, K_FOREVER); }
__weak void hubml_fs_unlock(void) { k_mutex_unlock(&fs_mtx); }

static K_MUTEX_DEFINE(st_mtx);	/* 장비 표 · 대기열 (rx 와 commit 사이) */

/* ── 시각 ─────────────────────────────────────────────────────────────── */
static int64_t epoch_base;	/* epoch_ms - uptime_ms */
static bool time_set;
void hubml_time_set(int64_t epoch_ms) { epoch_base = epoch_ms - k_uptime_get(); time_set = true; }
bool hubml_time_is_set(void) { return time_set; }
int64_t hubml_time_now(void) { return epoch_base + k_uptime_get(); }

void hubml_day_str(int64_t epoch_ms, char out[9])
{
	time_t t = (time_t)(epoch_ms / 1000);
	struct tm tm;
	gmtime_r(&t, &tm);
	snprintf(out, 9, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* ── 장비 표 ──────────────────────────────────────────────────────────── */
struct sstate {
	uint32_t next;		/* 이 앞은 모두 받음(대기열 또는 기록) */
	uint64_t bits;		/* bit i = next+i 받음 (bit 0 은 늘 0) */
};

struct edge {
	bool used;
	uint64_t uid;
	uint32_t addr;
	uint16_t boot_id;
	uint32_t fw;
	uint16_t contract;
	uint32_t schema_hash;
	int64_t last_rx_ms;
	struct sstate rcv[MCOL_NSTREAM];	/* 받은 것 */
	struct sstate dur[MCOL_NSTREAM];	/* 기록 완료 (ACK 하는 값) */
	bool need_ack;
	uint32_t rx, dup, oow, qfull;
};

static struct edge edges[HUBML_MAX_EDGES];	/* sid = 번호 + 1 */
static const struct hubml_xport *xp;
static char root[48];
static struct hubml_stats st;

/* ── 대기열: 두 벌을 번갈아 (rx 는 act 에, commit 은 다른 벌을 쓴다) ─────── */
struct qrec {
	uint16_t sid;
	uint8_t stream;
	uint16_t len;
	uint32_t off;		/* 버퍼 안 위치 */
	int64_t hub_ms;
};
#define QMAX_REC (HUBML_QUEUE_BYTES / 64)
struct qbuf {
	uint8_t data[HUBML_QUEUE_BYTES];
	struct qrec rec[QMAX_REC];
	size_t used;
	int n;
};
static struct qbuf qb[2];
static int act;
static K_MUTEX_DEFINE(commit_mtx);
static bool cut_before_state;
void hubml_ingest_test_cut_before_state(void) { cut_before_state = true; }

static struct edge *edge_by_sid(uint16_t sid)
{
	if (sid == 0 || sid > HUBML_MAX_EDGES || !edges[sid - 1].used) {
		return NULL;
	}
	return &edges[sid - 1];
}

/* 받기 판정: 0 새것, 1 중복, 2 창 밖 */
static int win_check(const struct sstate *s, uint32_t seq)
{
	if ((int32_t)(seq - s->next) < 0) {
		return 1;
	}
	const uint32_t d = seq - s->next;
	if (d >= HUBML_WIN) {
		return 2;
	}
	return (s->bits >> d) & 1 ? 1 : 0;
}

static void win_mark(struct sstate *s, uint32_t seq)
{
	s->bits |= 1ull << (seq - s->next);
	while (s->bits & 1) {
		s->bits >>= 1;
		s->next++;
	}
}

static void send_hello_ack(struct edge *e, uint16_t sid)
{
	uint8_t pl[MCOL_HELLO_ACK_LEN];
	sys_put_le16(sid, pl);
	sys_put_le16(0, pl + 2);
	sys_put_le64((uint64_t)hubml_time_now(), pl + 4);
	sys_put_le32(600, pl + 12);	/* 원파형 표본 주기 (s) */
	struct mcol_hdr h = { .type = MCOL_T_HELLO_ACK, .edge_sid = sid, .stream = MCOL_S_CTRL,
			      .len = sizeof(pl) };
	uint8_t out[64];
	const int n = mcol_encode(out, sizeof out, &h, pl);
	if (n > 0 && xp && xp->send) {
		xp->send(e->addr, out, (size_t)n);
	}
}

static void on_hello(uint32_t addr, const struct mcol_hdr *h, const uint8_t *pl)
{
	if (h->len < MCOL_HELLO_LEN) {
		st.bad++;
		return;
	}
	struct mcol_hello m;
	mcol_hello_get(pl, &m);
	int free_i = -1, i;
	for (i = 0; i < HUBML_MAX_EDGES; i++) {
		if (edges[i].used && edges[i].uid == m.uid) {
			break;
		}
		if (!edges[i].used && free_i < 0) {
			free_i = i;
		}
	}
	if (i == HUBML_MAX_EDGES) {
		if (free_i < 0) {
			LOG_WRN("장비 표가 찼다 (uid %016llx)", (unsigned long long)m.uid);
			return;
		}
		i = free_i;
		memset(&edges[i], 0, sizeof(edges[i]));
		edges[i].used = true;
		edges[i].uid = m.uid;
		LOG_INF("새 장비 uid %016llx → sid %d", (unsigned long long)m.uid, i + 1);
	}
	struct edge *e = &edges[i];
	e->addr = addr;
	e->boot_id = h->boot_id;
	e->fw = m.fw;
	e->contract = m.contract;
	e->schema_hash = m.schema_hash;
	e->last_rx_ms = hubml_time_now();
	e->need_ack = true;	/* 다시 붙은 장비에 지금까지 기록된 곳을 알려 준다 */
	send_hello_ack(e, (uint16_t)(i + 1));
}

void hubml_ingest_rx(uint32_t addr, const uint8_t *buf, size_t n)
{
	struct mcol_hdr h;
	const uint8_t *pl;
	k_mutex_lock(&st_mtx, K_FOREVER);
	st.rx++;
	if (mcol_decode(buf, n, &h, &pl)) {
		st.bad++;
		goto out;
	}
	if (h.type == MCOL_T_HELLO) {
		on_hello(addr, &h, pl);
		goto out;
	}
	struct edge *e = edge_by_sid(h.edge_sid);
	if (e == NULL) {
		st.nosid++;	/* HELLO 전 — Edge 가 다시 HELLO 한다 */
		goto out;
	}
	e->rx++;
	e->addr = addr;
	e->last_rx_ms = hubml_time_now();
	if (h.stream == MCOL_S_CTRL) {
		k_mutex_unlock(&st_mtx);
		hubml_on_ctrl(h.edge_sid, &h, pl);	/* 제어 흐름은 저장하지 않는다 — 배포 응답 등 */
		return;
	}
	struct sstate *s = &e->rcv[h.stream];
	const int w = win_check(s, h.seq);
	if (w == 1) {
		e->dup++;
		st.dup++;
		e->need_ack = true;	/* ACK 를 잃었을 수 있다 — 다시 알려 준다 */
		goto out;
	}
	if (w == 2) {
		e->oow++;
		st.oow++;
		goto out;	/* 창 밖 — 앞 순번이 기록된 뒤 Edge 가 다시 보낸다 */
	}
	struct qbuf *q = &qb[act];
	if (q->n >= QMAX_REC || q->used + n > sizeof(q->data)) {
		e->qfull++;
		st.qfull++;
		goto out;	/* 받지 않은 것으로 둔다 — Edge 가 다시 보낸다 */
	}
	struct qrec *r = &q->rec[q->n++];
	r->sid = h.edge_sid;
	r->stream = h.stream;
	r->len = (uint16_t)n;
	r->off = (uint32_t)q->used;
	r->hub_ms = e->last_rx_ms;
	memcpy(q->data + q->used, buf, n);
	q->used += n;
	win_mark(s, h.seq);
	st.accepted++;
out:
	k_mutex_unlock(&st_mtx);
}

/* ── 파일 ─────────────────────────────────────────────────────────────── */
static int ensure_dir(const char *p)
{
	struct fs_dirent d;
	if (fs_stat(p, &d) == 0) {
		return d.type == FS_DIR_ENTRY_DIR ? 0 : -ENOTDIR;
	}
	const int rc = fs_mkdir(p);
	return (rc == -EEXIST) ? 0 : rc;
}

static void seg_path(char *out, size_t n, uint64_t uid, const char *day, uint8_t stream)
{
	snprintf(out, n, "%s/ds/%016llx/%s/s%u.mcs", root, (unsigned long long)uid, day, stream);
}

static int seg_open(struct fs_file_t *f, uint64_t uid, const char *day, uint8_t stream)
{
	char p[96];
	snprintf(p, sizeof p, "%s/ds", root);
	int rc = ensure_dir(p);
	if (!rc) {
		snprintf(p, sizeof p, "%s/ds/%016llx", root, (unsigned long long)uid);
		rc = ensure_dir(p);
	}
	if (!rc) {
		snprintf(p, sizeof p, "%s/ds/%016llx/%s", root, (unsigned long long)uid, day);
		rc = ensure_dir(p);
	}
	if (rc) {
		return rc;
	}
	seg_path(p, sizeof p, uid, day, stream);
	struct fs_dirent d;
	const bool fresh = fs_stat(p, &d) != 0 || d.size == 0;
	fs_file_t_init(f);
	rc = fs_open(f, p, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc) {
		return rc;
	}
	if (fresh) {
		uint8_t hd[16] = { 'M', 'C', 'S', '1' };
		sys_put_le64(uid, hd + 4);
		hd[12] = stream;
		hd[13] = MCOL_VER;
		if (fs_write(f, hd, sizeof hd) != sizeof hd) {
			fs_close(f);
			return -EIO;
		}
	}
	return 0;
}

/* 상태 파일: 'MLS2' · n · rsv · n × { sid u16 · uid u64 · boot u16 · fw u32 · contract u16 · next[5] u32 · bits[5] u64 ·
 *   schema_hash u32 }   ('MLS1' 은 schema 없이 — 읽을 수 있다. 2026-11-09 실기: 재시작 뒤 schema 가 0 으로 보였다) */
#define ST_REC1 (8 + 2 + 4 + 2 + 4 * MCOL_NSTREAM + 8 * MCOL_NSTREAM)
#define ST_REC (ST_REC1 + 4)
static int state_save(const struct edge *snap)
{
	static uint8_t b[8 + HUBML_MAX_EDGES * (2 + ST_REC)];
	size_t o = 8;
	uint16_t n = 0;
	for (int i = 0; i < HUBML_MAX_EDGES; i++) {
		if (!snap[i].used) {
			continue;
		}
		sys_put_le16((uint16_t)(i + 1), b + o);
		o += 2;
		sys_put_le64(snap[i].uid, b + o);
		sys_put_le16(snap[i].boot_id, b + o + 8);
		sys_put_le32(snap[i].fw, b + o + 10);
		sys_put_le16(snap[i].contract, b + o + 14);
		for (int s = 0; s < MCOL_NSTREAM; s++) {
			sys_put_le32(snap[i].dur[s].next, b + o + 16 + 4 * s);
			sys_put_le64(snap[i].dur[s].bits, b + o + 16 + 4 * MCOL_NSTREAM + 8 * s);
		}
		sys_put_le32(snap[i].schema_hash, b + o + ST_REC1);
		o += ST_REC;
		n++;
	}
	memcpy(b, "MLS2", 4);
	sys_put_le16(n, b + 4);
	sys_put_le16(0, b + 6);
	char p[64];
	snprintf(p, sizeof p, "%s/ml_state.bin", root);
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, p, FS_O_CREATE | FS_O_WRITE);
	if (rc) {
		return rc;
	}
	rc = fs_truncate(&f, 0);
	if (!rc && fs_write(&f, b, o) != (ssize_t)o) {
		rc = -EIO;
	}
	const int rc2 = fs_close(&f);
	return rc ? rc : rc2;
}

static int state_load(void)
{
	static uint8_t b[8 + HUBML_MAX_EDGES * (2 + ST_REC)];
	char p[64];
	snprintf(p, sizeof p, "%s/ml_state.bin", root);
	struct fs_file_t f;
	fs_file_t_init(&f);
	if (fs_open(&f, p, FS_O_READ)) {
		return 0;	/* 처음 */
	}
	const ssize_t n = fs_read(&f, b, sizeof b);
	fs_close(&f);
	const bool v2 = n >= 8 && !memcmp(b, "MLS2", 4);
	const size_t rec = v2 ? ST_REC : ST_REC1;
	if (n < 8 || (!v2 && memcmp(b, "MLS1", 4))) {
		LOG_ERR("상태 파일이 깨졌다 — 처음부터 (데이터 파일은 그대로)");
		return -EBADMSG;
	}
	const uint16_t cnt = sys_get_le16(b + 4);
	size_t o = 8;
	for (int k = 0; k < cnt && o + 2 + rec <= (size_t)n; k++) {
		const uint16_t sid = sys_get_le16(b + o);
		o += 2;
		if (sid == 0 || sid > HUBML_MAX_EDGES) {
			o += rec;
			continue;
		}
		struct edge *e = &edges[sid - 1];
		memset(e, 0, sizeof(*e));
		e->used = true;
		e->uid = sys_get_le64(b + o);
		e->boot_id = sys_get_le16(b + o + 8);
		e->fw = sys_get_le32(b + o + 10);
		e->contract = sys_get_le16(b + o + 14);
		for (int s = 0; s < MCOL_NSTREAM; s++) {
			e->dur[s].next = sys_get_le32(b + o + 16 + 4 * s);
			e->dur[s].bits = sys_get_le64(b + o + 16 + 4 * MCOL_NSTREAM + 8 * s);
			e->rcv[s] = e->dur[s];
		}
		e->schema_hash = v2 ? sys_get_le32(b + o + ST_REC1) : 0;
		o += rec;
	}
	LOG_INF("상태 파일에서 장비 %u 대를 이어 받았다", cnt);
	return 0;
}

int hubml_ingest_init(const char *r, const struct hubml_xport *x)
{
	k_mutex_lock(&st_mtx, K_FOREVER);
	strncpy(root, r, sizeof root - 1);
	xp = x;
	memset(edges, 0, sizeof edges);
	memset(&st, 0, sizeof st);
	qb[0].n = qb[1].n = 0;
	qb[0].used = qb[1].used = 0;
	act = 0;
	k_mutex_unlock(&st_mtx);
	hubml_fs_lock();
	int rc = ensure_dir(root);
	if (!rc) {
		rc = state_load();
	}
	hubml_fs_unlock();
	return rc;
}

void hubml_ingest_crash(void)
{
	k_mutex_lock(&st_mtx, K_FOREVER);
	memset(edges, 0, sizeof edges);
	qb[0].n = qb[1].n = 0;
	qb[0].used = qb[1].used = 0;
	k_mutex_unlock(&st_mtx);
}

static int rec_cmp(const void *a, const void *b)
{
	const struct qrec *x = a, *y = b;
	if (x->sid != y->sid) {
		return x->sid < y->sid ? -1 : 1;
	}
	if (x->stream != y->stream) {
		return x->stream < y->stream ? -1 : 1;
	}
	return x->off < y->off ? -1 : (x->off > y->off ? 1 : 0);
}

static void send_acks(const struct edge *snap)
{
	static struct edge before[HUBML_MAX_EDGES];
	for (int i = 0; i < HUBML_MAX_EDGES; i++) {
		if (!snap[i].used) {
			continue;
		}
		k_mutex_lock(&st_mtx, K_FOREVER);
		const bool need = edges[i].need_ack;
		edges[i].need_ack = false;
		const uint32_t addr = edges[i].addr;
		k_mutex_unlock(&st_mtx);
		if (!need && !memcmp(before[i].dur, snap[i].dur, sizeof snap[i].dur)) {
			continue;
		}
		if (addr == 0) {
			continue;	/* 재시작 뒤 아직 말이 없는 장비 — 주소를 모른다(실기: addr=0 으로 보냈다). 다음에 말하면 보낸다 */
		}
		uint8_t pl[MCOL_ACK_ENTRY_LEN * MCOL_NSTREAM];
		size_t o = 0;
		for (int s = 1; s < MCOL_NSTREAM; s++) {
			pl[o] = (uint8_t)s;
			sys_put_le32(snap[i].dur[s].next, pl + o + 1);
			sys_put_le32((uint32_t)(snap[i].dur[s].bits >> 1), pl + o + 5);
			o += MCOL_ACK_ENTRY_LEN;
		}
		struct mcol_hdr h = { .type = MCOL_T_ACK, .edge_sid = (uint16_t)(i + 1),
				      .stream = MCOL_S_CTRL, .len = (uint16_t)o };
		uint8_t out[MCOL_HDR_LEN + sizeof(pl) + MCOL_CRC_LEN];
		const int n = mcol_encode(out, sizeof out, &h, pl);
		if (n > 0 && xp && xp->send) {
			xp->send(addr, out, (size_t)n);
			st.acks++;
		}
		before[i] = snap[i];
	}
}

int hubml_ingest_commit(void)
{
	static struct edge snap[HUBML_MAX_EDGES];
	k_mutex_lock(&commit_mtx, K_FOREVER);
	const int64_t t0 = k_uptime_get();

	/* 1) 대기열을 바꿔 끼우고 받은 상태를 사진 찍는다 — 이 사진 = 이번 기록이 끝나면 기록된 것 */
	k_mutex_lock(&st_mtx, K_FOREVER);
	struct qbuf *q = &qb[act];
	act ^= 1;
	qb[act].n = 0;
	qb[act].used = 0;
	for (int i = 0; i < HUBML_MAX_EDGES; i++) {
		snap[i] = edges[i];
		for (int s = 0; s < MCOL_NSTREAM; s++) {
			snap[i].dur[s] = edges[i].rcv[s];
		}
	}
	k_mutex_unlock(&st_mtx);

	/* 2) (장비, 흐름) 별로 모아 파일 하나씩 연다 */
	qsort(q->rec, q->n, sizeof(q->rec[0]), rec_cmp);
	int rc = 0;
	hubml_fs_lock();
	for (int i = 0; i < q->n && !rc;) {
		const struct qrec *r = &q->rec[i];
		const struct edge *e = &snap[r->sid - 1];
		char day[9];
		hubml_day_str(r->hub_ms, day);
		struct fs_file_t f;
		rc = seg_open(&f, e->uid, day, r->stream);
		if (rc) {
			break;
		}
		int j = i;
		for (; j < q->n && q->rec[j].sid == r->sid && q->rec[j].stream == r->stream; j++) {
			char d2[9];
			hubml_day_str(q->rec[j].hub_ms, d2);
			if (strcmp(d2, day)) {
				break;	/* 날이 바뀌면 다음 파일 */
			}
			uint8_t lp[2];
			sys_put_le16(q->rec[j].len, lp);
			if (fs_write(&f, lp, 2) != 2 ||
			    fs_write(&f, q->data + q->rec[j].off, q->rec[j].len) != q->rec[j].len) {
				rc = -EIO;
				break;
			}
			st.written++;
		}
		const int rc2 = fs_close(&f);	/* 닫기 = 디렉터리 항목까지 동기화 */
		if (!rc) {
			rc = rc2;
		}
		i = j;
	}
	/* 3) 데이터가 기록된 다음에 상태 파일 */
	if (!rc && cut_before_state) {
		cut_before_state = false;
		hubml_fs_unlock();
		q->n = 0;
		q->used = 0;
		k_mutex_unlock(&commit_mtx);
		return -ECANCELED;	/* 시험: 데이터는 디스크에, 상태·ACK 는 없음 */
	}
	if (!rc) {
		rc = state_save(snap);
	}
	hubml_fs_unlock();

	if (rc) {
		/* 기록 실패 — ACK 하지 않는다. 받은 상태를 기록된 상태로 되돌려 Edge 가 다시 보내게 한다 */
		k_mutex_lock(&st_mtx, K_FOREVER);
		for (int i = 0; i < HUBML_MAX_EDGES; i++) {
			if (edges[i].used) {
				memcpy(edges[i].rcv, edges[i].dur, sizeof edges[i].rcv);
			}
		}
		qb[act].n = 0;
		qb[act].used = 0;
		k_mutex_unlock(&st_mtx);
		st.commit_err++;
		LOG_ERR("기록 실패 %d — ACK 하지 않는다", rc);
	} else {
		k_mutex_lock(&st_mtx, K_FOREVER);
		for (int i = 0; i < HUBML_MAX_EDGES; i++) {
			if (edges[i].used && snap[i].used) {
				memcpy(edges[i].dur, snap[i].dur, sizeof edges[i].dur);
			}
		}
		k_mutex_unlock(&st_mtx);
		send_acks(snap);
	}
	q->n = 0;
	q->used = 0;
	st.commits++;
	st.last_commit_ms = (uint32_t)(k_uptime_get() - t0);
	st.max_commit_ms = MAX(st.max_commit_ms, st.last_commit_ms);
	k_mutex_unlock(&commit_mtx);
	return rc;
}

__weak void hubml_on_ctrl(uint16_t sid, const struct mcol_hdr *h, const uint8_t *pl)
{
	ARG_UNUSED(sid); ARG_UNUSED(h); ARG_UNUSED(pl);
}

int hubml_ingest_find(uint64_t uid, uint16_t *sid, uint32_t *addr)
{
	int rc = -ENOENT;
	k_mutex_lock(&st_mtx, K_FOREVER);
	for (int i = 0; i < HUBML_MAX_EDGES; i++) {
		if (edges[i].used && edges[i].uid == uid) {
			*sid = (uint16_t)(i + 1);
			*addr = edges[i].addr;
			rc = 0;
			break;
		}
	}
	k_mutex_unlock(&st_mtx);
	return rc;
}

int hubml_ingest_send(uint32_t addr, const uint8_t *buf, size_t n)
{
	return (xp && xp->send) ? xp->send(addr, buf, n) : -ENODEV;
}

void hubml_ingest_stats(struct hubml_stats *out)
{
	k_mutex_lock(&st_mtx, K_FOREVER);
	*out = st;
	out->queued = qb[act].used;
	k_mutex_unlock(&st_mtx);
}

int hubml_ingest_edges(struct hubml_edge_info *out, int max)
{
	int n = 0;
	k_mutex_lock(&st_mtx, K_FOREVER);
	for (int i = 0; i < HUBML_MAX_EDGES && n < max; i++) {
		const struct edge *e = &edges[i];
		if (!e->used) {
			continue;
		}
		struct hubml_edge_info *o = &out[n++];
		o->uid = e->uid;
		o->sid = (uint16_t)(i + 1);
		o->addr = e->addr;
		o->boot_id = e->boot_id;
		o->fw = e->fw;
		o->contract = e->contract;
		o->schema_hash = e->schema_hash;
		o->last_rx_ms = e->last_rx_ms;
		for (int s = 0; s < MCOL_NSTREAM; s++) {
			o->next[s] = e->dur[s].next;
		}
		o->rx = e->rx;
		o->dup = e->dup;
		o->oow = e->oow;
		o->qfull = e->qfull;
	}
	k_mutex_unlock(&st_mtx);
	return n;
}

/* ── 읽기 ─────────────────────────────────────────────────────────────── */
struct seen {
	uint32_t base;
	uint32_t n;
	uint8_t *bits;
};

int hubml_store_scan(const char *r, uint64_t uid, const char *day, uint8_t stream,
		     hubml_rec_cb cb, void *user, uint32_t *phys_dup)
{
	char p[96];
	snprintf(p, sizeof p, "%s/ds/%016llx/%s/s%u.mcs", r, (unsigned long long)uid, day, stream);
	struct fs_file_t f;
	fs_file_t_init(&f);
	hubml_fs_lock();
	struct fs_dirent de;
	if (fs_stat(p, &de) != 0) {
		hubml_fs_unlock();
		return -ENOENT;	/* 그날 파일이 없다 — 오류 로그 없이 */
	}
	int rc = fs_open(&f, p, FS_O_READ);
	if (rc) {
		hubml_fs_unlock();
		return rc;
	}
	uint8_t hd[16];
	if (fs_read(&f, hd, 16) != 16 || memcmp(hd, "MCS1", 4) || sys_get_le64(hd + 4) != uid) {
		fs_close(&f);
		hubml_fs_unlock();
		return -EBADMSG;
	}
	/* 본 seq 를 비트로 (흐름당 최대 1 M 레코드 — 그 이상은 하루 파일 하나에 오지 않는다) */
	const uint32_t cap = 1u << 20;
	uint8_t *bits = k_calloc(cap / 8, 1);
	if (!bits) {
		fs_close(&f);
		hubml_fs_unlock();
		return -ENOMEM;
	}
	static uint8_t rec[MCOL_MAX_DGRAM];
	int count = 0;
	for (;;) {
		uint8_t lp[2];
		if (fs_read(&f, lp, 2) != 2) {
			break;
		}
		const uint16_t len = sys_get_le16(lp);
		if (len > sizeof rec || fs_read(&f, rec, len) != len) {
			rc = -EBADMSG;
			break;
		}
		struct mcol_hdr h;
		const uint8_t *pl;
		if (mcol_decode(rec, len, &h, &pl)) {
			rc = -EBADMSG;
			break;
		}
		if (h.seq >= cap) {
			rc = -E2BIG;
			break;
		}
		if (bits[h.seq / 8] & (1u << (h.seq % 8))) {
			if (phys_dup) {
				(*phys_dup)++;
			}
			continue;
		}
		bits[h.seq / 8] |= 1u << (h.seq % 8);
		if (cb) {
			cb(&h, pl, user);
		}
		count++;
	}
	k_free(bits);
	fs_close(&f);
	hubml_fs_unlock();
	return rc ? rc : count;
}
