/* ggml-quants.c 의 **양자화** 경로만 쓰는 심볼들. 역양자화는 쓰지 않는다.
 * 불려도 조용히 틀린 값이 나오지 않게 큰 소리로 죽는다. */
#include <zephyr/kernel.h>
#include <stdarg.h>
#include <stddef.h>
#include "ggml.h"

void ggml_abort(const char *file, int line, const char *fmt, ...)
{
	printk("ggml_abort %s:%d: %s\n", file, line, fmt);
	k_panic();
}
size_t ggml_row_size(enum ggml_type t, int64_t n) { (void)t; (void)n; printk("스텁 ggml_row_size\n"); k_panic(); return 0; }
const char *ggml_type_name(enum ggml_type t) { (void)t; printk("스텁 ggml_type_name\n"); k_panic(); return ""; }
size_t ggml_type_size(enum ggml_type t) { (void)t; printk("스텁 ggml_type_size\n"); k_panic(); return 0; }
