/*
 * L6a — 모델 적재 관문: **메타데이터 + 어휘**.
 *
 * 사다리(docs/llm-hub-test-plan.md)의 L6 를 셋으로 나눈 첫 칸
 * (docs/l6-decisions.md 의 "하위 사다리"). 가리는 것:
 *
 *   ① zephyr_future.h 가 **의미까지** 맞는가 (지금까지 컴파일만 확인했다)
 *   ② `gguf_init_from_file_ptr` 가 **제자리 블롭**에서 메타데이터를 읽는가
 *   ③ `llama_model_loader` 가 **복사 없이** 가중치 위치를 잡는가
 *      — llama_mmap_zephyr.cpp 의 SUPPORTED=true 가 실제로 먹히는가
 *   ④ 어휘 248,320 토큰이 힙에 올라가는가
 *   ⑤ **이미지가 OTA 상한 8 MiB 에 들어가는가** (build.sh 가 찍는다)
 *
 * 추론은 하지 않는다. 그래프는 L6b 다.
 *
 * ── 왜 이 순서인가 ──────────────────────────────────────────────────────
 *
 * ①을 맨 앞에 둔 것이 중요하다. `std::future` 대체판이 조용히 틀리면
 * `check_tensors` 경로가 아니라 **아무 데서나** 어긋난다. 양성 대조를
 * 먼저 세워 둔다 — L4 의 `[0a]` 를 넣은 것과 같은 이유다(그때 블롭을 안
 * 올려 전부 0 을 읽고도 ±0.0 이라 그럴듯해 보였다).
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/mem_stats.h>

/*
 * ⚠ libc 아레나의 **실제** 사용량을 재려면 이 함수가 필요하다.
 *   `west build` 가 찍는 "RAM 79.48%" 는 아레나(96 MiB)를 **전부 예약으로**
 *   세어서다 — 어휘가 그 중 얼마를 쓰는지는 알려 주지 않는다. L6b 는 ggml
 *   문맥과 1,017 노드 그래프가 더 필요하므로 남은 자리를 알아야 한다.
 *
 * ⚠ 선언이 **공개 include 밖**에 있다 (zephyr/lib/libc/common/include/
 *   sys_malloc.h). 앱에서 그 경로를 include 하면 내부 구조에 의존하게
 *   되므로, 선언만 여기에 적는다. CONFIG_SYS_HEAP_RUNTIME_STATS=y 가
 *   있어야 정의가 링크된다 (prj.conf 에 있다).
 */
extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "llama.h"
#include "llama-arch.h"
#include "llama-hparams.h"
#include "llama-model-loader.h"
#include "llama-vocab.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))
#define BLOB_SIZE ((size_t)DT_REG_SIZE(BLOB_NODE))

/* 측정값 — docs/l6-decisions.md. 블롭이 이것보다 작으면 시험 GGUF 가 올라온 것이다. */
#define MODEL_BYTES 532517120u

static int fails;

/* 아레나 사용량 한 줄. 단계마다 부른다 — 어디서 늘었는지 보려고. */
static void heap(const char *when)
{
	struct sys_memory_stats st;
	if (malloc_runtime_stats_get(&st) != 0) {
		printk("      힙: (못 읽었다 — CONFIG_SYS_HEAP_RUNTIME_STATS 가 없나)\n");
		return;
	}
	const size_t total = st.free_bytes + st.allocated_bytes;
	printk("      힙[%s]: 쓴 %zu KiB · 최대 %zu KiB · 남은 %zu KiB / %zu KiB\n",
	       when, st.allocated_bytes / 1024, st.max_allocated_bytes / 1024,
	       st.free_bytes / 1024, total / 1024);
}

static void ok(const char *what)
{
	printk("OK — %s\n", what);
}

static void bad(const char *what)
{
	printk("FAIL — %s\n", what);
	fails++;
}

/* ── [0] zephyr_future.h 의 의미 — 양성 대조 ──────────────────────────── */
/*
 * 상류 `llama-model-loader.cpp:1730` 이 하는 것과 **같은 꼴**로 쓴다:
 * vector 에 담고, 값 하나를 꺼내고, 예외가 올라오는지 본다.
 * 여기서 통과하지 못하면 아래는 전부 의미가 없다.
 */
