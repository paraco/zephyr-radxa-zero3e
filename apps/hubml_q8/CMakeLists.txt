# SPDX-License-Identifier: Apache-2.0
# hubml_q8 (Lm8) — ed25519 검증 · SHA-512 를 Mac cryptography/hashlib 값과 대조. === Lm8 PASS|FAIL
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubml_q8)
set(HUBML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../subsys/hubml)
target_include_directories(app PRIVATE ${HUBML_DIR}/include ${HUBML_DIR}/testdata ${HUBML_DIR}/src)
target_sources(app PRIVATE src/main.c ${HUBML_DIR}/src/ed25519.c)
target_compile_definitions(app PRIVATE HUBML_ED25519_SIGN)
