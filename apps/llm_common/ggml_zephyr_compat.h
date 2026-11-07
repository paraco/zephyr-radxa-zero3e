/*
 * ggml 을 Zephyr 로 빌드하기 위한 호환 층.
 *
 * ⚠ **앱의 모든 소스에 `-include` 로 강제 포함된다** — ggml 소스뿐 아니라
 *   `main.c`·`*_graph.c`·`ggml_port.c` 까지다(cmake/llm_ggml.cmake 의
 *   target_compile_options). 강제 포함은 소스 1행보다 **먼저** 처리되므로
 *   소스 안에서 `#define` 으로 이 파일의 동작을 피할 수는 없다.
 *   실제로 그렇게 하려다 무한 재귀를 만들었다 — ggml_port.c 의 주석 참고.
 *
 * 원칙: 상류 ggml 소스를 한 줄도 고치지 않는다. 고치면 llama.cpp 를 올릴 때마다
 * 충돌을 손으로 풀어야 하고, 우리 수정과 상류 버그를 구분할 수 없게 된다.
 *
 * 강제 포함이라 **libc 헤더보다 먼저** 들어간다 — 기능 노출 매크로를 여기서
 * 켤 수 있는 이유다.
 */
#ifndef GGML_ZEPHYR_COMPAT_H
#define GGML_ZEPHYR_COMPAT_H

/*
 * ① M_PI.
 * picolibc 의 math.h 는 M_PI 를 `#if __BSD_VISIBLE || __XSI_VISIBLE` 로
 * 감싼다. Zephyr 가 _POSIX_C_SOURCE 를 세우면 __BSD_VISIBLE 이 꺼져 M_PI 가
 * 안 보인다. ggml-cpu/ops.cpp 가 RoPE 에서 쓴다.
 * 손으로 #define M_PI 하면 math.h 와 재정의 충돌이 날 수 있으므로
 * **기능 노출 매크로를 켜서 libc 가 주게 한다.**
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

/*
 * ② sysconf.
 *
 * Zephyr 의 sysconf enum(zephyr/posix/sys/sysconf.h)에 `_SC_PAGE_SIZE` 는
 * 있으나 **`_SC_PHYS_PAGES` 는 없다.** 우리가 컴파일하는 ggml 소스에서
 * sysconf 를 쓰는 곳은 ggml-cpu/ggml-cpu.cpp:373-374 두 줄뿐이고 둘 다
 * 장치 메모리 보고용이다.
 *
 * 상수만 정의하면 진짜 sysconf 가 -1 을 돌려주고 *total 이 쓰레기가 된다.
 * 그래서 **호출 자체를 우리 함수로 돌린다.** 구현과 번호는
 * apps/llm_common/ggml_port.c 에 있다.
 */
/*
 * ⚠ **없는 것만 정의한다.** `_SC_PAGE_SIZE` 는 Zephyr 의 sysconf.h 에
 *   **enum 멤버**로 이미 있다. 강제 포함이 그 헤더보다 먼저 들어가므로
 *   여기서 매크로로 덮으면 enum 선언의 식별자가 숫자로 치환돼
 *   `expected identifier before numeric constant` 로 빌드가 깨진다 —
 *   실제로 겪었다. 있는 값은 enum 그대로 쓰고 ggml_port.c 가 그 값으로 분기한다.
 */
#define GGML_ZEPHYR_SC_PHYS_PAGES	0x7A50	/* 'zP' — enum 값(작은 수)과 안 겹친다 */

#ifndef _SC_PHYS_PAGES
#define _SC_PHYS_PAGES GGML_ZEPHYR_SC_PHYS_PAGES
#endif

#ifdef __cplusplus
extern "C" {
#endif
long ggml_zephyr_sysconf(int opt);
#ifdef __cplusplus
}
#endif

/*
 * ⚠ 이 매크로는 **되돌릴 수 없다**(위 경고 참고). ggml_port.c 는 그래서
 *   sysconf 를 아예 부르지 않고 필요한 값을 직접 돌려준다.
 */
