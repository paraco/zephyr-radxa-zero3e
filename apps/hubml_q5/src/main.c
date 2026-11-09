/*
 * hubml_q5 (Lm5) — 배포 시험 (QEMU A55). 허브 배포 모듈 ↔ Edge 라이브러리(edge.c, 펌웨어와 같은 코드)를
 * 손실 10 % · 중복 5 % · 순서 뒤바뀜 채널로 잇는다. 장비 A(계약 34) · B(계약 24 — 계약 다름 시험).
 *  1 challenger 로 보내기 → Edge 슬롯 sha256 = 원장 파일 · 원장 deploy ok
 *  2 champion 별칭 없이 champion 슬롯 → 거부 · 3 서명 승격 → ACTIVATE → 슬롯 맞바꿈 · 4 되돌리기
 *  5 전송 중 조각 1 바이트 변조(MCOL CRC 다시 맞춤) → Edge sha256 거부 · 6 kind 1 → Edge 거부
 *  7 계약 다른 Edge → 거부 · 8 Edge 무응답 → 시간 초과 실패 · 9 Edge 점수 = 허브 xmd_score
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <ff.h>
#include <string.h>
#include <stdio.h>
#include <hubml/mcol.h>
#include <hubml/ingest.h>
#include <hubml/ledger.h>
#include <hubml/deploy.h>
#include <hubml/edge.h>
#include <hubml/sha256.h>

#define ROOT "/RAM:/ml"
static FATFS fat;
static struct fs_mount_t mnt = { .type = FS_FATFS, .mnt_point = "/RAM:", .fs_data = &fat };
static const uint8_t k3[] = {
#include "golden_kind3.xmb.inc"
};
static const uint8_t k1[] = {
#include "golden_kind1.xmb.inc"
};
static const char *SIG = "{\"alg\":\"ed25519\",\"key\":\"k1\",\"over\":\"sha256\",\"b64\":\"UNVERIFIED\"}";
static bool ok = true;
#define CHECK(c, ...) do { if (!(c)) { printk("✗ " __VA_ARGS__); printk("\n"); ok = false; } else { printk("✓ " __VA_ARGS__); printk("\n"); } } while (0)

/* ── 채널 ── */
static uint32_t rng = 99;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
struct pkt { bool used, to_hub; uint32_t addr; int due; uint16_t n; uint8_t b[MCOL_MAX_DGRAM]; };
static struct pkt pool[512];
static int now_t, loss = 10, dupp = 5;
static bool b_offline, corrupt_once;
static void put(bool to_hub, uint32_t addr, const uint8_t *b, size_t n)
{
	if ((int)(rnd() % 100) < loss) { return; }
	for (int c = 0; c < ((int)(rnd() % 100) < dupp ? 2 : 1); c++) {
		for (int i = 0; i < ARRAY_SIZE(pool); i++) {
			if (!pool[i].used) {
				pool[i] = (struct pkt){ .used = true, .to_hub = to_hub, .addr = addr, .due = now_t + (int)(rnd() % 3), .n = (uint16_t)n };
				memcpy(pool[i].b, b, n);
				break;
			}
		}
	}
}
static int hub_send(uint32_t addr, const uint8_t *b, size_t n)
{
	static uint8_t c[MCOL_MAX_DGRAM];
	memcpy(c, b, n);
	/* 변조 시험: 다음 CHUNK 하나의 데이터 1 바이트를 바꾸고 MCOL CRC 를 다시 맞춘다 (전송로 CRC 로는 못 잡는 오염) */
	if (corrupt_once && c[3] == MCOL_T_CMD && c[MCOL_HDR_LEN] == MCOL_OP_CHUNK && n > 200) {
		corrupt_once = false;
		c[MCOL_HDR_LEN + 100] ^= 0x40;
		const uint16_t len = sys_get_le16(c + 20);
		sys_put_le32(crc32_ieee(c, MCOL_HDR_LEN + len), c + MCOL_HDR_LEN + len);
	}
	if (addr == 2 && b_offline) { return 0; }
	put(false, addr, c, n);
	return 0;
}
static const struct hubml_xport xp = { .send = hub_send };

