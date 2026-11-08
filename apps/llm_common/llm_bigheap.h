/* SPDX-License-Identifier: Apache-2.0 — llm_bigheap.c 참고 */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t llm_bigheap_used(void);
size_t llm_bigheap_peak(void);
size_t llm_bigheap_size(void);
#ifdef __cplusplus
}
#endif