#define sysconf(x) ggml_zephyr_sysconf(x)


/*
 * ── ggml 스케줄러 문맥 버퍼 — 포팅 항목 9번 ─────────────────────────────
 *
 * `ggml-backend.cpp:2251`:
 *     context_buffer_size = ggml_sched_max_splits
 *                         * GGML_SCHED_MAX_SPLIT_INPUTS * 2
 *                         * sizeof(struct ggml_tensor)
 *                         + ggml_graph_overhead_custom(graph_size, false);
 *
 * qwen35 에서 `graph_size = max_nodes = 32 * n_tensors = 10240` 이고
 * (`llama-context.cpp:2745`), `ggml_sched_max_splits = graph_size` 다
 * ("노드마다 split 하나" 라는 최악 가정, `:2241` 주석).
 *
 *     10240 × 30 × 2 × 336 ≈ **197 MB**
 *
 * 실기에서 그 malloc 이 실패해 멈췄다. 호스트에서는 같은 197 MB 를 조용히
 * 받아 가므로 드러나지 않는다 — 호스트의 `compute buffer size = 2.22 MiB`
 * 는 **다른 버퍼**(활성)다.
 *
 * ⚠ 두 추정이 **곱해진다**:
 *   ① `graph_size` 10240 은 실제 노드 ~1,431개의 7배다.
 *   ② `GGML_SCHED_MAX_SPLIT_INPUTS` 30 은 **백엔드가 여러 개일 때** 쪼갠
 *      조각의 입력 수다. 우리는 **백엔드가 1개**라 split 이 사실상 하나뿐이고
 *      백엔드 간 복사도 없다.
 *   ①은 llama_context 안에 있어 손댈 수 없다. ②는 `#ifndef` 로 감싸여
 *   있으므로(`ggml-backend.cpp:1167`) 여기서 정한다 — **상류 수정 0건**.
 *
 * **2 로 둔다.** 측정으로 정했다:
 *     30 → 197.41 MB  (실패)
 *      4 →  26.79 MB  (실패 — 모델·어휘 뒤 남은 자리가 48.3 MB 인데
 *                      output 0.95 + KV 3.00 + RS 19.27 를 먼저 쓴다)
 *      2 →  13.4 MB   (들어간다)
 *
 * ⚠ **줄여도 안전한 근거.** 이 값은 `split->inputs_capacity` 와
 *   `sched->graph_inputs_capacity` 의 **초기** 용량일 뿐이다.
 *   `ggml_backend_sched_split_inputs_grow`(`:1255`) ·
 *   `ggml_backend_sched_graph_inputs_grow`(`:1270`) 가 모자라면
 *   **`realloc` 로 2배씩 늘린다** — 그 배열은 힙에 있고 문맥 버퍼가 아니다.
 *   즉 이 값이 바꾸는 것은 **문맥 버퍼 크기 하나**다.
 *
 * ⚠ 그래도 모자라면 **조용히 틀리지 않는다** — `ggml_init` 이 no_alloc 로
 *   그 버퍼를 쓰므로 텐서 자리가 없으면 거기서 멈춘다. 백엔드를 늘리는 날
 *   (GPU·NPU) 이 값을 되돌려야 한다.
 *
 * ⚠ **상류에 보고할 것:** `ggml-backend.cpp:2252` 의
 *   `sched->context_buffer = (char *) malloc(...)` 가 **결과를 확인하지
 *   않는다.** 그래서 메모리가 부족한 시스템에서는 실패가 한참 뒤
 *   `ggml_init` 의 `GGML_ASSERT(ctx->mem_buffer != NULL)`(ggml.c:1662)로
 *   나타나 원인을 가리기 어렵다. 우리가 그 길을 겪었다.
 */
#ifndef GGML_SCHED_MAX_SPLIT_INPUTS
#define GGML_SCHED_MAX_SPLIT_INPUTS 2
#endif

#endif /* GGML_ZEPHYR_COMPAT_H */