/* ── 가짜 Edge 2 대 (edge.c 를 그대로 쓴다) ── */
static struct hubml_edge EA, EB;
static struct { struct hubml_edge *e; uint64_t uid; uint16_t sid; uint16_t contract; } ED[2] = {
	{ &EA, 0x4d50444d0000000aull, 0, 34 }, { &EB, 0x4d50444d0000000bull, 0, 24 } };

static void edge_rx(int k, const uint8_t *b, size_t n)
{
	struct mcol_hdr h;
	const uint8_t *pl;
	if (mcol_decode(b, n, &h, &pl)) { return; }
	if (h.type == MCOL_T_HELLO_ACK) { ED[k].sid = sys_get_le16(pl); return; }
	if (h.type != MCOL_T_CMD) { return; }
	uint8_t r[MCOL_CMD_ACK_LEN], out[128];
	const int rl = hubml_edge_on_cmd(ED[k].e, pl, h.len, r);
	struct mcol_hdr a = { .type = MCOL_T_CMD_ACK, .edge_sid = ED[k].sid, .stream = MCOL_S_CTRL, .len = (uint16_t)rl };
	const int m = mcol_encode(out, sizeof out, &a, r);
	if (!(k == 1 && b_offline)) { put(true, (uint32_t)k + 1, out, (size_t)m); }
}

static void step(void)
{
	for (int i = 0; i < ARRAY_SIZE(pool); i++) {
		if (pool[i].used && pool[i].due <= now_t) {
			pool[i].used = false;
			if (pool[i].to_hub) { hubml_ingest_rx(pool[i].addr, pool[i].b, pool[i].n); }
			else { edge_rx((int)pool[i].addr - 1, pool[i].b, pool[i].n); }
		}
	}
	hubml_deploy_tick();
	now_t++;
}

static int run_until_done(uint64_t uid, int max, struct hubml_dep_status *st)
{
	for (int i = 0; i < max; i++) {
		step();
		if (!hubml_deploy_status(uid, st) && (st->state == HUBML_DEP_DONE || st->state == HUBML_DEP_FAIL)) { return i; }
	}
	return -1;
}

static void sha(const uint8_t *b, size_t n, uint8_t *d) { struct sha256 s; sha256_init(&s); sha256_update(&s, b, n); sha256_final(&s, d); }
static void dump_cb(uint32_t seq, const char *l, size_t n, void *u) { ARG_UNUSED(seq); ARG_UNUSED(u); printk("LEDGER %.*s\n", (int)n, l); }

