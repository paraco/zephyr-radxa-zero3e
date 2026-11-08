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
#include "gguf.h"
#include "sha256.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "hub.h"
#include "clk1416.h"
#include <rk3566_tsadc.h>
#include "llm_bigheap.h"
#include <moru_led.h>

LOG_MODULE_REGISTER(hub_llm, LOG_LEVEL_INF);

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define BLOB_WIN ((size_t)DT_REG_SIZE(DT_NODELABEL(model_blob)))
static size_t model_bytes;	/* GGUF 머리에서 계산한다 (데이터 시작 + 텐서 끝) */
static char model_hash[65] = "-", model_smp[65] = "-";

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
extern "C" const char *hub_llm_model_hash(void) { return model_hash; }
extern "C" const char *hub_llm_model_smp(void) { return model_smp; }

static int n_past_now(void)
{
	return ctx ? (int)llama_memory_seq_pos_max(llama_get_memory(ctx), 0) + 1 : 0;
}
extern "C" int hub_llm_n_past(void) { return state == ST_BUSY ? -1 : n_past_now(); }

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
	/* 큰 할당은 llm_heap 으로 간다 — 둘을 합쳐야 문맥 크기가 맞게 나온다 */
	return (malloc_runtime_stats_get(&s) == 0 ? s.allocated_bytes / 1024 : 0) +
	       llm_bigheap_used() / 1024;
}

/* llama·ggml 로그는 셸 CDC 를 어지럽히지 않게 경고 이상만 LOG 로 (uart2) */
static void log_cb(enum ggml_log_level lv, const char *text, void *)
{
	if (lv >= GGML_LOG_LEVEL_WARN && strlen(text) > 2) {	/* 진행 점 '.' 은 버린다 */
		LOG_WRN("%s", text);
	}
}

/* ⚠ 한 줄을 **다 만든 뒤 한 번에** 낸다. 다른 스레드에서 shell_fprintf 를
 *   부를 때마다 셸이 프롬프트를 다시 그려, 조각마다 내면 줄 가운데에
 *   'Moru > ' 가 끼었다 (2026-11-08 실기). */
static char line[8192];
static size_t ll;
static void lput(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int k = vsnprintf(line + ll, sizeof line - ll, fmt, ap);
	va_end(ap);
	if (k > 0) { ll += (size_t)k; if (ll >= sizeof line) { ll = sizeof line - 1; } }
}
static void lend(void)
{
	shell_fprintf(cur_sh, SHELL_NORMAL, "%s\n", line);
	ll = 0;
	line[0] = 0;
}
#define OUT(...) do { lput(__VA_ARGS__); lend(); } while (0)

static int temp_mc(void)
{
	int mc = 0;
	return rk3566_tsadc_get_mc(0, &mc) ? -999999 : mc;
}

/* 소프트웨어 보호 문턱 (°C) — hub guard 로 바꾼다 */
static volatile int g_down = 80, g_pause = 85, g_stop = 90;
extern "C" void hub_llm_set_guard(int d, int p, int t) { g_down = d; g_pause = p; g_stop = t; }
extern "C" void hub_llm_get_guard(int *d, int *p, int *t) { *d = g_down; *p = g_pause; *t = g_stop; }

/* 토큰 사이에서 부른다. 0 = 계속, 1 = 멈춤(hot). 온도를 못 읽으면 보호 없이 계속한다
 * (읽기 실패로 생성을 막지 않는다 — done 의 temp_mc 가 -999999 로 드러난다). */
