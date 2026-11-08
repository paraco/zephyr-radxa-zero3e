cmake_minimum_required(VERSION 3.20.0)
# 모델 가중치 창은 **공용 파일 하나**를 쓴다 (llm_l5b 와 같은 이유).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")

# 사다리 공용 Kconfig 조각 — uart2 인터럽트 폭주를 끈다 (사이클의 77%).
# 근거와 측정값은 그 파일에 있다. ⚠ prj.conf **뒤에** 병합되므로 그 파일이 이긴다.
set(EXTRA_CONF_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ladder.conf")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l7b)

# ── ggml·llama 소스만 -O3 로 (2026-11-07 밤) ────────────────────────────
#
# Zephyr Kconfig 에는 -O3 선택지가 없다 (SIZE/-Os · SPEED/-O2 · …).
# 업스트림 llama.cpp 는 Release 에서 -O3 를 쓴다. 전역 Kconfig 는 그대로
# 두고(이 앱은 -O2) **ggml·llama 소스와 main 에만** 뒤에 붙여 덮는다
# (gcc 는 마지막 -O 를 쓴다). 커널에는 주지 않는다.
#
# ⚠ include **전에** 세워야 한다 — llm_ggml.cmake 의 set(... CACHE ...) 는
#   FORCE 가 없어 이미 있는 값을 이기지 못한다.
# ⚠ 되돌리기는 이 줄을 지우는 것이다. 그러면 Kconfig 값(-O2)으로 돌아간다.
set(LLM_GGML_OPT_LEVEL "-O3" CACHE STRING "" FORCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_llama.cmake)
llm_add_ggml()
llm_add_llama(L6B)

target_sources(app PRIVATE
	src/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
