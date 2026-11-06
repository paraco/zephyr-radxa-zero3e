/*
 * l2_golden.h — 생성 파일이다. 손으로 고치지 않는다.
 *
 *   $ python3 tools/l2_make_gguf.py
 *
 * 출처: llama.cpp 의 참조 GGUFWriter 로 만든 파일을 참조 GGUFReader 로
 * 되읽어 뽑았다. apps/llm_l2 의 파서와 독립이다.
 * llama.cpp: 6c01da6
 */
#ifndef L2_GOLDEN_H
#define L2_GOLDEN_H

#include <stdint.h>

#define L2_FILE_SIZE      3872u
#define L2_FILE_CRC32     0x8320cd20u
#define L2_VERSION        3u
#define L2_TENSOR_COUNT   6u
#define L2_KV_COUNT       8u
#define L2_ALIGNMENT      32u
#define L2_DATA_OFFSET    672u

struct l2_golden_tensor {
	const char *name;
	uint32_t    n_dims;
	uint64_t    dims[4];
	uint32_t    type;
	uint64_t    offset;
};

static const struct l2_golden_tensor l2_tensors[] = {
	{ "t.f32.1d", 1u, { 64u, 0u, 0u, 0u }, 0u, 0u },
	{ "t.f32.2d", 2u, { 32u, 8u, 0u, 0u }, 0u, 256u },
	{ "t.f16.3d", 3u, { 32u, 4u, 2u, 0u }, 1u, 1280u },
	{ "t.f32.4d", 4u, { 16u, 4u, 2u, 2u }, 0u, 1792u },
	{ "t.q4_0", 2u, { 32u, 4u, 0u, 0u }, 2u, 2816u },
	{ "t.q4_k", 2u, { 256u, 2u, 0u, 0u }, 12u, 2912u },
};

/* KV 중 값까지 확인하는 것들 */
#define L2_KV_ARCH_VAL  "moru-l2-test"
#define L2_KV_ARCH_KEY  "general.architecture"
#define L2_KV_ARCH_TYPE 8u
#define L2_KV_U32_VAL  0xdeadbeefu
#define L2_KV_U32_KEY  "moru.u32"
#define L2_KV_U32_TYPE 4u
#define L2_KV_STR_VAL  "모루랩/MoruLab"
#define L2_KV_STR_KEY  "moru.str"
#define L2_KV_STR_TYPE 8u
#define L2_KV_BOOL_T_VAL  1
#define L2_KV_BOOL_T_KEY  "moru.bool_true"
#define L2_KV_BOOL_T_TYPE 7u
#define L2_KV_BOOL_F_VAL  0
#define L2_KV_BOOL_F_KEY  "moru.bool_false"
#define L2_KV_BOOL_F_TYPE 7u
#define L2_KV_ARR_KEY   "moru.arr_u32"
#define L2_KV_ARR_ELEMT 5u
#define L2_KV_ARR_LEN   5u
#define L2_KV_F32_KEY   "moru.f32"
#define L2_KV_F32_BITS  0x3fc00000u

#endif /* L2_GOLDEN_H */