static int thermal_gate(int *max_mc, int *throttle_ms, int *went_down)
{
	int t = temp_mc();
	if (t <= -999999) { return 0; }
	if (t > *max_mc) { *max_mc = t; }
	if (t >= g_stop * 1000) { return 1; }
	if (t >= g_down * 1000 && clk1416_is_1416()) {
		clk1416_back_to_1104("과열 보호");
		*went_down = 1;
	}
	if (t >= g_pause * 1000) {
		while (t >= (g_pause - 5) * 1000) {
			if (atomic_get(&abort_flag)) { return 0; }
			k_msleep(250);
			*throttle_ms += 250;
			t = temp_mc();
			if (t > *max_mc) { *max_mc = t; }
			if (t >= g_stop * 1000) { return 1; }
		}
	}
	return 0;
}

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
			OUT("HUBLLM/1 err id=%u code=prompt_too_long msg=%d_tokens", r.id, -n);
			return;
		}
	}
	if (n == 0) {
		OUT("HUBLLM/1 err id=%u code=empty msg=no_tokens", r.id);
		return;
	}
	const int nv = llama_vocab_n_tokens(vocab);
	for (int i = 0; i < n; i++) {
		if (toks[i] < 0 || toks[i] >= nv) {
			OUT("HUBLLM/1 err id=%u code=bad_token msg=%d", r.id, (int)toks[i]);
			return;
		}
	}
	{
		const int t = temp_mc();
		if (t > -999999 && t >= g_pause * 1000) {	/* 뜨거우면 시작하지 않는다 */
			OUT("HUBLLM/1 err id=%u code=too_hot msg=%d_mC>=%d", r.id, t, g_pause * 1000);
			return;
		}
	}
	llama_memory_t mem = llama_get_memory(ctx);
	if (!r.keep) { llama_memory_clear(mem, true); }
	const int n_past0 = n_past_now();
	if (n_past0 + n + r.max_gen + 1 > n_ctx_now) {
		OUT("HUBLLM/1 err id=%u code=ctx_full msg=%d+%d+%d>%d", r.id, n_past0, n, r.max_gen,
		    n_ctx_now);
		return;
	}

	/* 보드가 실제로 넣는 id 열을 되돌린다 — 호스트가 입력을 맞춰 볼 수 있게 */
	lput("HUBLLM/1 prompt id=%u n=%d ids=", r.id, n);
	for (int i = 0; i < n; i++) { lput(i ? ",%d" : "%d", (int)toks[i]); }
	lend();

	/* 샘플러: temp 0 = greedy. 그 밖 = top_k → top_p → temp → dist(seed) */
	llama_sampler *smp = llama_sampler_chain_init(llama_sampler_chain_default_params());
	const bool pen = (r.pres != 0.0f || r.freq != 0.0f || r.rep != 1.0f) && r.last_n != 0 &&
			 r.rep > 0.0f;
	auto add_pen = [&]() {
		if (pen) {
			llama_sampler_chain_add(smp, llama_sampler_init_penalties(
				nv, r.last_n, r.rep, r.freq, r.pres));
		}
	};
	if (r.temp <= 0.0f) {
		add_pen();
		llama_sampler_chain_add(smp, llama_sampler_init_greedy());
	} else {
		if (r.topk > 0) { llama_sampler_chain_add(smp, llama_sampler_init_top_k(r.topk)); }
		add_pen();	/* top_k 뒤 — 전체 어휘(248,320)에 걸면 느리다 (llama.h 주석) */
		if (r.topp > 0.0f && r.topp < 1.0f) {
			llama_sampler_chain_add(smp, llama_sampler_init_top_p(r.topp, 1));
		}
		llama_sampler_chain_add(smp, llama_sampler_init_temp(r.temp));
		llama_sampler_chain_add(smp, llama_sampler_init_dist(r.seed));
	}

	const int64_t t0 = k_uptime_get();
	if (llama_decode(ctx, llama_batch_get_one(toks, n)) != 0) {
		OUT("HUBLLM/1 err id=%u code=decode msg=prefill", r.id);
		llama_sampler_free(smp);
		return;
	}
	const int64_t t_pp = k_uptime_get() - t0;

	int max_mc = temp_mc(), throttle_ms = 0, went_down = 0;
	int n_gen = 0;
	int64_t t_tg = 0, ttft = 0;
	const char *stop = "max";
	llama_token tok = 0;
	for (;;) {
		tok = llama_sampler_sample(smp, ctx, -1);	/* accept 까지 한다 */
		if (n_gen == 0) { ttft = k_uptime_get() - t0; }

		char piece[64];
		const int pn = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, true);
		uint8_t b64[100];
		size_t bl = 0;
		(void)base64_encode(b64, sizeof b64 - 1, &bl, (const uint8_t *)piece, pn > 0 ? pn : 0);
		b64[bl] = 0;
		OUT("HUBLLM/1 tok id=%u i=%d tid=%d t=%" PRId64 " %s", r.id, n_gen, (int)tok,
		    k_uptime_get() - t0, (const char *)b64);
		n_gen++;

		if (llama_vocab_is_eog(vocab, tok)) { stop = "eos"; break; }
		if (n_gen >= r.max_gen) { stop = "max"; break; }
		if (atomic_get(&abort_flag)) { stop = "abort"; break; }
		if (thermal_gate(&max_mc, &throttle_ms, &went_down)) { stop = "hot"; break; }

		const int64_t s = k_uptime_get();
		if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
			OUT("HUBLLM/1 err id=%u code=decode msg=gen_%d", r.id, n_gen);
			llama_sampler_free(smp);
			return;
		}
		t_tg += k_uptime_get() - s;
	}
	llama_sampler_free(smp);
	/* keep: 마지막 토큰도 KV 에 넣어 둔다 — 다음 프롬프트는 그 **뒤**부터 보낸다 */
	if (r.keep && llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
		OUT("HUBLLM/1 err id=%u code=decode msg=keep_last", r.id);
		return;
	}

	const double pp_tps = t_pp ? (double)n * 1000.0 / (double)t_pp : 0.0;
	const double tg_tps = (n_gen > 1 && t_tg) ? (double)(n_gen - 1) * 1000.0 / (double)t_tg : 0.0;
	OUT("HUBLLM/1 done id=%u n_prompt=%d n_gen=%d ttft_ms=%" PRId64 " pp_tps=%.2f tg_tps=%.2f "
	    "n_ctx=%d ctx_kib=%d heap_kib=%u repack=1 threads=%d n_past=%d keep=%d temp=%.2f pres=%.2f rep=%.2f clk=%d guard_fail=%u temp_mc=%d max_temp_mc=%d throttle_ms=%d clk_down=%d stop=%s",
	    r.id, n, n_gen, ttft, pp_tps, tg_tps, n_ctx_now, ctx_kib,
	    (unsigned)heap_used_kib(), HUB_THREADS, n_past_now(), r.keep ? 1 : 0,
	    (double)r.temp, (double)r.pres, (double)r.rep,
	    clk1416_is_1416() ? 1416 : 1104, clk1416_guard_fail(), temp_mc(), max_mc, throttle_ms,
	    went_down, stop);
}

