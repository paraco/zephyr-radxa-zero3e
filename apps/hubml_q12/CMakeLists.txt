# SPDX-License-Identifier: Apache-2.0
# hubml_q12 (Lm12) — ml shadow ls: 시범 운영(shadow) 점수 SCORE 의 날별 요약. === Lm12 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_q12)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hubml.cmake)
hubml_add()
target_sources(app PRIVATE src/main.c)
target_compile_definitions(app PRIVATE HUBML_SHELL_ROOT="/RAM:/ml" HUBML_SHELL_HUB_ID="4d425737333500000000000000032106")
target_include_directories(app PRIVATE ${HUBML_DIR}/testdata)
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind3.xmb ${gen_dir}/golden_kind3.xmb.inc)
