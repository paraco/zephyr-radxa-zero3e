cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l5b)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
llm_add_ggml()

target_sources(app PRIVATE
	src/main.c
	src/l5b_graph.c
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/l5b_graph.c)
