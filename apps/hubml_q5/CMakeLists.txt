# SPDX-License-Identifier: Apache-2.0
# hubml_q5 (Lm5) — 배포(모델 전송 · 승격 · 되돌리기) QEMU 시험. 판정 줄: === Lm5 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_q5)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hubml.cmake)
hubml_add()
target_sources(app PRIVATE src/main.c)
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind3.xmb ${gen_dir}/golden_kind3.xmb.inc)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind1.xmb ${gen_dir}/golden_kind1.xmb.inc)
target_include_directories(app PRIVATE ${HUBML_DIR}/testdata)