static void check_future(void)
{
	printk("  [0] std::future 대체판 (zephyr_future.h) ... ");

	std::vector<std::future<std::pair<int, bool>>> v;
	v.emplace_back(std::async(std::launch::async,
				  [] { return std::make_pair(0x6d6f, true); }));
	v.emplace_back(std::async(std::launch::async,
				  []() -> std::pair<int, bool> {
					  throw std::runtime_error("의도된 예외");
				  }));

	auto r = v[0].get();
	if (r.first != 0x6d6f || !r.second) {
		printk("\n      값이 어긋났다: %#x / %d\n", r.first, (int)r.second);
		bad("future 값 전달");
		return;
	}

	bool caught = false;
	try {
		(void)v[1].get();
	} catch (const std::runtime_error &) {
		caught = true;
	}
	if (!caught) {
		printk("\n      예외가 올라오지 않았다 — 적재 실패가 조용히 묻힌다\n");
		bad("future 예외 재던지기");
		return;
	}

	/* deferred 와 인자 전달 */
	int base = 10;
	auto f = std::async(std::launch::deferred, [](int a, int b) { return a + b; },
			    base, 5);
	if (f.get() != 15) {
		bad("future 인자 전달");
		return;
	}
	ok("값 · 예외 재던지기 · deferred · 인자 전달");
}

/* ── [1] 블롭이 실제 모델인가 ─────────────────────────────────────────── */
static bool check_blob(size_t *out_size)
{
	printk("  [1] 블롭 점검 ... ");
	const uint8_t *p = (const uint8_t *)BLOB_ADDR;
	const uint32_t magic = *(const uint32_t *)p;

	if (magic != 0x46554747u) {	/* 'GGUF' (리틀엔디언) */
		printk("\n      매직이 0x%08x 다 (GGUF=0x46554747)\n", magic);
		printk("      → U-Boot 가 블롭을 안 올렸다. boot.cmd 의 fatload 를 보라.\n");
		bad("블롭 매직");
		return false;
	}

	/*
	 * ⚠ **크기를 여기서 가린다.** boot 파티션에는 L2·L3 용 시험
	 *   GGUF(3,872 B)가 남아 있고 그것도 매직이 'GGUF' 다. 매직만 보면
	 *   통과하고 아래가 "모델이 아니다" 로 죽는다 — 원인이 안 보인다
	 *   (sdcard/boot.cmd 의 경고와 같은 덫이다).
	 *
	 *   블롭 자체는 크기를 모른다(fatload 가 알려 주지 않는다). 그래서
	 *   GGUF 헤더에서 **텐서 수**로 가린다 — 시험 GGUF 는 2개다.
	 */
	const uint64_t n_tensors = *(const uint64_t *)(p + 8);
	const uint64_t n_kv      = *(const uint64_t *)(p + 16);
	printk("OK — 매직 'GGUF' v%u, 텐서 %" PRIu64 "개, KV %" PRIu64 "개\n",
	       *(const uint32_t *)(p + 4), n_tensors, n_kv);

	if (n_tensors < 16) {
		printk("      ⚠ 텐서가 %" PRIu64 "개뿐이다 — **시험 GGUF** 가 올라왔다.\n",
		       n_tensors);
		printk("        실제 모델은 파티션 3 의 blob.bin 이다. boot 파티션의\n");
		printk("        작은 blob.bin 이 가리고 있는지 보라(docs/llm-ladder-status.md).\n");
		bad("블롭이 실제 모델이 아니다");
		return false;
	}
	*out_size = MODEL_BYTES;
	return true;
}

/* ── [2] 메타데이터 — 제자리에서 ──────────────────────────────────────── */
/*
 * `gguf_init_from_file_ptr(FILE *)` 를 쓴다. `fmemopen` 으로 블롭 위에
 * FILE 을 씌운다 — picolibc tinystdio 에 있다(확인했다).
 *
 * ⚠ 왜 `gguf_init_from_buffer` 가 아닌가: 상류 `llama_model_loader` 생성자는
 *   `metadata` 를 받아도 **쓰지 않는다.** `fname` 이 비어 있고 `file` 이
 *   nullptr 이 아닐 때만 `weights_map`(텐서 위치)을 만든다
 *   (llama-model-loader.cpp:680 `else if (file != nullptr)`). 그 경로가
 *   `gguf_init_from_file_ptr` 를 부르므로 FILE 이 필요하다.
 *   `fname` 경로는 `ggml_fopen` 을 타서 Zephyr 에서 쓸 수 없다.
 */
static FILE *open_blob(size_t size)
{
	FILE *f = fmemopen((void *)BLOB_ADDR, size, "rb");
	if (f == nullptr) {
		printk("      fmemopen 실패 — 아레나가 모자라나?\n");
	}
	return f;
}