static bool make_ctx(int want);
static void serve(void);

static void hex32(const uint8_t *d, char *out)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
	out[64] = 0;
}

/* F2: 크기·해시. 머리 = 파일 처음부터 텐서 데이터 시작까지.
 *     표본 = 데이터 구간에서 균등 간격 64 개 × 64 KiB (마지막 덩이는 끝에 붙는다).
 *     호스트 tools/gguf_hash.py 와 같은 규칙. */
static bool model_scan(void)
{
	gguf_init_params gp = { /*no_alloc =*/ true, /*ctx =*/ nullptr };
	gguf_context *g = gguf_init_from_buffer((const void *)BLOB_ADDR, BLOB_WIN, gp);
	if (g == nullptr) { return false; }
	const size_t off = gguf_get_data_offset(g);
	size_t end = 0;
	for (int64_t i = 0; i < gguf_get_n_tensors(g); i++) {
		const size_t e = gguf_get_tensor_offset(g, i) + gguf_get_tensor_size(g, i);
		if (e > end) { end = e; }
	}
	gguf_free(g);
	model_bytes = off + end;
	if (model_bytes > BLOB_WIN) { return false; }

	const int64_t t0 = k_uptime_get();
	uint8_t d[32];
	struct sha256 h;
	sha256_init(&h);
	sha256_update(&h, (const void *)BLOB_ADDR, off);
	sha256_final(&h, d);
	hex32(d, model_hash);
	const size_t CH = 65536, NS = 64;
	sha256_init(&h);
	for (size_t k = 0; k < NS; k++) {
		const size_t pos = (end > CH) ? off + (end - CH) * k / (NS - 1) : off;
		sha256_update(&h, (const uint8_t *)BLOB_ADDR + pos, end > CH ? CH : end);
	}
	sha256_final(&h, d);
	hex32(d, model_smp);
	LOG_INF("모델 %zu B (머리 %zu B) · 해시 %" PRId64 " ms · 머리 %.16s… · 표본 %.16s…",
		model_bytes, off, k_uptime_get() - t0, model_hash, model_smp);
	return true;
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
	if (!model_scan()) {
		LOG_ERR("GGUF 머리를 읽지 못했다");
		state = ST_ERROR;
		moru_led_set(MORU_LED_FAIL);
		return;
	}
	FILE *f = fmemopen((void *)BLOB_ADDR, model_bytes, "rb");
	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers    = 0;
	mp.load_mode       = LLAMA_LOAD_MODE_MMAP;
	mp.check_tensors   = false;
	mp.use_extra_bufts = true;	/* repack (q4_K/q5_K/q6_K_8x4) — 사본은 llm_heap */
	model = f ? llama_model_load_from_file_ptr(f, mp) : nullptr;
	if (model == nullptr) {
		LOG_ERR("모델 적재 실패");
		state = ST_ERROR;
		moru_led_set(MORU_LED_FAIL);
		return;
	}
	vocab = llama_model_get_vocab(model);

	if (!make_ctx(0)) {
		LOG_ERR("문맥 생성 실패");
		state = ST_ERROR;
		moru_led_set(MORU_LED_FAIL);
		return;
	}
	load_ms = (int)(k_uptime_get() - t0);
	LOG_INF("LLM 준비: 적재 %d ms · n_ctx %d · 문맥 %d KiB · 힙 %u KiB · llm_heap %u MiB", load_ms,
		n_ctx_now, ctx_kib, (unsigned)heap_used_kib(), (unsigned)(llm_bigheap_used() >> 20));
	state = ST_READY;
	moru_led_set(MORU_LED_READY);
	serve();
}

