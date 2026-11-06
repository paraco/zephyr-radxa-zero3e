/*
 * L2 — GGUF 파서 관문.
 *
 * 사다리(docs/llm-hub-test-plan.md) 셋째 칸. 가리는 것:
 *   "창 밖에 적재된 GGUF 파일의 헤더·KV·텐서 정보를 정확히 읽는가"
 *
 * 블롭은 QEMU `-device loader,addr=0x60000000` (실기는 U-Boot fatload)로
 * **Zephyr 보다 먼저** 들어간다 — L1 이 가릴 수 없던 경로다.
 *
 * 골든값은 src/l2_golden.h — llama.cpp 의 참조 GGUFWriter 로 만든 파일을
 * 참조 GGUFReader 로 되읽어 뽑았다. 이 파서와 독립이다.
 *
 * 형식은 참조 구현에서 확인했다 (gguf-py/gguf/gguf_reader.py):
 *   magic u32 'GGUF' | version u32 | tensor_count u64 | kv_count u64
 *   KV × kv_count:        key{len u64, 바이트} · type u32 · 값
 *   텐서정보 × count:      name{len u64, 바이트} · n_dims u32 ·
 *                         dims u64×n_dims · type u32 · offset u64
 *   데이터 시작 = 정렬 올림(여기까지의 오프셋, general.alignment 또는 32)
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "l2_golden.h"

#define BLOB_NODE	DT_NODELABEL(model_blob)
#define BLOB_ADDR	DT_REG_ADDR(BLOB_NODE)
#define BLOB_SIZE	DT_REG_SIZE(BLOB_NODE)

#define GGUF_MAGIC	0x46554747u	/* 'G','G','U','F' 리틀엔디안 */

/* GGUFValueType — gguf-py/gguf/constants.py:6905 */
enum gguf_vt {
	GV_UINT8 = 0, GV_INT8 = 1, GV_UINT16 = 2, GV_INT16 = 3,
	GV_UINT32 = 4, GV_INT32 = 5, GV_FLOAT32 = 6, GV_BOOL = 7,
	GV_STRING = 8, GV_ARRAY = 9, GV_UINT64 = 10, GV_INT64 = 11,
	GV_FLOAT64 = 12,
};

/* 커서 — 모든 읽기가 경계를 검사한다. 실제 모델은 0.55 GB 이고 손상될 수 있다. */
struct cur {
	const uint8_t *base;
	size_t size;
	size_t off;
	int err;
};

static bool take(struct cur *c, void *dst, size_t n)
{
	if (c->err || c->off + n > c->size) {
		c->err = 1;
		return false;
	}
	/*
	 * ⚠ memcpy 를 쓴다. GGUF 는 문자열 길이가 가변이라 뒤따르는 u32/u64 가
	 *   정렬되지 않는다. aarch64 하드웨어는 Normal 메모리의 비정렬 적재를
	 *   처리하지만, uint64_t* 로 역참조하면 **컴파일러가 정렬을 가정해**
	 *   잘못된 코드를 낼 수 있다.
	 */
	memcpy(dst, c->base + c->off, n);
	c->off += n;
	return true;
}

static uint32_t u32(struct cur *c) { uint32_t v = 0; take(c, &v, 4); return v; }
static uint64_t u64(struct cur *c) { uint64_t v = 0; take(c, &v, 8); return v; }

/* 문자열 — 복사하지 않고 위치와 길이만 준다 */
static bool gstr(struct cur *c, const char **p, uint64_t *len)
{
	uint64_t n = u64(c);

	if (c->err || n > c->size || c->off + n > c->size) {
		c->err = 1;
		return false;
	}
	*p = (const char *)(c->base + c->off);
	*len = n;
	c->off += n;
	return true;
}

static bool streq(const char *p, uint64_t len, const char *lit)
{
	return strlen(lit) == len && memcmp(p, lit, len) == 0;
}

/* 스칼라 값의 바이트 수. 0 = 가변 또는 미지 */
static size_t vt_size(uint32_t t)
{
	switch (t) {
	case GV_UINT8: case GV_INT8: case GV_BOOL:	return 1;
	case GV_UINT16: case GV_INT16:			return 2;
	case GV_UINT32: case GV_INT32: case GV_FLOAT32:	return 4;
	case GV_UINT64: case GV_INT64: case GV_FLOAT64:	return 8;
	default:					return 0;
	}
}

