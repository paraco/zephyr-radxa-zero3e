/*
 * L6b — 한 토큰 순전파. 호스트 골든값과 층별 대조.
 *
 * 사다리 L6 의 둘째 칸 (docs/l6-decisions.md 의 "하위 사다리").
 * 토크나이저·샘플러는 쓰지 않는다 — 토큰 id 를 고정한다(golden.inc).
 *
 * ── 적재 경로 ───────────────────────────────────────────────────────────
 *
 * `llama_model_load_from_file_ptr(FILE *, params)` 가 **공개 API** 다
 * (`llama.h:533`, `llama.cpp:488`). `fmemopen` 으로 제자리 블롭에 FILE 을
 * 씌워 넘기면 끝이다 — 경로를 받는 `llama_model_load_from_file` 은
 * `ggml_fopen` 을 타서 Zephyr 에서 쓸 수 없다.
 *
 * L6a 가 그 길의 각 조각을 따로 가렸고(메타데이터·제자리 매핑·어휘),
 * 여기서는 상류 진입점 하나로 묶어 쓴다.
 *
 * ── 층별 값을 어떻게 꺼내나 ─────────────────────────────────────────────
 *
 * 호스트의 `llama-eval-callback` 은 `common/` 의 `common_debug_cb_eval` 을
 * 쓴다. 우리는 `common/` 을 넣지 않았으므로 `llama_context_params::cb_eval`
 * 에 직접 콜백을 건다 — 같은 자리에서 같은 텐서를 받는다.
 *
 * ⚠ 콜백은 **두 번** 불린다 (`ggml-backend.h:333`):
 *     ask=true  → "이 노드를 보겠는가" 를 묻는다. true 를 돌려주면
 *     ask=false → 계산이 **끝난 뒤** 데이터와 함께 다시 부른다.
 *   ask=true 에서 값을 읽으면 **아직 계산되지 않은 쓰레기**를 본다.
 *
 * ⚠ 합(sum)은 상쇄가 있어 작은 값에서 상대오차가 커 보인다. 그래서
 *   허용 오차를 **ppb(십억분율)** 로 두고, 합이 0 에 가까울 때는 |합| 으로
 *   정규화한다 — L5a·L5b 가 쓰는 방식 그대로(`llm_fullsum.h`).
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/mem_stats.h>

#include "ggml.h"
#include "ggml-backend.h"
#include "llama.h"
#include "llm_fullsum.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "golden.inc"

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(BLOB_NODE))

/* 실제 모델 크기 — L6a 와 같은 상수. 시험 GGUF 가 올라오면 걸린다. */
#define MODEL_BYTES 532517120u

#define N_GOLDEN ((int)(sizeof(l6b_golden) / sizeof(l6b_golden[0])))

/*
 * 허용 오차 — ppb.
 *
 * L3·L4 에서 Q4 역양자화와 양자화 내적이 **비트 일치**함을 확인했고,
 * L5a·L5b 에서 libm(sin/cos/exp/sigmoid)만 ~1 ulp(94~392 ppb) 갈림을
 * 확인했다. 1,017 노드를 지나면 그것이 쌓이므로 넉넉히 둔다.
 *
 * ⚠ 이 값을 "맞을 때까지" 키우지 않는다. 넘으면 **어디서** 갈렸는지
 *   좁히는 것이 할 일이다 — 지점이 9개인 이유다.
 */
#define TOL_PPB 20000	/* 2e-5 상대오차 */

static int fails;
static int seen;
static int skipped;

struct cb_state {
	int matched;
};

/*
 * 골든 목록에서 **(이름, 몇 번째)** 로 찾는다. 없으면 -1.
 *
 * ⚠ **이름이 유일하지 않다.** 1,017개 노드에 서로 다른 이름이 933개뿐이다.
 *   `norm-0` 은 3번 나오고 값이 전부 다르다
 *   (-35.549904 · 184.643585 · -8.183719).
 *   처음에 이름만으로 찾았더니 2·3번째가 1번째 골든과 비교돼 **보드가
 *   정확히 맞춘 값이 FAIL 로 나왔다** — 실제로 겪었다. 셋 다 비트 일치였다.
 *
 * 그래서 콜백이 같은 이름을 몇 번째로 보는지 센다. 상류 eval-callback 도
 * 그래프 순서대로 찍으므로 순서가 맞는다.
 */
#define SEEN_MAX 64
static struct { const char *name; int count; } seen_names[SEEN_MAX];
static int seen_names_n;

