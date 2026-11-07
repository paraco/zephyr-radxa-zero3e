cmake_minimum_required(VERSION 3.20.0)
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l8h)
target_sources(app PRIVATE src/main.c)