int main(void)
{
	printk("=== Lm5 hubml 배포 (QEMU) ===\n");
	if (fs_mount(&mnt)) { printk("=== Lm5 FAIL\n"); return 0; }
	fs_mkdir(ROOT);
	hubml_time_set(1794182400000LL);
	hubml_ingest_init(ROOT, &xp);
	hubml_ledger_init(ROOT, "4d425737333500000000000000032106");
	for (int k = 0; k < 2; k++) {
		hubml_edge_init(ED[k].e, ED[k].contract);
		uint8_t pl[MCOL_HELLO_LEN], out[64];
		struct mcol_hello hm = { .uid = ED[k].uid, .fw = 0x0100, .contract = ED[k].contract, .nfeat = (uint8_t)ED[k].contract, .schema_hash = 0x4d463334 };
		mcol_hello_put(pl, &hm);
		struct mcol_hdr h = { .type = MCOL_T_HELLO, .len = sizeof pl };
		const int n = mcol_encode(out, sizeof out, &h, pl);
		for (int t = 0; t < 30 && !ED[k].sid; t++) { if (t % 5 == 0) { put(true, (uint32_t)k + 1, out, (size_t)n); } step(); }
	}
	CHECK(ED[0].sid && ED[1].sid, "두 Edge 가 HELLO 로 sid 를 받음 (%u, %u)", ED[0].sid, ED[1].sid);
	const uint64_t A = ED[0].uid, B = ED[1].uid;
	char m3[25], m1[25], mB[25];
	hubml_model_register(A, k3, sizeof k3, "hub", NULL, m3);
	hubml_model_register(A, k1, sizeof k1, "hub", NULL, m1);
	hubml_model_register(B, k3, sizeof k3, "hub", NULL, mB);
	uint8_t fsha[32];
	sha(k3, sizeof k3, fsha);
	struct hubml_dep_status st;

	/* 1 */
	hubml_alias_set(A, HUBML_A_CHALLENGER, m3, "propose", "hub", NULL);
	CHECK(hubml_deploy_start(A, m3, 1) == 0, "challenger 로 보내기 시작 (%s, %u B)", m3, (unsigned)sizeof k3);
	int t = run_until_done(A, 3000, &st);
	CHECK(st.state == HUBML_DEP_DONE && EA.slot[1].valid && !memcmp(EA.slot[1].sha, fsha, 32),
	      "손실 10 %% 채널로 %d 틱 · 조각 %u 번 보냄 · 다시 보냄 %u · Edge challenger sha256 = 원장 파일", t, st.sent_chunks, st.resends);
	/* 2 */
	CHECK(hubml_deploy_start(A, m3, 0) == -EACCES, "champion 별칭 없이 champion 슬롯 → 거부");
	CHECK(hubml_deploy_activate(A) == -EACCES, "champion 별칭 없이 ACTIVATE → 거부");
	/* 3 */
	hubml_alias_set(A, HUBML_A_CHAMPION, m3, "promote", "studio:k1", SIG);
	CHECK(hubml_deploy_activate(A) == 0, "서명 승격 뒤 ACTIVATE");
	run_until_done(A, 1000, &st);
	CHECK(st.state == HUBML_DEP_DONE && EA.slot[0].valid && !memcmp(EA.slot[0].sha, fsha, 32) && !EA.slot[1].valid,
	      "Edge champion = %s (challenger 는 비었음 — 옛 champion 이 없었다)", m3);
	/* 9 */
	{
		static const uint32_t PV[][36] = {
#include "pv_vectors.inc"
		};
		int same = 0;
		for (int i = 0; i < ARRAY_SIZE(PV); i++) {
			float x[34], c, h;
			for (int k = 0; k < 34; k++) { memcpy(&x[k], &PV[i][k], 4); }
			hubml_edge_score(&EA, x, &c, &h);
			same += !memcmp(&c, &PV[i][35], 4);
		}
		CHECK(same == ARRAY_SIZE(PV), "Edge champion 점수 = 허브 점수 비트까지 %d/%d", same, ARRAY_SIZE(PV));
	}
	/* 5 변조 */
	corrupt_once = true;
	hubml_deploy_start(A, m3, 1);
	run_until_done(A, 3000, &st);
	CHECK(st.state == HUBML_DEP_FAIL && st.code == -3 && !EA.slot[1].valid, "전송 중 1 바이트 변조(CRC 다시 맞춤) → Edge sha256 거부 (code %d), 슬롯 그대로", st.code);
	/* 4 되돌리기: challenger 에 다시 넣고 ACTIVATE 두 번 = 맞바꿈 두 번 */
	hubml_deploy_start(A, m3, 1);
	run_until_done(A, 3000, &st);
	CHECK(st.state == HUBML_DEP_DONE && EA.slot[1].valid, "변조 뒤 다시 보내기 성공");
	/* 6 kind 1 */
	hubml_deploy_start(A, m1, 1);
	run_until_done(A, 6000, &st);
	CHECK(st.state == HUBML_DEP_FAIL && st.code == -4, "kind 1 (트리) → Edge 거부 (code %d) — Edge 는 kind 3 만", st.code);
	/* 7 계약 다른 Edge */
	hubml_deploy_start(B, mB, 1);
	run_until_done(B, 3000, &st);
	CHECK(st.state == HUBML_DEP_FAIL && st.code == -5 && !EB.slot[1].valid, "계약 24 인 Edge 에 계약 34 모델 → 거부 (code %d)", st.code);
	/* 8 무응답 */
	b_offline = true;
	hubml_deploy_start(B, mB, 1);
	t = run_until_done(B, 2000, &st);
	CHECK(st.state == HUBML_DEP_FAIL && st.code == -ETIMEDOUT, "Edge 무응답 → %d 틱 뒤 시간 초과 실패", t);
	b_offline = false;

	struct hubml_ledger_head h;
	hubml_ledger_head(&h);
	CHECK(h.ok, "원장 사슬 정상 (%u 줄)", h.next_seq - 1);
	hubml_ledger_read(1, 1000, dump_cb, NULL);
	printk(ok ? "=== Lm5 PASS\n" : "=== Lm5 FAIL\n");
	return 0;
}
