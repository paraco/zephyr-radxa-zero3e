# SPDX-License-Identifier: Apache-2.0
# hubml_q — HUB AI ML 허브 모듈의 QEMU 시험 (실기 없이). 판정 줄: === Lm1 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_q)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/hubml.cmake)
hubml_add()
target_sources(app PRIVATE src/main.c)
