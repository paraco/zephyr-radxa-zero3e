cmake_minimum_required(VERSION 3.20.0)
# ⚠ **이 앱은 apps/llm_common/ladder.conf 를 쓰지 않는다.** 그 파일이
#   CONFIG_UART_INTERRUPT_DRIVEN=n 을 넣는데, 이 앱은 그 인터럽트 폭주를
#   **진단**하는 것이 목적이라 폭주가 살아 있어야 한다. prj.conf 에서
#   =y 로 명시한다 (ladder.conf 가 prj.conf 를 덮으므로 여기서 빼야 한다).
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l8j)
target_sources(app PRIVATE src/main.c)
