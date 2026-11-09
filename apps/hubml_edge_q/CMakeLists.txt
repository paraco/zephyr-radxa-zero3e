# SPDX-License-Identifier: Apache-2.0
# hubml_edge_q (Lm3) — Edge 쪽 코드(MCOL/1 · .xmb kind 1/3 평가)를 32비트 Cortex-M33 (QEMU mps2/an521) 에서 시험.
# 허브(Cortex-A55, 64비트)가 만든 모델 파일을 그대로 읽어 같은 값을 내는지 본다. 판정 줄: === Lm3 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_edge_q)
set(HUBML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../subsys/hubml)
target_include_directories(app PRIVATE ${HUBML_DIR}/include ${HUBML_DIR}/testdata)
target_compile_definitions(app PRIVATE XMB_MAX_FEAT=34 XMB_MAX_NODE=2400)
target_sources(app PRIVATE src/main.c ${HUBML_DIR}/src/mcol.c ${HUBML_DIR}/src/xmb.c)
# Cortex-M33 은 VFMA 가 있어 기본으로 축약될 수 있다 — Edge 펌웨어(R0)도 같은 옵션을 쓴다
set_source_files_properties(${HUBML_DIR}/src/xmb.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind3.xmb ${gen_dir}/golden_kind3.xmb.inc)
generate_inc_file_for_target(app ${HUBML_DIR}/testdata/golden_kind1.xmb ${gen_dir}/golden_kind1.xmb.inc)
