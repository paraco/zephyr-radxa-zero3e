cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l5a)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
llm_add_ggml()

target_sources(app PRIVATE
	src/main.c
	src/l5_graph.c
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)

# 그래프 파일도 ggml 과 **같은 -mcpu** 로 짓는다. 여기서 기능 매크로를 읽어
# 호스트와 비교하므로, 이 파일만 플래그가 다르면 비교가 거짓말이 된다.
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/l5_graph.c)
