/*
 * L6b — 한 토큰 순전파. **지금은 크기 측정용 골격이다.**
 *
 * 먼저 가리는 것: `llama.cpp/src` 전체(32) + `src/models`(175) = **207개**가
 * Zephyr 에서 **링크되고 OTA 상한 8 MiB 에 들어가는가**.
 *
 * 왜 207개 전부인가: `llama-model.cpp:351` 의 공장 switch 가
 * `new llama_model_qwen35(params)` 꼴로 175개 생성자를 **전부 참조**한다.
 * `--gc-sections` 는 참조된 심볼을 버리지 못한다.
 *
 * 그래프 대조는 apps/llm_l6b/golden.inc 가 준비돼 있다 (토큰 5834, 지점 9개).
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/mem_stats.h>

#include "llama.h"
#include "ggml.h"

#include <cinttypes>
#include <cstdio>

#include "golden.inc"

extern "C" int malloc_runtime_stats_get(struct sys_memory_stats *stats);

#define BLOB_NODE DT_NODELABEL(model_blob)
#define BLOB_ADDR ((uintptr_t)DT_REG_ADDR(BLOB_NODE))

int main(void)
{
	printk("=== L6b: 골격 (링크·크기 측정) ===\n");
	printk("  블롭 0x%08" PRIxPTR " · 골든 토큰 %d · 대조 지점 %u개 · 노드 %d개\n",
	       (uintptr_t)BLOB_ADDR, L6B_TOKEN,
	       (unsigned)(sizeof(l6b_golden) / sizeof(l6b_golden[0])), L6B_N_NODES);

	struct sys_memory_stats st;
	if (malloc_runtime_stats_get(&st) == 0) {
		printk("  힙: 쓴 %zu KiB · 남은 %zu KiB\n",
		       st.allocated_bytes / 1024, st.free_bytes / 1024);
	}

	/* 상류 심볼이 정말 링크됐는지 — 부르지 않고 주소만 본다 */
	printk("  llama_backend_init  @ %p\n", (void *)&llama_backend_init);
	printk("  llama_model_load_from_file @ %p\n", (void *)&llama_model_load_from_file);
	printk("  ggml_init           @ %p\n", (void *)&ggml_init);

	printk("  골든 첫 지점: %s %s sum=%f\n", l6b_golden[0].name,
	       l6b_golden[0].op, (double)l6b_golden[0].sum);
	printk("  골든 끝 지점: %s %s sum=%f\n",
	       l6b_golden[sizeof(l6b_golden) / sizeof(l6b_golden[0]) - 1].name,
	       l6b_golden[sizeof(l6b_golden) / sizeof(l6b_golden[0]) - 1].op,
	       (double)l6b_golden[sizeof(l6b_golden) / sizeof(l6b_golden[0]) - 1].sum);

	printk("=== L6b 골격 OK — 207개 소스가 링크됐다 ===\n");
	return 0;
}
