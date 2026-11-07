/*
 * L1 — 대용량 메모리 관문.
 *
 * 사다리(docs/llm-hub-test-plan.md) 둘째 칸. 가리는 것:
 *   "Zephyr 창(128 MB) 밖의 DRAM 을 읽고 쓸 수 있는가"
 * Qwen3.5-0.8B Q4 가 약 0.55 GB 라 창에 안 들어간다. 이게 안 되면
 * L2(GGUF 파서) 이후가 전부 성립하지 않는다.
 *
 * 패턴은 **주소의 함수**다:  word[i] = (uint32)(i * 2654435761) ^ 'moru'
 * 그래서 ① 호스트에서 numpy 로 벡터화해 골든 CRC 를 만들 수 있고
 *        ② 주소가 섞이는 오류(별칭·줄바꿈·바이트순서)를 값으로 잡는다.
 *        단순히 0으로 채우면 창이 실제 RAM 보다 커도 통과해 버린다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <stdint.h>
#include <inttypes.h>

#define BLOB_NODE	DT_NODELABEL(model_blob)
#define BLOB_ADDR	DT_REG_ADDR(BLOB_NODE)
#define BLOB_SIZE	DT_REG_SIZE(BLOB_NODE)

#define PATTERN_MULT	2654435761u	/* Knuth 곱셈 해시 */
#define PATTERN_XOR	0x6D6F7275u	/* 'moru' */

/* 별칭 사전 점검의 보폭. 2 MB = 매핑 블록 하나당 한 번 */
#define ALIAS_STRIDE	(2u * 1024u * 1024u)

/* CRC 를 나눠 계산하는 단위 */
#define CRC_CHUNK	(64u * 1024u)

static inline uint32_t pattern_word(uint32_t i)
{
	return (uint32_t)(i * PATTERN_MULT) ^ PATTERN_XOR;
}

/*
 * 골든 CRC32 — 호스트에서 tools/l1_golden.py 로 계산한 값.
 *
 *   $ python3 tools/l1_golden.py --size 512M   →  0xc9806e9c
 *
 * 영역 크기가 DT 에 고정돼 있으므로 이 값은 이 시험의 상수다.
 * ⚠ apps/llm_common/model_blob.overlay 의 reg 크기나 pattern_word() 를
 *   바꾸면 다시 만들어야 한다. 아래 BUILD_ASSERT 가 먼저 막는다.
 */
#ifndef L1_GOLDEN_CRC32
#define L1_GOLDEN_CRC32 0xc9806e9cu
#endif
BUILD_ASSERT(BLOB_SIZE == 512u * 1024u * 1024u,
	     "골든 CRC32 는 512 MiB 전용이다 — 크기를 바꿨으면 tools/l1_golden.py 로 다시 만들어라");