int main(void)
{
	printk("=== L6a: 모델 적재 관문 (메타데이터 + 어휘) ===\n");
	printk("  창: 0x%08" PRIxPTR " + %u MiB\n",
	       (uintptr_t)BLOB_ADDR, (unsigned)(BLOB_SIZE / (1024 * 1024)));

	check_future();
	heap("시작");

	size_t blob_size = 0;
	if (!check_blob(&blob_size)) {
		goto done;
	}

	{
		printk("  [2] 메타데이터 (gguf_init_from_file_ptr + fmemopen) ... ");
		FILE *f = open_blob(blob_size);
		if (f == nullptr) {
			bad("fmemopen");
			goto done;
		}

		std::vector<std::string> splits;
		llama_model_loader *ml = nullptr;
		int64_t t0 = k_uptime_get();
		try {
			ml = new llama_model_loader(
				/* meta            */ nullptr,
				/* set_tensor_data */ nullptr,
				/* ud              */ nullptr,
				/* fname           */ std::string(),
				/* splits          */ splits,
				/* file            */ f,
				/* load_mode       */ LLAMA_LOAD_MODE_MMAP,
				/* check_tensors   */ false,
				/* no_alloc        */ true,
				/* load_mtp        */ false,
				/* kv_overrides    */ nullptr,
				/* buft_overrides  */ nullptr);
		} catch (const std::exception &e) {
			printk("\n      예외: %s\n", e.what());
			bad("llama_model_loader 생성");
			goto done;
		}
		int64_t t_meta = k_uptime_get() - t0;

		printk("OK (%" PRId64 " ms)\n", t_meta);
		printk("      arch        %s\n", ml->arch_name.c_str());
		printk("      텐서        %d개 (weights_map %zu)\n",
		       ml->n_tensors, ml->weights_map.size());
		printk("      데이터 시작 0x%zx\n", gguf_get_data_offset(ml->metadata));
		heap("메타데이터 뒤");

		/* [3] 제자리 적재가 실제로 켜졌나 — 결정 3 의 핵심 한 줄 */
		printk("  [3] 제자리 적재 (llama_mmap::SUPPORTED) ... ");
		if (!llama_mmap::SUPPORTED) {
			printk("\n      SUPPORTED=false — 상류 fallback 이 링크됐다.\n");
			printk("      llama_mmap_zephyr.cpp 가 빠졌거나 상류 llama-mmap.cpp 가\n");
			printk("      cmake/llama-sources.txt 에 들어 있다. 508 MiB 를 또 복사한다.\n");
			bad("제자리 적재가 꺼졌다");
		} else if (!ml->use_mmap) {
			printk("\n      SUPPORTED 는 true 인데 로더가 use_mmap 을 껐다\n");
			bad("로더가 mmap 을 거부했다");
		} else {
			ok("SUPPORTED=true · 로더 use_mmap=true — 복사 없음");
		}

		/* [4] 텐서가 블롭 안을 가리키나 */
		printk("  [4] 텐서 위치가 블롭 안인가 ... ");
		size_t outside = 0, max_end = 0;
		for (const auto &kv : ml->weights_map) {
			const size_t off = kv.second.offs;
			const size_t n   = ggml_nbytes(kv.second.tensor);
			if (off + n > blob_size) {
				if (outside == 0) {
					printk("\n      첫 이탈: '%s' off=0x%zx + %zu > %zu\n",
					       kv.first.c_str(), off, n, blob_size);
				}
				outside++;
			}
			if (off + n > max_end) {
				max_end = off + n;
			}
		}
		if (outside == 0) {
			printk("OK — %zu개 전부 안쪽, 끝 0x%zx (%zu MiB)\n",
			       ml->weights_map.size(), max_end, max_end / (1024 * 1024));
		} else {
			printk("      FAIL — %zu개가 블롭 밖 (적재가 잘렸나?)\n", outside);
			bad("텐서 위치");
		}

		/* [5] 어휘 */
		printk("  [5] 어휘 적재 (llama_vocab::load) ... ");
		t0 = k_uptime_get();
		llama_vocab *vocab = nullptr;
		try {
			vocab = new llama_vocab();
			const LLM_KV kv_fn(llm_arch_from_string(ml->arch_name));
			vocab->load(*ml, kv_fn);
		} catch (const std::exception &e) {
			printk("\n      예외: %s\n", e.what());
			bad("어휘 적재");
			goto done;
		}
		int64_t t_vocab = k_uptime_get() - t0;
		printk("OK (%" PRId64 " ms)\n", t_vocab);
		printk("      토큰 %u개 · BOS %d · EOS %d\n",
		       (unsigned)vocab->n_tokens(), (int)vocab->token_bos(),
		       (int)vocab->token_eos());

		heap("어휘 뒤");

		if (vocab->n_tokens() < 100000) {
			printk("      ⚠ Qwen3.5 는 248,320 개여야 한다\n");
			bad("어휘 수");
		}
	}

done:
	if (fails == 0) {
		printk("=== L6a PASS (실패 0) ===\n");
	} else {
		printk("=== L6a FAIL (실패 %d) ===\n", fails);
	}
	return 0;
}
