/*
 * L6c — 여러 토큰. KV 캐시와 DeltaNet 재귀 상태가 **토큰 사이로** 넘어가나.
 *
 * L6 의 셋째 칸. L6b 는 토큰 **하나**를 넣었으므로 상태가 넘어가는지는
 * 가리지 못했다. 상태가 안 넘어가면 **둘째 토큰부터** 틀린다.
 *
 * ── 왜 "토큰 열" 이 기준인가 ────────────────────────────────────────────
 *
 * 로짓 값은 1 ulp 교란에 4.3% 흔들린다(docs/l6b-sensitivity.md). 그런데
 * **탐욕 argmax 열은 16개까지 그 교란에 견딘다** — 호스트의 두 빌드
 * (`-ffp-contract=off` 와 `=fast`)가 16개 토큰을 똑같이 낸다:
 *
 *     , I am a student of the university of the north of the north of the
 *
 * 그래서 토큰 id 열은 **전부 일치를 요구**할 수 있다. 측정으로 정한 기준이다.
 *
 * ── 무엇을 가리나 ───────────────────────────────────────────────────────
 *
 *   [1] 블롭이 실제 모델인가
 *   [2] 모델 적재
 *   [3] 문맥 생성
 *   [4] 토큰 12개를 **이어서** 생성하고 매 단계 호스트와 대조
 *       · 토큰 id 가 어긋나면 그 자리에서 **멈춘다** — 그 뒤는 입력이
 *         달라져 비교가 무의미하다
 *       · 단계마다 시간을 잰다 (문맥이 길어지며 느려지는가)
 *   [5] 생성된 글 전체
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/mem_stats.h>

#include "ggml.h"
#include "llama.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "host_gen.inc"

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(BLOB_NODE))
#define MODEL_BYTES 532517120u

static int fails;

static void heap(const char *when)
{
	struct sys_memory_stats s;
	if (malloc_runtime_stats_get(&s) != 0) {
		return;
	}
	printk("      힙[%s]: 쓴 %zu KiB · 최대 %zu KiB · 남은 %zu KiB\n",
	       when, s.allocated_bytes / 1024, s.max_allocated_bytes / 1024,
	       s.free_bytes / 1024);
}

#ifdef LLM_CLK1416
#include "clk1416.h"
#endif

#include <moru_led.h>

int main(void)
{
	printk("=== L6c: 여러 토큰 (KV 캐시 · DeltaNet 상태) ===\n");
#ifdef LLM_CLK1416
	/* L6d: 1416 MHz 로 올린다. 실패하면 1104 그대로 시험한다(이유를 찍는다). */
	printk("  [L6d] 1416 MHz 판: %s\n", clk1416_raise() == 0 ? "올렸다" : "**못 올렸다 — 1104 로 잰다**");