int main(void)
{
	int fail = 0;

	printk("=== L1: 대용량 메모리 관문 ===\n");
	printk("  창: 0x%08" PRIxPTR " + %u MiB  (DT model_blob, zephyr,memory-region)\n",
	       (uintptr_t)BLOB_ADDR, (unsigned)(BLOB_SIZE / (1024 * 1024)));

	volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)BLOB_ADDR;
	const uint32_t nwords = (uint32_t)(BLOB_SIZE / sizeof(uint32_t));

	/*
	 * [1] 별칭 사전 점검.
	 * 2 MB 마다 그 주소에서만 나오는 값을 넣고 전부 되읽는다. 창이 실제
	 * RAM 보다 크면 뒤쪽 쓰기가 앞쪽을 덮어 여기서 걸린다 — 512 MB 를
	 * 다 쓰기 전에, 싸게.
	 */
	printk("  [1] 별칭 점검 (2 MiB 보폭 %u 지점) ... ", (unsigned)(BLOB_SIZE / ALIAS_STRIDE));
	for (uint32_t off = 0; off < BLOB_SIZE; off += ALIAS_STRIDE) {
		w[off / 4] = pattern_word(off / 4);
	}
	uint32_t alias_bad = 0;
	for (uint32_t off = 0; off < BLOB_SIZE; off += ALIAS_STRIDE) {
		if (w[off / 4] != pattern_word(off / 4)) {
			if (alias_bad == 0) {
				printk("\n      첫 불일치 off=0x%08x 읽음=0x%08x 기대=0x%08x\n",
				       off, w[off / 4], pattern_word(off / 4));
			}
			alias_bad++;
		}
	}
	if (alias_bad == 0) {
		printk("OK\n");
	} else {
		printk("      FAIL — %u 지점 불일치 (창이 실제 RAM 보다 크다)\n", alias_bad);
		fail++;
		goto done;	/* 여기서 틀리면 뒤는 의미 없다 */
	}

	/* [2] 전 영역 쓰기 */
	printk("  [2] 전 영역 쓰기 %u MiB ... ", (unsigned)(BLOB_SIZE / (1024 * 1024)));
	int64_t t0 = k_uptime_get();
	for (uint32_t i = 0; i < nwords; i++) {
		w[i] = pattern_word(i);
	}
	int64_t t_wr = k_uptime_get() - t0;
	printk("%" PRId64 " ms", t_wr);
	if (t_wr > 0) {
		printk("  (%" PRId64 " MiB/s)", (int64_t)(BLOB_SIZE / 1024 / 1024) * 1000 / t_wr);
	}
	printk("\n");

	/* [3] 되읽기 + 값 비교 — CRC 가 맞아도 어디가 틀렸는지 알려 준다 */
	printk("  [3] 되읽기 값 비교 ... ");
	uint32_t bad = 0;
	for (uint32_t i = 0; i < nwords; i++) {
		if (w[i] != pattern_word(i)) {
			if (bad == 0) {
				printk("\n      첫 불일치 i=%u (off=0x%08x) 읽음=0x%08x 기대=0x%08x\n",
				       i, i * 4u, w[i], pattern_word(i));
			}
			bad++;
		}
	}
	if (bad == 0) {
		printk("OK\n");
	} else {
		printk("      FAIL — %u 워드 불일치\n", bad);
		fail++;
	}

	/* [4] CRC32 — 호스트 골든값과 대조 */
	printk("  [4] CRC32 (%u KiB 청크) ... ", (unsigned)(CRC_CHUNK / 1024));
	t0 = k_uptime_get();
	uint32_t crc = 0;
	const uint8_t *p = (const uint8_t *)(uintptr_t)BLOB_ADDR;
	for (uint32_t off = 0; off < BLOB_SIZE; off += CRC_CHUNK) {
		uint32_t n = CRC_CHUNK;
		if (off + n > BLOB_SIZE) {
			n = (uint32_t)BLOB_SIZE - off;
		}
		crc = crc32_ieee_update(crc, p + off, n);
	}
	int64_t t_crc = k_uptime_get() - t0;
	printk("0x%08x  (%" PRId64 " ms)\n", crc, t_crc);

	if (L1_GOLDEN_CRC32 == 0u) {
		printk("      ⚠ 골든값이 안 박혔다 — tools/l1_golden.py 로 만들어 넣어라\n");
		printk("        비교를 건너뛴다 (이 실행은 PASS 로 셈하지 않는다)\n");
		fail++;
	} else if (crc == (uint32_t)L1_GOLDEN_CRC32) {
		printk("      OK — 호스트 골든값 0x%08x 와 일치\n", (uint32_t)L1_GOLDEN_CRC32);
	} else {
		printk("      FAIL — 골든값 0x%08x 와 다르다\n", (uint32_t)L1_GOLDEN_CRC32);
		fail++;
	}

done:
	if (fail == 0) {
		printk("=== L1 PASS (실패 0) ===\n");
	} else {
		printk("=== L1 FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
