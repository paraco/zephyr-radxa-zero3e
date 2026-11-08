/*
 * L7b — 프롬프트를 **한 배치로** 넣는다 (prefill).
 *
 * L6c 는 토큰을 하나씩 넣었다(n_ubatch = 1). 실제 대화는 프롬프트 수십 토큰을
 * 한 번에 넣는다 — 그러면 행렬×벡터가 아니라 행렬×행렬 경로를 지나고,
 * 어텐션 마스크·DeltaNet 의 덩이 처리(chunk)가 처음으로 쓰인다.
 *
 *   [1] 블롭 · [2] 모델 적재 · [3] 문맥 (n_ctx 256, n_batch = n_ubatch = 64)
 *   [4] 프롬프트마다
 *       a. 보드에서 tokenize → 호스트 id 열과 일치 (L7a 가 이미 가렸다 — 재확인)
 *       b. **한 배치 prefill** → 마지막 위치 1등이 호스트와 같은가
 *          + 상위 5 의 순서·로짓 차이 (참고)
 *       c. 이어서 greedy 8 개 → 호스트 열과 **전부 일치**
 *       d. 같은 프롬프트를 **하나씩** 넣어 prefill → 1등이 b 와 같은가
 *          (호스트 없이 보드 안에서 두 경로를 맞춘다)
 *   [5] 시간 — 배치 prefill 의 토큰/초, 하나씩 넣을 때와 비교
 *
 * 정답값: tools/l7b_host_prefill.cpp → src/golden.inc (호스트 1스레드).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/mem_stats.h>

#include "ggml.h"
#include "llama.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "prompts.h"
#include "golden.inc"
#include <moru_led.h>

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define MODEL_BYTES 532517120u

BUILD_ASSERT(L7B_N_PROMPTS == sizeof(l7b_gold) / sizeof(l7b_gold[0]),
	     "prompts.h 와 golden.inc 의 수가 다르다 — golden.inc 를 다시 만든다");

static int fails;
static llama_token toks[L7B_MAX_TOK];

static void heap(const char *when)
{
	struct sys_memory_stats s;
	if (malloc_runtime_stats_get(&s) == 0) {
		printk("      힙[%s]: 쓴 %zu KiB · 최대 %zu KiB · 남은 %zu KiB\n", when,
		       s.allocated_bytes / 1024, s.max_allocated_bytes / 1024, s.free_bytes / 1024);
	}
}

static void top5(const float *lg, int nv, int *id)
{
	for (int k = 0; k < L7B_TOPK; k++) { id[k] = -1; }
	for (int i = 0; i < nv; i++) {
		for (int k = 0; k < L7B_TOPK; k++) {
			if (id[k] < 0 || lg[i] > lg[id[k]]) {
				for (int j = L7B_TOPK - 1; j > k; j--) { id[j] = id[j - 1]; }
				id[k] = i;
				break;
			}
		}
	}
}

static int argmax(const float *lg, int nv)
{
	int b = 0;
	for (int i = 1; i < nv; i++) { if (lg[i] > lg[b]) { b = i; } }
	return b;
}

int main(void)
{
	printk("=== L7b: 프롬프트 한 배치 prefill (n_ubatch %d) ===\n", L7B_MAX_TOK);
	printk("  프롬프트 %u 개 · 기준: 마지막 1등 일치 + 이어서 greedy %d 개 일치\n",
	       (unsigned)L7B_N_PROMPTS, L7B_N_CONT);

	moru_led_set(MORU_LED_BLOB);
	printk("  [1] 블롭 점검 ... ");
	if (*(const uint32_t *)BLOB_ADDR != 0x46554747u) {
		printk("FAIL — 매직 0x%08x\n", *(const uint32_t *)BLOB_ADDR);
		fails++;
		goto done;
	}
	printk("OK\n");

	llama_backend_init();
	{
		printk("  [2] 모델 적재 ... ");
		FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
		if (f == nullptr) { printk("FAIL — fmemopen\n"); fails++; goto done; }
		llama_model_params mp = llama_model_default_params();
		mp.n_gpu_layers    = 0;
		mp.load_mode       = LLAMA_LOAD_MODE_MMAP;
		mp.check_tensors   = false;
		mp.use_extra_bufts = false;
		int64_t t0 = k_uptime_get();
		llama_model *model = llama_model_load_from_file_ptr(f, mp);
		if (model == nullptr) { printk("FAIL — nullptr\n"); fails++; goto done; }
		printk("OK (%" PRId64 " ms)\n", k_uptime_get() - t0);
		heap("모델 적재 뒤");

		printk("  [3] 문맥 (n_ctx %d · n_batch = n_ubatch = %d · 스레드 4) ... ",
		       L7B_N_CTX, L7B_MAX_TOK);
		llama_context_params cp = llama_context_default_params();
		cp.n_ctx           = L7B_N_CTX;
		cp.n_batch         = L7B_MAX_TOK;
		cp.n_ubatch        = L7B_MAX_TOK;
		cp.n_threads       = 4;
		cp.n_threads_batch = 4;
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
		llama_context *ctx = llama_init_from_model(model, cp);
		if (ctx == nullptr) { printk("FAIL — nullptr\n"); fails++; goto done; }
		printk("OK\n");
		heap("문맥 생성 뒤");

		const llama_vocab *vocab = llama_model_get_vocab(model);
		const int nv = llama_vocab_n_tokens(vocab);
		llama_memory_t mem = llama_get_memory(ctx);

		moru_led_set(MORU_LED_INFER);
		for (unsigned p = 0; p < L7B_N_PROMPTS; p++) {
			const l7b_prompt &P = l7b_prompts[p];
			printk("  [4] '%s'\n", P.name);

			/* a. tokenize */
			const int n = llama_tokenize(vocab, P.text, (int)strlen(P.text), toks,
						     L7B_MAX_TOK, false, P.parse_special);
			bool tok_ok = (n == l7b_gold[p].n_prompt);
			for (int i = 0; tok_ok && i < n; i++) { tok_ok = (toks[i] == l7b_gold[p].prompt[i]); }
			printk("      a. 토큰 %d 개 %s\n", n, tok_ok ? "OK (호스트와 같다)" : "FAIL");
			if (!tok_ok) { fails++; continue; }

			/* b. 한 배치 prefill */
			llama_memory_clear(mem, true);
			int64_t s0 = k_uptime_get();
			int rc = llama_decode(ctx, llama_batch_get_one(toks, n));
			const int64_t t_batch = k_uptime_get() - s0;
			if (rc != 0) { printk("      b. FAIL — llama_decode %d\n", rc); fails++; continue; }
			const float *lg = llama_get_logits_ith(ctx, -1);
			int id[L7B_TOPK];
			top5(lg, nv, id);
			const bool top_ok = (id[0] == l7b_gold[p].top_id[0]);
			printk("      b. 배치 prefill %" PRId64 " ms (%d 토큰 · %" PRId64 " 토큰/초) · 1등 %d %s\n",
			       t_batch, n, t_batch ? (int64_t)n * 1000 / t_batch : 0, id[0],
			       top_ok ? "OK" : "FAIL");
			for (int k = 0; k < L7B_TOPK; k++) {
				printk("           %d등 보드 %6d %9.4f · 호스트 %6d %9.4f%s\n", k + 1,
				       id[k], (double)lg[id[k]], l7b_gold[p].top_id[k],
				       (double)l7b_gold[p].top_logit[k],
				       id[k] == l7b_gold[p].top_id[k] ? "" : "  (순서 다름)");
			}
			if (!top_ok) { fails++; continue; }

			/* c. 이어서 greedy */
			int matched = 0;
			llama_token cur = id[0];
			char text[256] = {0};
			size_t tl = 0;
			int64_t t_gen = 0;
			for (int s = 0; s < L7B_N_CONT; s++) {
				if (cur != l7b_gold[p].cont[s]) {
					printk("      c. %d 번째 FAIL — 보드 %d · 호스트 %d\n", s + 1,
					       cur, l7b_gold[p].cont[s]);
					break;
				}
				matched++;
				char pc[32];
				const int pn = llama_token_to_piece(vocab, cur, pc, sizeof pc, 0, true);
				if (pn > 0 && tl + pn < sizeof text - 1) { memcpy(text + tl, pc, pn); tl += pn; }
				if (s == L7B_N_CONT - 1) { break; }
				s0 = k_uptime_get();
				rc = llama_decode(ctx, llama_batch_get_one(&cur, 1));
				t_gen += k_uptime_get() - s0;
				if (rc != 0) { printk("      c. FAIL — decode %d\n", rc); break; }
				cur = argmax(llama_get_logits_ith(ctx, -1), nv);
			}
			printk("      c. 이어서 %d/%d 일치 · 토큰당 %" PRId64 " ms · \"%s\"\n", matched,
			       L7B_N_CONT, t_gen / (L7B_N_CONT - 1), text);
			if (matched != L7B_N_CONT) { fails++; }

			/* d. 하나씩 prefill — 보드 안에서 두 경로 대조 */
			llama_memory_clear(mem, true);
			s0 = k_uptime_get();
			for (int i = 0; i < n && rc == 0; i++) {
				rc = llama_decode(ctx, llama_batch_get_one(&toks[i], 1));
			}
			const int64_t t_seq = k_uptime_get() - s0;
			if (rc != 0) { printk("      d. FAIL — decode %d\n", rc); fails++; continue; }
			int id2[L7B_TOPK];
			top5(llama_get_logits_ith(ctx, -1), nv, id2);
			printk("      d. 하나씩 prefill %" PRId64 " ms · 1등 %d %s · 배치가 %" PRId64 ".%" PRId64 "배 빠르다\n",
			       t_seq, id2[0], id2[0] == id[0] ? "OK (배치와 같다)" : "FAIL (배치와 다르다)",
			       t_batch ? t_seq / t_batch : 0, t_batch ? (t_seq * 10 / t_batch) % 10 : 0);
			if (id2[0] != id[0]) { fails++; }
		}
		heap("끝");
	}

done:
	if (fails == 0) {
		printk("=== L7b PASS (실패 0) ===\n");
		moru_led_set(MORU_LED_READY);
	} else {
		printk("=== L7b FAIL (실패 %d) ===\n", fails);
		moru_led_set(MORU_LED_FAIL);
	}
	printk("    (시험 끝 — 여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