#endif
	printk("  창 0x%08" PRIxPTR " + %u MiB · 첫 토큰 %d · %d개 생성\n",
	       (uintptr_t)BLOB_ADDR, (unsigned)(BLOB_SIZE / (1024 * 1024)),
	       L6C_FIRST_TOKEN, L6C_N_GEN);
	printk("  기준: **토큰 id 열 전부 일치** (1 ulp 교란에도 16개까지 견딘다)\n");

	moru_led_set(MORU_LED_BLOB);	/* 블롭 점검·모델 적재·문맥 생성 */
	printk("  [1] 블롭 점검 ... ");
	{
		const uint8_t *p = (const uint8_t *)BLOB_ADDR;
		if (*(const uint32_t *)p != 0x46554747u) {
			printk("FAIL — 매직이 0x%08x 다\n", *(const uint32_t *)p);
			fails++;
			goto done;
		}
		const uint64_t nt = *(const uint64_t *)(p + 8);
		printk("OK — 'GGUF' v3, 텐서 %" PRIu64 "개\n", nt);
		if (nt < 16) {
			printk("      FAIL — 시험 GGUF 가 올라왔다\n");
			fails++;
			goto done;
		}
	}

	llama_backend_init();

	{
		printk("  [2] 모델 적재 ... ");
		FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
		if (f == nullptr) {
			printk("FAIL — fmemopen\n");
			fails++;
			goto done;
		}
		llama_model_params mp = llama_model_default_params();
		mp.n_gpu_layers    = 0;
		mp.load_mode       = LLAMA_LOAD_MODE_MMAP;
		mp.check_tensors   = false;
		mp.use_extra_bufts = false;

		int64_t t0 = k_uptime_get();
		llama_model *model = llama_model_load_from_file_ptr(f, mp);
		if (model == nullptr) {
			printk("FAIL — nullptr\n");
			fails++;
			goto done;
		}
		printk("OK (%" PRId64 " ms)\n", k_uptime_get() - t0);
		heap("모델 적재 뒤");

		/* ⚠ 보드와 호스트가 **같은 설정**이어야 한다 (tools/l6c_host_gen.cpp) */
		printk("  [3] 문맥 생성 (n_ctx=256) ... ");
		llama_context_params cp = llama_context_default_params();
		cp.n_ctx           = 256;
		cp.n_batch         = 1;
		cp.n_ubatch        = 1;
		/* ── n_threads=4 (2026-11-07 밤) ────────────────────────────
		 * L8e 가 실기에서 네 코어를 올렸다. 여기서 ggml 이 실제로
		 * 그것을 쓰게 한다. ggml 은 워커 3개를 pthread 로 만든다.
		 *
		 * ⚠ **호스트 골든은 n_threads=1 로 뽑은 그대로 둔다.**
		 *   그러면 이 시험이 묻는 것이 분명해진다 —
		 *   "4스레드 보드가 1스레드 호스트와 **여전히** 같은 토큰을
		 *    내는가". 행렬곱은 보통 스레드가 행/덩이를 나눠 가져
		 *    누적 순서가 바뀌지 않으므로 비트 일치가 기대되지만,
		 *    **기대는 증거가 아니다.** 12/12 일치가 깨지면 그 자체가
		 *    정보이고, 어느 층에서 갈라지는지 L6b 로 내려가 본다.
		 */
		cp.n_threads       = 4;
		cp.n_threads_batch = 4;
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
		llama_context *ctx = llama_init_from_model(model, cp);
		if (ctx == nullptr) {
			printk("FAIL — nullptr\n");
			fails++;
			goto done;
		}
		printk("OK\n");
		heap("문맥 생성 뒤");

		const llama_vocab *vocab = llama_model_get_vocab(model);
		const int n_vocab = llama_vocab_n_tokens(vocab);

		moru_led_set(MORU_LED_INFER);
		printk("  [4] 토큰 %d개 이어서 생성 (실기는 토큰당 20초대)\n", L6C_N_GEN);
		llama_token t = L6C_FIRST_TOKEN;
		int matched = 0;
		char text[256] = {0};
		size_t tlen = 0;

		for (int step = 0; step < L6C_N_GEN; step++) {
			llama_batch b = llama_batch_get_one(&t, 1);
			int64_t s0 = k_uptime_get();
			const int rc = llama_decode(ctx, b);
			const int64_t dt = k_uptime_get() - s0;
			if (rc != 0) {
				printk("      %2d단계 FAIL — llama_decode 가 %d\n",
				       step + 1, rc);
				fails++;
				break;
			}

			const float *lg = llama_get_logits_ith(ctx, 0);
			if (lg == nullptr) {
				printk("      %2d단계 FAIL — 로짓을 못 읽었다\n", step + 1);
				fails++;
				break;
			}
			int best = 0;
			for (int i = 1; i < n_vocab; i++) {
				if (lg[i] > lg[best]) {
					best = i;
				}
			}

			/* 조각을 모아 글을 만든다 — 숫자만 보면 맞는지 감이 안 온다 */
			char piece[32] = {0};
			const int pn = llama_token_to_piece(vocab, best, piece,
							    sizeof piece - 1, 0, true);
			if (pn > 0 && tlen + (size_t)pn < sizeof text - 1) {
				memcpy(text + tlen, piece, (size_t)pn);
				tlen += (size_t)pn;
			}

			const int want = l6c_host_gen[step].id;
			if (best == want) {
				printk("      %2d단계 OK   id %6d 로짓 %9.6f  (호스트 %9.6f '%s')  %" PRId64 " ms\n",
				       step + 1, best, (double)lg[best],
				       (double)l6c_host_gen[step].logit,
				       l6c_host_gen[step].piece, dt);
				matched++;
			} else {
				printk("      %2d단계 FAIL id %6d (호스트 %6d '%s')  %" PRId64 " ms\n",
				       step + 1, best, want, l6c_host_gen[step].piece, dt);
				fails++;
				/*
				 * ⚠ **여기서 멈춘다.** 토큰이 갈리면 다음 단계의
				 *   입력이 달라져 비교가 무의미해진다. 계속 돌리면
				 *   "여러 단계가 실패" 로 보여 **갈린 지점을 가린다**.
				 */
				printk("      → 입력이 갈렸으므로 멈춘다 (뒤는 비교가 무의미하다)\n");
				break;
			}
			t = best;
		}

		printk("  [5] 결과\n");
		printk("      생성: \"%s\"\n", text);
		if (matched == L6C_N_GEN) {
			printk("      OK — %d/%d 토큰 일치\n", matched, L6C_N_GEN);
			printk("      → KV 캐시와 DeltaNet 상태가 토큰 사이로 제대로 넘어간다\n");
		} else {
			printk("      %d/%d 토큰 일치\n", matched, L6C_N_GEN);
			if (matched >= 1) {
				printk("      ⚠ 첫 토큰은 맞고 %d번째에서 갈렸다 — **상태 전달**을 의심한다\n",
				       matched + 1);
				printk("        (L6b 가 첫 토큰을 이미 가렸으므로 순전파 자체는 아니다)\n");
			}
		}
		heap("생성 뒤");
	}

done:
	if (fails == 0) {
		printk("=== L6c PASS (실패 0) ===\n");
		moru_led_set(MORU_LED_READY);
	} else {
		printk("=== L6c FAIL (실패 %d) ===\n", fails);
		moru_led_set(MORU_LED_FAIL);
	}
#ifdef LLM_CLK1416
	clk1416_report();
	clk1416_back_to_1104("시험 끝");
#endif
	/* ⚠ main 에서 돌아오지 않는다 — 이유는 apps/llm_l6b/src/main.cpp 참고
	 *   (정적 소멸자가 NULL 로 뛴다). */
	printk("    (시험 끝 — 여기서 멈춘다. 전원을 다시 주면 moru 로 돌아온다)\n");
	k_sleep(K_FOREVER);
	return 0;
}
