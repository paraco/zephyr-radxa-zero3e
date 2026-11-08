/*
 * L7a — 토크나이저. 보드가 글자를 호스트와 **같은 토큰 id** 로 바꾸나.
 *
 * L7 의 첫 칸. L6c 는 토큰 id 를 고정해 넣었으므로 토크나이저를 지나지
 * 않았다. 여기서는 순전파 없이 토크나이저만 본다.
 *
 *   [1] 블롭이 실제 모델인가
 *   [2] 어휘만 적재 (vocab_only — 가중치는 읽지 않는다)
 *   [3] 문장마다 tokenize → 호스트 id 열과 **전부 일치** 요구
 *       + detokenize → 호스트의 되돌림 결과와 **바이트 일치** 요구
 *   [4] 시간 — 긴 문장 100 회
 *
 * 정답값: tools/l7a_host_tok.cpp → src/golden.inc (같은 llama.cpp 소스).
 * 문장: src/cases.h (호스트와 같은 파일).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/mem_stats.h>

#include "llama.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "cases.h"
#include "golden.inc"
#include <moru_led.h>

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define MODEL_BYTES 532517120u

BUILD_ASSERT(L7A_N_CASES == sizeof(l7a_gold) / sizeof(l7a_gold[0]),
	     "cases.h 와 golden.inc 의 문장 수가 다르다 — golden.inc 를 다시 만든다");

static int fails;
static llama_token toks[L7A_MAX_TOK];
static char back[4096];

static void heap(const char *when)
{
	struct sys_memory_stats s;
	if (malloc_runtime_stats_get(&s) == 0) {
		printk("      힙[%s]: 쓴 %zu KiB · 남은 %zu KiB\n", when,
		       s.allocated_bytes / 1024, s.free_bytes / 1024);
	}
}

/* 어긋난 곳 앞뒤를 찍는다 */
static void show_diff(const int *gold, int ng, const llama_token *got, int n)
{
	int k = 0;
	while (k < ng && k < n && gold[k] == got[k]) { k++; }
	printk("        첫 차이 위치 %d (호스트 %d 개 · 보드 %d 개)\n", k, ng, n);
	for (int i = (k > 2 ? k - 2 : 0); i < k + 4 && (i < ng || i < n); i++) {
		printk("          [%3d] 호스트 %7d  보드 %7d%s\n", i,
		       i < ng ? gold[i] : -1, i < n ? (int)got[i] : -1,
		       (i < ng && i < n && gold[i] == got[i]) ? "" : "  ←");
	}
}

int main(void)
{
	printk("=== L7a: 토크나이저 (글자 → 토큰 id → 글자) ===\n");
	printk("  문장 %u 개 · 기준: id 열 전부 일치 + 되돌림 바이트 일치\n",
	       (unsigned)L7A_N_CASES);

	moru_led_set(MORU_LED_BLOB);
	printk("  [1] 블롭 점검 ... ");
	if (*(const uint32_t *)BLOB_ADDR != 0x46554747u) {
		printk("FAIL — 매직이 0x%08x 다\n", *(const uint32_t *)BLOB_ADDR);
		fails++;
		goto done;
	}
	printk("OK — 'GGUF'\n");

	llama_backend_init();
	{
		printk("  [2] 어휘만 적재 (vocab_only) ...\n");
		FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
		if (f == nullptr) {
			printk("      FAIL — fmemopen\n");
			fails++;
			goto done;
		}
		llama_model_params mp = llama_model_default_params();
		mp.n_gpu_layers  = 0;
		mp.vocab_only    = true;
		mp.load_mode     = LLAMA_LOAD_MODE_MMAP;
		mp.check_tensors = false;
		mp.use_extra_bufts = false;
		const int64_t t0 = k_uptime_get();
		llama_model *model = llama_model_load_from_file_ptr(f, mp);
		if (model == nullptr) {
			printk("      FAIL — nullptr\n");
			fails++;
			goto done;
		}
		const llama_vocab *vocab = llama_model_get_vocab(model);
		const int nv = llama_vocab_n_tokens(vocab);
		printk("      OK (%" PRId64 " ms) · 어휘 %d 개 (호스트 %d)%s\n",
		       k_uptime_get() - t0, nv, L7A_HOST_N_VOCAB,
		       nv == L7A_HOST_N_VOCAB ? "" : "  ⚠ 다르다");
		if (nv != L7A_HOST_N_VOCAB) { fails++; }
		heap("어휘 적재 뒤");

		moru_led_set(MORU_LED_INFER);
		printk("  [3] 문장별 대조\n");
		for (unsigned i = 0; i < L7A_N_CASES; i++) {
			const l7a_case &c = l7a_cases[i];
			const int64_t s0 = k_uptime_get();
			const int n = llama_tokenize(vocab, c.text, (int)strlen(c.text), toks,
						     L7A_MAX_TOK, false, c.parse_special);
			const int64_t dt = k_uptime_get() - s0;
			const int *g = &l7a_gold_ids[l7a_gold[i].off];
			const int ng = l7a_gold[i].n;
			bool ok = (n == ng);
			for (int k = 0; ok && k < n; k++) { ok = (g[k] == toks[k]); }

			const int m = (n >= 0) ? llama_detokenize(vocab, toks, n, back, sizeof(back),
								  false, c.parse_special) : -1;
			const bool bok = (m == l7a_gold[i].back_len) &&
					 memcmp(back, l7a_gold[i].back, m) == 0;
			printk("      %-9s %3d 토큰 %s · 되돌림 %s  (%" PRId64 " ms)\n", c.name, n,
			       ok ? "OK  " : "FAIL", bok ? "OK" : "FAIL", dt);
			if (!ok) { fails++; show_diff(g, ng, toks, n); }
			if (!bok) { fails++; printk("        되돌림 길이 보드 %d · 호스트 %d\n",
						    m, l7a_gold[i].back_len); }
		}

		printk("  [4] 시간 — 'long' 을 100 회\n");
		const l7a_case &lc = l7a_cases[L7A_N_CASES - 1];
		const int64_t s0 = k_uptime_get();
		int last = 0;
		for (int r = 0; r < 100; r++) {
			last = llama_tokenize(vocab, lc.text, (int)strlen(lc.text), toks,
					      L7A_MAX_TOK, false, lc.parse_special);
		}
		const int64_t dt = k_uptime_get() - s0;
		printk("      %" PRId64 " ms / 100 회 · 회당 %" PRId64 " µs · %d 토큰 · %u 바이트\n",
		       dt, dt * 10, last, (unsigned)strlen(lc.text));
		heap("끝");
	}

done:
	if (fails == 0) {
		printk("=== L7a PASS (실패 0) ===\n");
		moru_led_set(MORU_LED_READY);
	} else {
		printk("=== L7a FAIL (실패 %d) ===\n", fails);
		moru_led_set(MORU_LED_FAIL);
	}
	/* main 에서 돌아오지 않는다 — 정적 소멸자 문제 (apps/llm_l6b 참고) */
	printk("    (시험 끝 — 여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