/* n=0 이면 ctx_try 순으로. 실패하면 false (ctx 는 nullptr) */
static bool make_ctx(int want)
{
	if (ctx) { llama_free(ctx); ctx = nullptr; }
	const size_t before = heap_used_kib();
	const int one[1] = { want };
	const int *list = want ? one : ctx_try;
	const int cnt = want ? 1 : (int)ARRAY_SIZE(ctx_try);
	for (int k = 0; k < cnt; k++) {
		const int nc = list[k];
		llama_context_params cp = llama_context_default_params();
		cp.n_ctx           = nc;
		cp.n_batch         = nc;
		cp.n_ubatch        = HUB_UBATCH;
		cp.n_threads       = HUB_THREADS;
		cp.n_threads_batch = HUB_THREADS;
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
		ctx = llama_init_from_model(model, cp);
		if (ctx) {
			n_ctx_now = nc;
			ctx_kib = (int)(heap_used_kib() - before);
			return true;
		}
		LOG_WRN("n_ctx %d 문맥 실패", nc);
	}
	n_ctx_now = 0;
	ctx_kib = 0;
	return false;
}

static void serve(void)
{
	for (;;) {
		k_sem_take(&req_sem, K_FOREVER);
		moru_led_set(MORU_LED_INFER);
		if (cur.kind == HUB_RESET) {
			llama_memory_clear(llama_get_memory(ctx), true);
			OUT("HUBLLM/1 reset n_past=0");
		} else if (cur.kind == HUB_CLK) {
			const int tnow = temp_mc();
			if (cur.ctx_n == 1416 && !clk1416_is_1416() && tnow > -999999 &&
			    tnow >= (g_down - 5) * 1000) {
				OUT("HUBLLM/1 err id=0 code=too_hot msg=%d_mC>=%d", tnow, (g_down - 5) * 1000);
			} else if (cur.ctx_n == 1416 && !clk1416_is_1416()) {
				(void)clk1416_raise();	/* 실패하면 1104 그대로 (이유는 printk) */
			} else if (cur.ctx_n == 1104) {
				clk1416_back_to_1104("hub clk 1104");
			}
			OUT("HUBLLM/1 clk mhz=%u state=%d guard_reads=%u guard_fail=%u",
			    clk1416_cpu_mhz(), clk1416_is_1416() ? 1416 : 1104,
			    clk1416_guard_reads(), clk1416_guard_fail());
		} else if (cur.kind == HUB_CTX) {
			const int old = n_ctx_now;
			if (!make_ctx(cur.ctx_n)) {
				OUT("HUBLLM/1 err id=0 code=ctx_alloc msg=%d", cur.ctx_n);
				if (!make_ctx(old) && !make_ctx(0)) {
					state = ST_ERROR;
					moru_led_set(MORU_LED_FAIL);
					return;
				}
			}
			OUT("HUBLLM/1 ctx n_ctx=%d ctx_kib=%d heap_kib=%u", n_ctx_now, ctx_kib,
			    (unsigned)heap_used_kib());
		} else {
			run_one();
		}
		moru_led_set(MORU_LED_READY);
		state = ST_READY;
	}
}

/* 256 KiB — L6c 의 main 스택과 같다 (ggml 그래프 생성이 스택을 많이 쓴다) */
K_THREAD_DEFINE(hub_tid, 262144, hub_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(10), 0, 1500);
