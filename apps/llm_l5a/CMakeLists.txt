cmake_minimum_required(VERSION 3.20.0)
# 모델 가중치 창은 **공용 파일 하나**를 쓴다 — 앱마다 복사해 두면 주소를
# 고칠 때 갈린다 (실제로 실기 DRAM 끝을 넘는 주소가 여섯 군데에 퍼져 있었다).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")

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