/* 값 하나를 건너뛴다 (배열·문자열 포함) */
static bool skip_value(struct cur *c, uint32_t t)
{
	if (t == GV_STRING) {
		const char *p; uint64_t n;
		return gstr(c, &p, &n);
	}
	if (t == GV_ARRAY) {
		uint32_t et = u32(c);
		uint64_t n = u64(c);

		if (c->err) { return false; }
		if (et == GV_STRING) {
			for (uint64_t i = 0; i < n; i++) {
				const char *p; uint64_t l;
				if (!gstr(c, &p, &l)) { return false; }
			}
			return true;
		}
		size_t es = vt_size(et);
		if (es == 0) { c->err = 1; return false; }	/* 중첩 배열은 없다 */
		if (n > c->size / es) { c->err = 1; return false; }
		c->off += (size_t)(n * es);
		if (c->off > c->size) { c->err = 1; return false; }
		return true;
	}
	size_t s = vt_size(t);
	if (s == 0) { c->err = 1; return false; }
	c->off += s;
	if (c->off > c->size) { c->err = 1; return false; }
	return true;
}

#define CHECK(cond, fmt, ...) do {					\
	if (cond) {							\
		printk("OK\n");						\
	} else {							\
		printk("FAIL — " fmt "\n", ##__VA_ARGS__);		\
		fail++;							\
	}								\
} while (0)

