/*
 * L3 — 역양자화 관문. **허용 오차 0 — 비트 일치**다.
 *
 * ggml 의 C 구현(dequantize_row_q4_0 / dequantize_row_q4_K)을 Zephyr 로
 * 그대로 빌드해 돌리고, 같은 코드를 호스트에서 돌린 결과와 비트 단위로
 * 비교한다. 재구현하지 않는다 — 계획의 B안이 ggml 포팅이고, 재구현하면
 * 그 질문을 피해 갈 뿐 아니라 재구현의 버그를 ggml 의 버그로 착각하게 된다.
 *
 * 골든값: src/l3_golden.h (tools/l3_golden.sh 가 만든다).
 *
 * 왜 비트 일치인가: 정수 언팩 + 고정 스케일 곱이다. 다르면 구현이 틀린 것이다.
 * 실제로 골든값에 **음의 0(0x80000000)** 이 들어 있다 — x0=0 이고 스케일이
 * 음수인 경우다. +0.0 을 내는 구현은 값으로는 같고 비트로는 다르다.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "ggml-quants.h"
#include "l3_golden.h"

#define BLOB_ADDR	DT_REG_ADDR(DT_NODELABEL(model_blob))

static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static int compare(const char *what, const float *got, const uint32_t *golden, int n)
{
	int bad = 0;

	printk("  %s %d개 ... ", what, n);
	for (int i = 0; i < n; i++) {
		if (bits(got[i]) != golden[i]) {
			if (bad < 4) {
				printk("\n      [%d] 얻음 0x%08x  골든 0x%08x", i, bits(got[i]), golden[i]);
				if ((bits(got[i]) ^ golden[i]) == 0x80000000u) {
					printk("   ← **부호만 다르다 (음의 0?)**");
				}
			}
			bad++;
		}
	}
	if (bad == 0) {
		printk("OK — 비트 일치\n");
		return 0;
	}
	printk("\n      FAIL — %d/%d 비트 불일치\n", bad, n);
	return 1;
}

static float y40[L3_Q4_0_N];
static float y4k[L3_Q4_K_N];

int main(void)
{
	int fail = 0;
	const uint8_t *blob = (const uint8_t *)(uintptr_t)BLOB_ADDR;

	printk("=== L3: 역양자화 관문 (허용 오차 0 — 비트 일치) ===\n");
	printk("  ggml 소스를 그대로 빌드했다: dequantize_row_q4_0 / _q4_K\n");

	/*
	 * [0a] 블롭이 실제로 적재됐는가.
	 * ⚠ 이 검사가 없으면 L_BLOB 을 빼먹었을 때 전부 0 인 메모리를 역양자화해
	 *   Q4_0 은 -8×0 = **-0.0**, Q4_K 는 0×0-0 = **+0.0** 이 나온다. 그럴듯한
	 *   부동소수 값이라 "포팅이 틀렸다" 로 오진하게 된다 — 실제로 한 번 겪었다.
	 */
	printk("  [0a] 블롭 매직 ... ");
	uint32_t magic = 0;
	memcpy(&magic, blob, 4);
	if (magic == 0x46554747u) {
		printk("OK ('GGUF')\n");
	} else {
		printk("FAIL — 0x%08x. 블롭이 없다.\n", magic);
		printk("       L_BLOB=<gguf> ./tools/qemu_run.sh apps/llm_l3 로 돌려라\n");
		printk("=== L3 FAIL (실패 1) ===\n");
		return 0;
	}

	/* 블록 구조체 크기가 ggml 과 같은가 — 패딩이 다르면 전부 어긋난다 */
	printk("  [0] 블록 크기 q4_0=%u q4_K=%u ... ",
	       (unsigned)sizeof(block_q4_0), (unsigned)sizeof(block_q4_K));
	if (sizeof(block_q4_0) == 18 && sizeof(block_q4_K) == 144) {
		printk("OK\n");
	} else {
		printk("FAIL — 기대 18 / 144\n");
		fail++;
	}

	dequantize_row_q4_0((const block_q4_0 *)(blob + L3_Q4_0_FILE_OFF), y40, L3_Q4_0_N);
	fail += compare("[1] Q4_0", y40, l3_q4_0_golden_bits, L3_Q4_0_N);

	dequantize_row_q4_K((const block_q4_K *)(blob + L3_Q4_K_FILE_OFF), y4k, L3_Q4_K_N);
	fail += compare("[2] Q4_K", y4k, l3_q4_k_golden_bits, L3_Q4_K_N);

	if (fail == 0) {
		printk("=== L3 PASS (실패 0) ===\n");
	} else {
		printk("=== L3 FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
