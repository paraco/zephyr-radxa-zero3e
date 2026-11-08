cmake_minimum_required(VERSION 3.20.0)
# L9c — llm_heap 후보 구역(0x10000000 + 768 MiB) 검증 + DRAM 대역폭.
# 모델 창 overlay 도 같이 넣어 블롭 구역의 읽기 대역폭도 잰다.
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay;${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/llm_heap.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l9c)
target_sources(app PRIVATE src/main.c)
