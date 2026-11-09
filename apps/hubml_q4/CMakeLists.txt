# SPDX-License-Identifier: Apache-2.0
# hubml_q4 (Lm4) — 원장(JSONL · 해시 사슬 · 별칭) QEMU 시험. 판정 줄: === Lm4 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_q4)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hubml.cmake)
hubml_add()
target_sources(app PRIVATE src/main.c)
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind3.xmb ${gen_dir}/golden_kind3.xmb.inc)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind1.xmb ${gen_dir}/golden_kind1.xmb.inc)
