/*
 * L4 — ggml 커널 관문.
 *
 * 사다리(docs/llm-hub-test-plan.md) 다섯째 칸: 작은 행렬곱 · RMSNorm · SiLU ·
 * RoPE, **dotprod 포함**.
 *
 * 그래프는 src/l4_graph.c 하나다 — 호스트 골든 도구도 **같은 파일**을
 * 컴파일한다. 양쪽에 따로 쓰면 전사 오류를 "커널이 다르다" 로 오진한다.
 *
 * 허용 오차: 계획서는 상대 1e-5 다(f32 누산 순서 차이). 비트 일치면 그렇게
 * 적고, 아니면 최대 상대오차를 ppb 로 찍는다 — 숫자를 봐야 판단할 수 있다.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "l4_graph.h"
#include "l4_golden.h"

/* 계획서의 L4 허용 오차 1e-5 = 10,000 ppb */
#define TOL_PPB		10000ull

static uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float fabsf_(float f) { return f < 0 ? -f : f; }

/* 상대오차를 ppb(10억분율)로. printk 에 부동소수를 쓰지 않는다. */
static uint64_t rel_ppb(float got, float want)
{
	float d = fabsf_(got - want);
	float m = fabsf_(want);

	if (d == 0.0f) { return 0; }
	if (m < 1e-30f) { m = 1e-30f; }
	float r = d / m * 1e9f;

	if (r > 1e18f) { return (uint64_t)1e18; }
	return (uint64_t)r;
}

static int check(const char *name, const float *got, const uint32_t *golden, int n)
{
	int nbit = 0;
	uint64_t worst = 0;
	int worst_i = 0;

	printk("  %-12s %4d개 ... ", name, n);
	for (int i = 0; i < n; i++) {
		float want;

		memcpy(&want, &golden[i], 4);
		if (bits(got[i]) != golden[i]) {
			nbit++;
			uint64_t r = rel_ppb(got[i], want);

			if (r > worst) { worst = r; worst_i = i; }
		}
	}
	if (nbit == 0) {
		printk("OK — **비트 일치**\n");
		return 0;
	}
	if (worst <= TOL_PPB) {
		printk("OK — 비트는 %d/%d 다르나 최대 상대오차 %" PRIu64 " ppb ≤ %" PRIu64 "\n",
		       nbit, n, worst, TOL_PPB);
		return 0;
	}
	printk("FAIL — 최대 상대오차 %" PRIu64 " ppb (> %" PRIu64 ") at [%d]\n",
	       worst, TOL_PPB, worst_i);
	printk("         얻음 0x%08x  골든 0x%08x\n", bits(got[worst_i]), golden[worst_i]);
	return 1;
}

static struct l4_results res;

int main(void)
{
	int fail = 0;

	printk("=== L4: ggml 커널 관문 ===\n");

	int rc = l4_run(&res);

	if (rc != 0) {
		printk("  l4_run 실패: %d\n", rc);
		printk("=== L4 FAIL (실패 1) ===\n");
		return 0;
	}

	/*
	 * [0] 커널 기능 집합이 호스트와 같은가.
	 * 다르면 ggml 이 **다른 커널 경로**를 골라 Q4_0 행렬곱 결과가 갈린다.
	 * 계획서가 -cpu max 를 쓰지 말라고 한 이유가 이것이다.
	 */
	printk("  [0] 기능 집합  Zephyr dotprod=%d i8mm=%d / 호스트 %d %d ... ",
	       res.dotprod, res.i8mm, L4_HOST_DOTPROD, L4_HOST_I8MM);
	if (res.dotprod == L4_HOST_DOTPROD && res.i8mm == L4_HOST_I8MM) {
		printk("OK\n");
	} else {
		printk("불일치 — 아래 비교는 다른 커널 경로끼리다\n");
		fail++;
	}

	fail += check("[1] mm f32", res.mm_f32, l4_g_mm_f32, L4_MM_N);
	fail += check("[2] mm Q4_0", res.mm_q4_0, l4_g_mm_q4_0, L4_MM_N);
	fail += check("[3] RMSNorm", res.rms, l4_g_rms, L4_RMS_NE);
	fail += check("[4] SiLU", res.silu, l4_g_silu, L4_SILU_NE);
	fail += check("[5] RoPE", res.rope, l4_g_rope, L4_ROPE_NE);

	if (fail == 0) {
		printk("=== L4 PASS (실패 0) ===\n");
	} else {
		printk("=== L4 FAIL (실패 %d) ===\n", fail);
	}
	return 0;
}