int main(void)
{
	int fail = 0;
	const uint8_t *blob = (const uint8_t *)(uintptr_t)BLOB_ADDR;

	printk("=== L2: GGUF 파서 관문 ===\n");
	printk("  블롭: 0x%08" PRIxPTR "  기대 크기 %u B\n",
	       (uintptr_t)BLOB_ADDR, (unsigned)L2_FILE_SIZE);

	BUILD_ASSERT(L2_FILE_SIZE <= BLOB_SIZE, "시험 파일이 창보다 크다");

	/*
	 * [1] 적재 자체 — 매직이 보이는가.
	 * 블롭은 Zephyr 의 MMU·캐시가 켜지기 전에 적재기가 써 넣었다.
	 * 캐시 가능 매핑으로 읽어 낡은 줄을 보면 여기서 걸린다 (L1 의 잔여 위험).
	 */
	printk("  [1] 매직 'GGUF' 가 보이는가 ... ");
	uint32_t magic = 0;
	memcpy(&magic, blob, 4);
	CHECK(magic == GGUF_MAGIC, "0x%08x (기대 0x%08x) — 적재가 안 됐거나 낡은 캐시다",
	      magic, GGUF_MAGIC);
	if (magic != GGUF_MAGIC) {
		goto done;	/* 여기서 틀리면 뒤는 전부 의미 없다 */
	}

	/* [2] 파일 전체 CRC32 — 끝에서 끝까지 온전히 적재됐는가 */
	printk("  [2] 파일 CRC32 ... ");
	uint32_t crc = crc32_ieee(blob, L2_FILE_SIZE);
	CHECK(crc == L2_FILE_CRC32, "0x%08x (골든 0x%08x)", crc, L2_FILE_CRC32);

	struct cur c = { .base = blob, .size = L2_FILE_SIZE, .off = 0, .err = 0 };
	(void)u32(&c);					/* 매직 */
	uint32_t ver = u32(&c);
	uint64_t ntensor = u64(&c);
	uint64_t nkv = u64(&c);

	printk("  [3] 버전 %u · 텐서 %" PRIu64 "개 · KV %" PRIu64 "개 ... ",
	       ver, ntensor, nkv);
	CHECK(ver == L2_VERSION && ntensor == L2_TENSOR_COUNT && nkv == L2_KV_COUNT,
	      "골든은 버전 %u · 텐서 %u · KV %u",
	      (unsigned)L2_VERSION, (unsigned)L2_TENSOR_COUNT, (unsigned)L2_KV_COUNT);

	/* [4] KV 전부 훑기 + 값까지 확인하는 몇 개 */
	printk("  [4] KV %u개 훑기 ... ", (unsigned)L2_KV_COUNT);
	uint32_t align = L2_ALIGNMENT;		/* general.alignment 가 없으면 기본 32 */
	int kv_hit = 0;
	for (uint64_t i = 0; i < nkv && !c.err; i++) {
		const char *k; uint64_t klen;

		if (!gstr(&c, &k, &klen)) { break; }
		uint32_t t = u32(&c);
		size_t vpos = c.off;

		if (streq(k, klen, L2_KV_ARCH_KEY)) {
			const char *s; uint64_t sl;
			struct cur p = c;
			if (t == L2_KV_ARCH_TYPE && gstr(&p, &s, &sl) &&
			    streq(s, sl, L2_KV_ARCH_VAL)) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_U32_KEY)) {
			uint32_t v = 0; memcpy(&v, blob + vpos, 4);
			if (t == L2_KV_U32_TYPE && v == L2_KV_U32_VAL) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_STR_KEY)) {
			const char *s; uint64_t sl;
			struct cur p = c;
			if (t == L2_KV_STR_TYPE && gstr(&p, &s, &sl) &&
			    streq(s, sl, L2_KV_STR_VAL)) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_BOOL_T_KEY)) {
			if (t == L2_KV_BOOL_T_TYPE && blob[vpos] == L2_KV_BOOL_T_VAL) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_BOOL_F_KEY)) {
			if (t == L2_KV_BOOL_F_TYPE && blob[vpos] == L2_KV_BOOL_F_VAL) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_F32_KEY)) {
			uint32_t bits = 0; memcpy(&bits, blob + vpos, 4);
			if (t == GV_FLOAT32 && bits == L2_KV_F32_BITS) { kv_hit++; }
		} else if (streq(k, klen, L2_KV_ARR_KEY)) {
			uint32_t et = 0; uint64_t n = 0;
			memcpy(&et, blob + vpos, 4);
			memcpy(&n, blob + vpos + 4, 8);
			if (t == GV_ARRAY && et == L2_KV_ARR_ELEMT && n == L2_KV_ARR_LEN) { kv_hit++; }
		} else if (streq(k, klen, "general.alignment")) {
			memcpy(&align, blob + vpos, 4);
		}

		skip_value(&c, t);
	}
	CHECK(!c.err && kv_hit == 7, "err=%d 값확인 %d/7", c.err, kv_hit);

	/* [5] 텐서 정보 — 이름·차원수·형태·형식·오프셋 전부 */
	printk("  [5] 텐서 정보 %u개 ... ", (unsigned)L2_TENSOR_COUNT);
	int tbad = 0;
	for (uint64_t i = 0; i < ntensor && !c.err; i++) {
		const char *nm; uint64_t nmlen;

		if (!gstr(&c, &nm, &nmlen)) { break; }
		uint32_t nd = u32(&c);
		uint64_t dims[4] = { 0, 0, 0, 0 };

		if (nd > 4) { c.err = 1; break; }
		for (uint32_t d = 0; d < nd; d++) { dims[d] = u64(&c); }
		uint32_t ty = u32(&c);
		uint64_t off = u64(&c);

		if (c.err) { break; }
		if (i >= ARRAY_SIZE(l2_tensors)) { tbad++; continue; }
		const struct l2_golden_tensor *g = &l2_tensors[i];
		bool ok = streq(nm, nmlen, g->name) && nd == g->n_dims &&
			  ty == g->type && off == g->offset;
		for (uint32_t d = 0; d < 4; d++) {
			if (dims[d] != g->dims[d]) { ok = false; }
		}
		if (!ok) {
			printk("\n      [%u] 어긋남: 읽음 n_dims=%u type=%u off=%" PRIu64
			       " dims={%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "}"
			       "\n           골든  %s n_dims=%u type=%u off=%" PRIu64
			       " dims={%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "}\n",
			       (unsigned)i, nd, ty, off, dims[0], dims[1], dims[2], dims[3],
			       g->name, g->n_dims, g->type, g->offset,
			       g->dims[0], g->dims[1], g->dims[2], g->dims[3]);
			tbad++;
		}
	}
	CHECK(!c.err && tbad == 0, "err=%d 어긋난 텐서 %d개", c.err, tbad);

	/* [6] 데이터 시작 — 정렬 올림 */
	printk("  [6] 데이터 시작 정렬(%u) ... ", align);
	size_t data_off = c.off;
	size_t pad = data_off % align;
	if (pad) { data_off += align - pad; }
	CHECK(data_off == L2_DATA_OFFSET, "%u (골든 %u) — 정렬 전 오프셋 %u",
	      (unsigned)data_off, (unsigned)L2_DATA_OFFSET, (unsigned)c.off);

done:
	if (fail == 0) {
		printk("=== L2 PASS (실패 0) ===\n");
	} else {
		printk("=== L2 FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
