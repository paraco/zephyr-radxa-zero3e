cmake_minimum_required(VERSION 3.20.0)
# 모델 가중치 창은 **공용 파일 하나**를 쓴다 (llm_l5b 와 같은 이유).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")

# 사다리 공용 Kconfig 조각 — uart2 인터럽트 폭주를 끈다 (사이클의 77%).
# 근거와 측정값은 그 파일에 있다. ⚠ prj.conf **뒤에** 병합되므로 그 파일이 이긴다.
set(EXTRA_CONF_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ladder.conf")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l6b)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_llama.cmake)
llm_add_ggml()
llm_add_llama(L6B)

target_sources(app PRIVATE
	src/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
