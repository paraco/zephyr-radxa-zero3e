cmake_minimum_required(VERSION 3.20.0)
# 모델 가중치 창은 **공용 파일 하나**를 쓴다 (llm_l5b 와 같은 이유).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l6c)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_llama.cmake)
llm_add_ggml()
llm_add_llama(L6B)

target_sources(app PRIVATE
	src/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
