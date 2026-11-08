/*
 * hubllm LLM 스레드 — 모델을 한 번 적재하고, 셸이 넘긴 요청을 하나씩 생성한다.
 *
 * 경로는 L7b 에서 실기 검증한 그대로다: tokenize(L7a 12/12) → 한 배치 prefill
 * (L7b 1등·이어서 8/8) → greedy 로 하나씩. 규약은 hub.h.
 *
 * 우선순위: 이 스레드는 K_PRIO_PREEMPT(10). ggml 워커 3개는 pthread 로
 * 이 우선순위를 물려받는다. 셸(5)·USB(협조형)·LED(타이머 ISR)는 그보다 높아
 * 네 코어가 다 바빠도 abort 를 받고 USB 가 산다.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/logging/log.h>

#include "llama.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "hub.h"
#include <moru_led.h>

LOG_MODULE_REGISTER(hub_llm, LOG_LEVEL_INF);

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define MODEL_BYTES 532517120u	/* L6c 와 같다 — F2 에서 GGUF 머리로 계산하게 바꾼다 */

/* 문맥: 큰 것부터 시도한다. n_ubatch 는 메모리를 크게 좌우한다(L7b: 64 → +108 MiB). */
static const int ctx_try[] = { 1024, 512, 256 };
#define HUB_UBATCH 32
#define HUB_THREADS 4

enum st { ST_LOADING, ST_READY, ST_BUSY, ST_NOBLOB, ST_ERROR };
static volatile int state = ST_LOADING;
static int n_ctx_now, ctx_kib, load_ms;
static atomic_t abort_flag;

static struct hub_req cur;
static const struct shell *cur_sh;
static K_SEM_DEFINE(req_sem, 0, 1);

static llama_model *model;
static llama_context *ctx;
static const llama_vocab *vocab;
static llama_token toks[HUB_MAX_PROMPT_IDS];

extern "C" const char *hub_llm_state(void)
{
	static const char *const n[] = { "loading", "ready", "busy", "noblob", "error" };
	return n[state];
}
extern "C" int hub_llm_n_ctx(void) { return n_ctx_now; }
extern "C" int hub_llm_ctx_kib(void) { return ctx_kib; }
extern "C" int hub_llm_load_ms(void) { return load_ms; }
extern "C" void hub_llm_abort(void) { atomic_set(&abort_flag, 1); }

extern "C" int hub_llm_submit(const struct shell *sh, const struct hub_req *r)
{
	if (state == ST_BUSY) { return -EBUSY; }
	if (state != ST_READY) { return -EAGAIN; }
	cur = *r;
	cur_sh = sh;
	atomic_set(&abort_flag, 0);
	state = ST_BUSY;
	k_sem_give(&req_sem);
	return 0;
}

static size_t heap_used_kib(void)
{
	struct sys_memory_stats s;
	return malloc_runtime_stats_get(&s) == 0 ? s.allocated_bytes / 1024 : 0;
}

/* llama·ggml 로그는 셸 CDC 를 어지럽히지 않게 경고 이상만 LOG 로 (uart2) */
static void log_cb(enum ggml_log_level lv, const char *text, void *)
{
	if (lv >= GGML_LOG_LEVEL_WARN) {
		LOG_WRN("%s", text);
	}
}

#define OUT(...) shell_fprintf(cur_sh, SHELL_NORMAL, __VA_ARGS__)

