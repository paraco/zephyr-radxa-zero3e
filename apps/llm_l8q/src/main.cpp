/*
 * L8q — L8p 를 **repack 켜고** 다시 (GGML_CPU_REPACK · q4_K/q5_K/q6_K_8x4 · 가중치는 llm_heap).
 *
 * 더 하는 것: [0] 적재 시간·llm_heap 사용량 [C] L7b 'chat' 정답 대조(1등 + 이어서 8) — repack 은
 * 누산 순서를 바꾸므로 답이 크게 달라지지 않는지 [G] 생성 속도(이어서 32 토큰).
 *
 * (아래는 L8p 설명)
 * L8p — 배치 prefill 이 왜 느린가: 연산(op)별 시간.
 *
 * L7b 실측: 55 토큰을 한 배치로 7,378 ms · 하나씩 14,999 ms — 배치가 2 배만 빠르다.
 * 가중치를 한 번 읽어 여러 토큰에 쓰면 보통 몇 배 이상 빨라야 한다.
 *
 * 방법: llama_context_params.cb_eval 로 그래프의 **노드마다** 끝난 시각을 받아
 * (앞 노드와의 차이 = 그 노드의 계산 시간) "연산 · src0 형" 별로 모은다.
 *   A. 'long' 55 토큰을 한 배치(n_ubatch 64)로
 *   B. 같은 55 토큰을 하나씩(n_ubatch 1 문맥)
 * 두 경우의 **토큰당** 비용을 연산별로 나란히 찍는다. 배치로 넣어도 토큰당
 * 비용이 줄지 않는 연산이 원인이다.
 *
 * ⚠ 콜백이 노드마다 동기화를 걸어 전체 시간은 늘어난다 — 그래서 각 경우를 콜백
 *   없이도 한 번 재서 "덧붙은 시간"을 함께 찍는다. 비율 비교가 목적이다.
 * ⚠ 4 스레드 · 1104 MHz · 과열 보호 없음(측정 앱) — 실행은 1 분 남짓.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#include "ggml.h"
#include "llama.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "prompts.h"
#include "l7b_golden.inc"
#include "llm_bigheap.h"
#include <moru_led.h>
#include <rk3566_tsadc.h>

#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(model_blob)))
#define MODEL_BYTES 532517120u

#define MAXK 48
struct agg { char key[40]; uint64_t cyc; uint32_t n; };
static agg acc[MAXK];
static int nacc;
static uint64_t last_cyc;
static bool rec;

static agg *slot(const char *k)
{
	for (int i = 0; i < nacc; i++) { if (!strcmp(acc[i].key, k)) { return &acc[i]; } }
	if (nacc == MAXK) { return &acc[MAXK - 1]; }
	memcpy(acc[nacc].key, k, strnlen(k, sizeof acc[nacc].key - 1));
	return &acc[nacc++];
}

static bool cb(struct ggml_tensor *t, bool ask, void *)
{
	if (ask) { return rec; }		/* 모든 노드를 본다 */
	const uint64_t now = k_cycle_get_64();
	char k[40];
	if (t->op == GGML_OP_MUL_MAT || t->op == GGML_OP_MUL_MAT_ID) {
		snprintf(k, sizeof k, "%s·%s", ggml_op_desc(t), t->src[0] ? ggml_type_name(t->src[0]->type) : "?");
	} else {
		snprintf(k, sizeof k, "%s", ggml_op_desc(t));
	}
	agg *a = slot(k);
	a->cyc += now - last_cyc;
	a->n++;
	last_cyc = now;
	return true;
}

static llama_context *mk(llama_model *m, int ub, bool with_cb)
{
	llama_context_params cp = llama_context_default_params();
	cp.n_ctx = 256; cp.n_batch = 256; cp.n_ubatch = ub;
	cp.n_threads = 4; cp.n_threads_batch = 4;
	cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
	if (with_cb) { cp.cb_eval = cb; cp.cb_eval_user_data = nullptr; }
	return llama_init_from_model(m, cp);
}