static int bump_seen(const char *name)
{
	for (int i = 0; i < seen_names_n; i++) {
		if (strcmp(seen_names[i].name, name) == 0) {
			return seen_names[i].count++;
		}
	}
	if (seen_names_n < SEEN_MAX) {
		seen_names[seen_names_n].name = name;	/* 텐서가 이름을 소유한다 */
		seen_names[seen_names_n].count = 1;
		seen_names_n++;
	}
	return 0;
}

static int golden_index(const char *name, int nth)
{
	for (int i = 0; i < N_GOLDEN; i++) {
		if (l6b_golden[i].nth == nth &&
		    strcmp(l6b_golden[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

/* 이 이름이 골든에 **하나라도** 있나 — ask 단계에서 쓴다 */
static bool name_wanted(const char *name)
{
	for (int i = 0; i < N_GOLDEN; i++) {
		if (strcmp(l6b_golden[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool eval_cb(struct ggml_tensor *t, bool ask, void *user_data)
{
	struct cb_state *st = (struct cb_state *)user_data;

	const char *nm = ggml_get_name(t);

	if (ask) {
		/*
		 * ⚠ 여기서 값을 읽으면 안 된다 — 아직 계산 전이다.
		 * ⚠ 세는 것도 여기서 하면 안 된다 — ask 와 실제 호출이 1:1 이
		 *   아닐 수 있다. ask 는 이름만 보고 답한다.
		 */
		return name_wanted(nm);
	}

	const int nth = bump_seen(nm);
	const int gi  = golden_index(nm, nth);
	if (gi < 0) {
		/* 골든에 없는 차례다 — 조용히 넘기지 않고 센다 */
		skipped++;
		return true;
	}

	seen++;

	/* f32 만 다룬다 — 골든값이 f32 노드에서 나왔다. */
	if (t->type != GGML_TYPE_F32) {
		printk("      %-20s FAIL — 타입이 %s 다 (f32 가 아니다)\n",
		       l6b_golden[gi].name, ggml_type_name(t->type));
		fails++;
		return true;
	}

	/* 연산 종류도 대조한다 — 그래프가 다르게 짜였으면 여기서 걸린다 */
	const char *op = ggml_op_name(t->op);
	if (strcmp(op, l6b_golden[gi].op) != 0) {
		printk("      %-20s ⚠ 연산이 %s 다 (골든은 %s) — 그래프가 다르다\n",
		       l6b_golden[gi].name, op, l6b_golden[gi].op);
		fails++;
		return true;
	}

	const int64_t n = ggml_nelements(t);
	struct llm_fullsum fs = llm_fullsum_of((const float *)t->data, (size_t)n);

	/* 비유한 값은 조용히 통과하지 못하게 한다 — L4 검토에서 배운 것 */
	if (!std::isfinite(fs.sum) || !std::isfinite(fs.abssum)) {
		printk("      %-20s FAIL — 합이 유한하지 않다 (sum=%g |sum|=%g)\n",
		       l6b_golden[gi].name, (double)fs.sum, (double)fs.abssum);
		fails++;
		return true;
	}

	const float want = l6b_golden[gi].sum;
	const float diff = fs.sum - want;
	/* 상쇄가 있는 합은 |합| 으로 정규화한다 (L5b 와 같은 방식) */
	const float scale = fs.abssum > 1.0f ? fs.abssum : 1.0f;
	const int64_t ppb = (int64_t)(fabsf(diff) / scale * 1e9f);

	if (ppb <= TOL_PPB) {
		printk("      %-18s#%d OK  합 %14.6f (골든 %14.6f) %" PRId64 " ppb\n",
		       l6b_golden[gi].name, nth, (double)fs.sum, (double)want, ppb);
		st->matched++;
	} else {
		printk("      %-18s#%d FAIL 합 %14.6f (골든 %14.6f) **%" PRId64 " ppb**\n",
		       l6b_golden[gi].name, nth, (double)fs.sum, (double)want, ppb);
		fails++;
	}
	return true;
}

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

int main(void)
{
	printk("=== L6b: 한 토큰 순전파 (골든 대조) ===\n");
	printk("  창 0x%08" PRIxPTR " + %u MiB · 토큰 %d · 대조 지점 %d개 · 허용 %d ppb\n",
	       (uintptr_t)BLOB_ADDR, (unsigned)(BLOB_SIZE / (1024 * 1024)),
	       L6B_TOKEN, N_GOLDEN, TOL_PPB);

	/* [1] 블롭이 실제 모델인가 — L6a 와 같은 관문 */
	printk("  [1] 블롭 점검 ... ");
	const uint8_t *p = (const uint8_t *)BLOB_ADDR;
	if (*(const uint32_t *)p != 0x46554747u) {
		printk("FAIL — 매직이 0x%08x 다\n", *(const uint32_t *)p);
		fails++;
		goto done;
	}
	{
		const uint64_t nt = *(const uint64_t *)(p + 8);
		printk("OK — 'GGUF' v%u, 텐서 %" PRIu64 "개\n",
		       *(const uint32_t *)(p + 4), nt);
		if (nt < 16) {
			printk("      FAIL — 시험 GGUF 가 올라왔다 (파티션 3 의 blob.bin 이 아니다)\n");
			fails++;
			goto done;
		}
	}

	llama_backend_init();
	heap("backend_init 뒤");

	{
		/* [2] 모델 적재 — 상류 공개 API 하나로 */
		printk("  [2] 모델 적재 (llama_model_load_from_file_ptr) ... ");
		FILE *f = fmemopen((void *)BLOB_ADDR, MODEL_BYTES, "rb");
		if (f == nullptr) {
			printk("FAIL — fmemopen\n");
			fails++;
			goto done;
		}

		llama_model_params mp = llama_model_default_params();
		mp.n_gpu_layers = 0;
		mp.load_mode    = LLAMA_LOAD_MODE_MMAP;	/* 제자리 적재 — 결정 3 */
		mp.check_tensors = false;
		mp.use_extra_bufts = false;		/* repack 버퍼를 쓰지 않는다 */

		int64_t t0 = k_uptime_get();
		llama_model *model = llama_model_load_from_file_ptr(f, mp);
		int64_t t_load = k_uptime_get() - t0;
		if (model == nullptr) {
			printk("FAIL — nullptr\n");
			fails++;
			goto done;
		}
		printk("OK (%" PRId64 " ms)\n", t_load);
		{
			char desc[128] = {0};
			llama_model_desc(model, desc, sizeof desc);
			printk("      %s\n", desc);
			printk("      n_embd %d · n_layer %d · n_vocab %d · 파라미터 %" PRIu64 "\n",
			       llama_model_n_embd(model), llama_model_n_layer(model),
			       llama_vocab_n_tokens(llama_model_get_vocab(model)),
			       llama_model_n_params(model));
		}
		heap("모델 적재 뒤");

		/*
		 * [2a] 백엔드 쪽 수치를 먼저 찍는다.
		 *
		 * ⚠ 왜: 호스트는 `compute buffer size = 2.22 MiB` 인데 보드가
		 *   **197.41 MB** 를 요구했다(약 89배). 메모리를 더 주는 것이 아니라
		 *   **무엇이 다른지**를 봐야 한다. 버퍼 크기 계산에 들어가는 값은
		 *   정렬·최대크기·장치 메모리 보고다 — 셋 다 우리 포팅이 손댄
		 *   자리다(ggml_port.c 의 ggml_zephyr_sysconf).
		 */
		/*
		 * [2b] **가장 큰 연속 블록**을 잰다.
		 *
		 * ⚠ 왜 필요한가: 남은 자리가 25 MB 인데 13.66 MB 할당이 실패했다.
		 *   `malloc_runtime_stats_get` 의 `free_bytes` 는 **합계**지, 연속
		 *   블록이 아니다. 어휘 적재가 248,320개 토큰의 작은 할당을 하고
		 *   그 중 20 MB 를 돌려주므로(최대 82,667 → 지금 62,839 KiB) 구멍이
		 *   흩어진다. ggml 은 **연속** 블록을 요구한다.
		 *
		 *   "합계가 남았는데 왜 안 되나" 를 추측으로 메우지 않고 잰다.
		 */
		printk("  [2b] 가장 큰 연속 블록 ... ");
		{
			size_t lo = 0, hi = 128u * 1024 * 1024;
			while (lo + 65536 < hi) {
				const size_t mid = lo + (hi - lo) / 2;
				void *q = malloc(mid);
				if (q != nullptr) {
					free(q);
					lo = mid;
				} else {
					hi = mid;
				}
			}
			printk("%zu KiB\n", lo / 1024);
			struct sys_memory_stats ms;
			if (malloc_runtime_stats_get(&ms) == 0) {
				printk("      (합계로 남은 %zu KiB — 차이가 **단편화**다)\n",
				       ms.free_bytes / 1024);
			}
		}

		printk("  [2a] 백엔드 수치\n");
		printk("      장치 %zu개 · 백엔드 %zu개\n",
		       ggml_backend_dev_count(), ggml_backend_reg_count());
		{
			ggml_backend_dev_t dev =
				ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
			if (dev == nullptr) {
				printk("      FAIL — CPU 장치가 없다\n");
				fails++;
				goto done;
			}
			size_t dev_free = 0, dev_total = 0;
			ggml_backend_dev_memory(dev, &dev_free, &dev_total);
			printk("      '%s' 메모리 보고: 남은 %zu KiB / 전체 %zu KiB\n",
			       ggml_backend_dev_name(dev), dev_free / 1024, dev_total / 1024);

			ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
			printk("      buft '%s': 정렬 %zu · 최대 %zu KiB\n",
			       ggml_backend_buft_name(buft),
			       ggml_backend_buft_get_alignment(buft),
			       ggml_backend_buft_get_max_size(buft) / 1024);

			/* 작은 텐서 하나의 alloc_size — 정렬이 미친 값이면 여기서 보인다 */
			ggml_init_params ip = { 1024 * 1024, nullptr, true };
			ggml_context *tc = ggml_init(ip);
			if (tc != nullptr) {
				ggml_tensor *t1 =
					ggml_new_tensor_1d(tc, GGML_TYPE_F32, 1024);
				printk("      f32[1024]: nbytes %zu · alloc_size %zu\n",
				       ggml_nbytes(t1),
				       ggml_backend_buft_get_alloc_size(buft, t1));
				ggml_free(tc);
			}
		}

		/* [3] 문맥 — n_ctx 를 작게. 자리가 많지 않다 */
		printk("  [3] 문맥 생성 (n_ctx=32 -> 256 으로 올림된다) ... ");
		struct cb_state st = { 0 };
		llama_context_params cp = llama_context_default_params();
		cp.n_ctx           = 32;
		cp.n_batch         = 1;
		cp.n_ubatch        = 1;
		cp.n_threads       = 1;
		cp.n_threads_batch = 1;
		cp.cb_eval           = eval_cb;
		cp.cb_eval_user_data = &st;
		/*
		 * ⚠ flash-attn 은 **끈다.** 호스트 골든값이 기본 설정에서 나왔고,
		 *   flash-attn 경로는 simd-gemm.h 의 타일 크기로 갈린다
		 *   (tools/host_golden.sh 의 -U__ARM_NEON__ 주석 참고).
		 */
		cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;

		llama_context *ctx = llama_init_from_model(model, cp);
		if (ctx == nullptr) {
			printk("FAIL — nullptr (자리가 모자라나?)\n");
			fails++;
			goto done;
		}
		printk("OK\n");
		heap("문맥 생성 뒤");

		/* [4] 토큰 하나 — 콜백이 지점마다 대조한다 */
		printk("  [4] 순전파 1토큰 (id=%d)\n", L6B_TOKEN);
		llama_token tok = L6B_TOKEN;
		llama_batch batch = llama_batch_get_one(&tok, 1);

		t0 = k_uptime_get();
		const int rc = llama_decode(ctx, batch);
		int64_t t_dec = k_uptime_get() - t0;
		if (rc != 0) {
			printk("      FAIL — llama_decode 가 %d 를 돌려줬다\n", rc);
			fails++;
		} else {
			printk("      llama_decode OK (%" PRId64 " ms)\n", t_dec);
		}
		heap("순전파 뒤");

		printk("  [5] 대조 결과 ... ");
		if (st.matched == N_GOLDEN && fails == 0) {
			printk("OK — %d/%d 지점 일치\n", st.matched, N_GOLDEN);
		} else {
			printk("%d/%d 지점 일치 (본 노드 %d개 · 골든에 없는 차례 %d개)\n",
			       st.matched, N_GOLDEN, seen, skipped);
			if (st.matched < N_GOLDEN) {
				printk("      ⚠ 못 본 지점이 있다 — 그래프의 노드 이름이\n");
				printk("        골든(호스트)과 다르다. golden.inc 를 다시 만들어라.\n");
				fails++;
			}
		}
	}

done:
	if (fails == 0) {
		printk("=== L6b PASS (실패 0) ===\n");
	} else {
		printk("=== L6b FAIL (실패 %d) ===\n", fails);
	}
	return 0;
}
