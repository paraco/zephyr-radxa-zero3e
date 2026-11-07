cmake_minimum_required(VERSION 3.20.0)
# 모델 가중치 창은 **공용 파일 하나**를 쓴다 — 앱마다 복사해 두면 주소를
# 고칠 때 갈린다 (실제로 실기 DRAM 끝을 넘는 주소가 여섯 군데에 퍼져 있었다).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")

# 사다리 공용 Kconfig 조각 — uart2 인터럽트 폭주를 끈다 (사이클의 77%).
# 근거와 측정값은 그 파일에 있다. ⚠ prj.conf **뒤에** 병합되므로 그 파일이 이긴다.
set(EXTRA_CONF_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ladder.conf")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l4)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
llm_add_ggml()

target_sources(app PRIVATE
	src/main.c
	src/l4_graph.c
	${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
target_include_directories(app PRIVATE src)

# 그래프 파일도 ggml 과 **같은 -mcpu** 로 짓는다. 여기서 기능 매크로를
# 읽어 호스트와 비교하므로, 이 파일만 플래그가 다르면 비교가 거짓말이 된다.
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/l4_graph.c)