static void run_one(void)
{
	const hub_req &r = cur;
	int n = 0;

	if (r.use_ids) {
		n = r.n_ids;
		for (int i = 0; i < n; i++) { toks[i] = r.ids[i]; }
	} else {
		n = llama_tokenize(vocab, r.text, r.text_len, toks, HUB_MAX_PROMPT_IDS,
				   false, r.special);
		if (n < 0) {
			OUT("HUBLLM/1 err id=%u code=prompt_too_long msg=%d_tokens\n", r.id, -n);
			return;
		}
	}
	if (n == 0) {
		OUT("HUBLLM/1 err id=%u code=empty msg=no_tokens\n", r.id);
		return;
	}
	const int nv = llama_vocab_n_tokens(vocab);
	for (int i = 0; i < n; i++) {
		if (toks[i] < 0 || toks[i] >= nv) {
			OUT("HUBLLM/1 err id=%u code=bad_token msg=%d\n", r.id, (int)toks[i]);
			return;
		}
	}
	if (n + r.max_gen > n_ctx_now) {
		OUT("HUBLLM/1 err id=%u code=ctx_full msg=%d+%d>%d\n", r.id, n, r.max_gen, n_ctx_now);
		return;
	}

	/* 보드가 실제로 넣는 id 열을 되돌린다 — 호스트가 입력을 맞춰 볼 수 있게 */
	OUT("HUBLLM/1 prompt id=%u n=%d ids=", r.id, n);
	for (int i = 0; i < n; i++) { OUT(i ? ",%d" : "%d", (int)toks[i]); }
	OUT("\n");

	llama_memory_clear(llama_get_memory(ctx), true);	/* 첫 판: 단발 */
	const int64_t t0 = k_uptime_get();
	if (llama_decode(ctx, llama_batch_get_one(toks, n)) != 0) {
		OUT("HUBLLM/1 err id=%u code=decode msg=prefill\n", r.id);
		return;
	}
	const int64_t t_pp = k_uptime_get() - t0;

	int n_gen = 0;
	int64_t t_tg = 0, ttft = 0;
	const char *stop = "max";
	llama_token tok = 0;
	for (;;) {
		const float *lg = llama_get_logits_ith(ctx, -1);
		int best = 0;
		for (int i = 1; i < nv; i++) { if (lg[i] > lg[best]) { best = i; } }
		tok = best;
		if (n_gen == 0) { ttft = k_uptime_get() - t0; }

		char piece[64];
		const int pn = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, true);
		uint8_t b64[100];
		size_t bl = 0;
		(void)base64_encode(b64, sizeof b64 - 1, &bl, (const uint8_t *)piece, pn > 0 ? pn : 0);
		b64[bl] = 0;
		OUT("HUBLLM/1 tok id=%u i=%d tid=%d t=%" PRId64 " %s\n", r.id, n_gen, (int)tok,
		    k_uptime_get() - t0, (const char *)b64);
		n_gen++;

		if (llama_vocab_is_eog(vocab, tok)) { stop = "eos"; break; }
		if (n_gen >= r.max_gen) { stop = "max"; break; }
		if (atomic_get(&abort_flag)) { stop = "abort"; break; }

		const int64_t s = k_uptime_get();
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
			OUT("HUBLLM/1 err id=%u code=decode msg=gen_%d\n", r.id, n_gen);
			return;
		}
		t_tg += k_uptime_get() - s;
	}

	const double pp_tps = t_pp ? (double)n * 1000.0 / (double)t_pp : 0.0;
	const double tg_tps = (n_gen > 1 && t_tg) ? (double)(n_gen - 1) * 1000.0 / (double)t_tg : 0.0;
	OUT("HUBLLM/1 done id=%u n_prompt=%d n_gen=%d ttft_ms=%" PRId64 " pp_tps=%.2f tg_tps=%.2f "
	    "n_ctx=%d ctx_kib=%d heap_kib=%u threads=%d stop=%s\n",
	    r.id, n, n_gen, ttft, pp_tps, tg_tps, n_ctx_now, ctx_kib,
	    (unsigned)heap_used_kib(), HUB_THREADS, stop);
}

static void hub_thread(void *, void *, void *)
{
	llama_log_set(log_cb, nullptr);

	moru_led_set(MORU_LED_BLOB);
	if (*(const uint32_t *)BLOB_ADDR != 0x46554747u) {
		LOG_WRN("블롭이 GGUF 가 아니다 (0x%08x) — LLM 없이 셸만", *(const uint32_t *)BLOB_ADDR);
		state = ST_NOBLOB;
		moru_led_set(MORU_LED_READY);
		return;
	}
	llama_backend_init();
	const int64_t t0 = k_uptime_get();
	FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers    = 0;
	mp.load_mode       = LLAMA_LOAD_MODE_MMAP;
	mp.check_tensors   = false;
	mp.use_extra_bufts = false;
	model = f ? llama_model_load_from_file_ptr(f, mp) : nullptr;
	if (model == nullptr) {
		LOG_ERR("모델 적재 실패");
		state = ST_ERROR;
		moru_led_set(MORU_LED_FAIL);
		return;
	}
	vocab = llama_model_get_vocab(model);

	const size_t before = heap_used_kib();
	for (int nc : ctx_try) {
		llama_context_params cp = llama_context_default_params();
		cp.n_ctx           = nc;
		cp.n_batch         = nc;
		cp.n_ubatch        = HUB_UBATCH;
		cp.n_threads       = HUB_THREADS;
		cp.n_threads_batch = HUB_THREADS;
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
		ctx = llama_init_from_model(model, cp);
		if (ctx) { n_ctx_now = nc; break; }
		LOG_WRN("n_ctx %d 문맥 실패 — 줄인다", nc);
	}
	if (ctx == nullptr) {
		LOG_ERR("문맥 생성 실패");
		state = ST_ERROR;
		moru_led_set(MORU_LED_FAIL);
		return;
	}
	ctx_kib = (int)(heap_used_kib() - before);
	load_ms = (int)(k_uptime_get() - t0);
	LOG_INF("LLM 준비: 적재 %d ms · n_ctx %d · 문맥 %d KiB · 힙 %u KiB", load_ms, n_ctx_now,
		ctx_kib, (unsigned)heap_used_kib());
	state = ST_READY;
	moru_led_set(MORU_LED_READY);

	for (;;) {
		k_sem_take(&req_sem, K_FOREVER);
		moru_led_set(MORU_LED_INFER);
		run_one();
		moru_led_set(MORU_LED_READY);
		state = ST_READY;
	}
}

/* 256 KiB — L6c 의 main 스택과 같다 (ggml 그래프 생성이 스택을 많이 쓴다) */
K_THREAD_DEFINE(hub_tid, 262144, hub_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(10), 0, 1500);