static llama_token toks[64];

static int64_t run(llama_context *c, int n, bool batch)
{
	llama_memory_clear(llama_get_memory(c), true);
	const int64_t t0 = k_uptime_get();
	if (batch) {
		(void)llama_decode(c, llama_batch_get_one(toks, n));
	} else {
		for (int i = 0; i < n; i++) { (void)llama_decode(c, llama_batch_get_one(&toks[i], 1)); }
	}
	return k_uptime_get() - t0;
}

static agg resA[MAXK], resB[MAXK];
static int nA, nB;

int main(void)
{
	printk("=== L8q: repack ON — 배치 prefill 연산별 시간 · 정답 대조 · 생성 속도 ===\n");
	moru_led_set(MORU_LED_BLOB);
	FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
	llama_backend_init();
	llama_model_params mp = llama_model_default_params();
	mp.n_gpu_layers = 0; mp.load_mode = LLAMA_LOAD_MODE_MMAP;
	mp.check_tensors = false; mp.use_extra_bufts = true;	/* repack 버퍼 */
	const int64_t tl0 = k_uptime_get();
	llama_model *m = f ? llama_model_load_from_file_ptr(f, mp) : nullptr;
	if (!m) { printk("모델 적재 실패\n"); k_sleep(K_FOREVER); }
	printk("  [0] 적재 %" PRId64 " ms · llm_heap 사용 %zu MiB (최대 %zu / %zu MiB)\n",
	       k_uptime_get() - tl0, llm_bigheap_used() >> 20, llm_bigheap_peak() >> 20,
	       llm_bigheap_size() >> 20);
	const llama_vocab *v = llama_model_get_vocab(m);
	const l7b_prompt &P = l7b_prompts[1];	/* 'long' */
	const int n = llama_tokenize(v, P.text, (int)strlen(P.text), toks, 64, false, P.parse_special);
	printk("  프롬프트 '%s' %d 토큰\n", P.name, n);
	moru_led_set(MORU_LED_INFER);

	/* 콜백 없이 — 덧붙은 시간을 가늠하는 기준 */
	llama_context *ca = mk(m, 64, false), *cb1 = mk(m, 1, false);
	const int64_t a0 = run(ca, n, true), b0 = run(cb1, n, false);
	llama_free(ca); llama_free(cb1);
	printk("  콜백 없음: 배치 %" PRId64 " ms · 하나씩 %" PRId64 " ms (%.2f 배)\n", a0, b0,
	       (double)b0 / (double)a0);

	/* A: 배치 */
	ca = mk(m, 64, true);
	nacc = 0; rec = true; last_cyc = k_cycle_get_64();
	llama_memory_clear(llama_get_memory(ca), true);
	last_cyc = k_cycle_get_64();
	const int64_t a1 = run(ca, n, true);
	rec = false;
	memcpy(resA, acc, sizeof acc); nA = nacc;
	llama_free(ca);
	/* B: 하나씩 */
	cb1 = mk(m, 1, true);
	nacc = 0; memset(acc, 0, sizeof acc); rec = true;
	llama_memory_clear(llama_get_memory(cb1), true);
	last_cyc = k_cycle_get_64();
	const int64_t b1 = run(cb1, n, false);
	rec = false;
	memcpy(resB, acc, sizeof acc); nB = nacc;
	llama_free(cb1);
	printk("  콜백 있음: 배치 %" PRId64 " ms · 하나씩 %" PRId64 " ms\n", a1, b1);

	const double hz = (double)sys_clock_hw_cycles_per_sec();
	uint64_t ta = 0, tb = 0;
	for (int i = 0; i < nA; i++) { ta += resA[i].cyc; }
	for (int i = 0; i < nB; i++) { tb += resB[i].cyc; }
	printk("\n  %-26s %10s %10s %8s %6s %6s\n", "연산·형", "배치ms/tok", "하나ms/tok", "배치/하나", "배치%", "노드A");
	for (int i = 0; i < nA; i++) {
		uint64_t cb_ = 0;
		for (int j = 0; j < nB; j++) { if (!strcmp(resB[j].key, resA[i].key)) { cb_ = resB[j].cyc; } }
		const double ma = resA[i].cyc / hz * 1000.0 / n, mb = cb_ / hz * 1000.0 / n;
		if (ma < 0.05 && mb < 0.05) { continue; }
		printk("  %-26s %10.2f %10.2f %8.2f %5.1f%% %6u\n", resA[i].key, ma, mb,
		       mb > 0 ? ma / mb : 0.0, 100.0 * (double)resA[i].cyc / (double)ta, resA[i].n);
	}
	printk("  %-26s %10.2f %10.2f\n", "합계", ta / hz * 1000.0 / n, tb / hz * 1000.0 / n);
	/* [C] L7b 'chat' 정답 대조 + [G] 생성 속도 */
	{
		llama_context *cc = mk(m, 64, false);
		const l7b_prompt &C = l7b_prompts[0];
		const int nc = llama_tokenize(v, C.text, (int)strlen(C.text), toks, 64, false, C.parse_special);
		llama_memory_clear(llama_get_memory(cc), true);
		(void)llama_decode(cc, llama_batch_get_one(toks, nc));
		const int nv = llama_vocab_n_tokens(v);
		const float *lg = llama_get_logits_ith(cc, -1);
		int best = 0;
		for (int i = 1; i < nv; i++) { if (lg[i] > lg[best]) { best = i; } }
		printk("\n  [C] 'chat' %d 토큰 · 1등 %d (%.4f) · 호스트 %d (%.4f) %s\n", nc, best,
		       (double)lg[best], l7b_gold[0].top_id[0], (double)l7b_gold[0].top_logit[0],
		       best == l7b_gold[0].top_id[0] ? "OK" : "FAIL");
		int same = 0;
		llama_token cur = best;
		for (int s = 0; s < L7B_N_CONT; s++) {
			if (cur != l7b_gold[0].cont[s]) { break; }
			same++;
			if (s == L7B_N_CONT - 1) { break; }
			(void)llama_decode(cc, llama_batch_get_one(&cur, 1));
			const float *l2 = llama_get_logits_ith(cc, -1);
			int b2 = 0;
			for (int i = 1; i < nv; i++) { if (l2[i] > l2[b2]) { b2 = i; } }
			cur = b2;
		}
		printk("      이어서 %d/%d 호스트와 같다 (호스트는 repack OFF)\n", same, L7B_N_CONT);
		/* [G] 32 토큰 생성 */
		const int64_t g0 = k_uptime_get();
		for (int s = 0; s < 32; s++) {
			(void)llama_decode(cc, llama_batch_get_one(&cur, 1));
			const float *l3 = llama_get_logits_ith(cc, -1);
			int b3 = 0;
			for (int i = 1; i < nv; i++) { if (l3[i] > l3[b3]) { b3 = i; } }
			cur = b3;
		}
		const int64_t gt = k_uptime_get() - g0;
		printk("  [G] 생성 32 토큰 %" PRId64 " ms · %.1f ms/토큰 · %.2f 토큰/초 (repack OFF 274 ms · 3.65)\n",
		       gt, (double)gt / 32, 32000.0 / (double)gt);
		llama_free(cc);
	}
	printk("  llm_heap 최대 %zu MiB\n", llm_bigheap_peak() >> 20);
	int mc = 0;
	(void)rk3566_tsadc_get_mc(0, &mc);
	printk("  CPU 온도 %d.%d °C\n", mc / 1000, (mc % 1000) / 100);
	printk("=== L8q 측정 끝 ===\n");
	moru_led_set(MORU_LED_READY);
	k_sleep(K_FOREVER);
	return 0;
}
