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
project(llm_l3)

# ggml 소스를 **그대로** 가져온다. 계획의 B안(ggml CPU 백엔드 포팅)에서
# 핵심 질문은 "ggml 의 C 가 Zephyr 로 그대로 빌드되는가" 다. 재구현하면
# 그 질문을 피해 가는 것이고, 재구현의 버그를 ggml 의 버그로 착각하게 된다.
set(LLAMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../upstream/llama.cpp"
    CACHE PATH "llama.cpp 체크아웃")
if(NOT EXISTS "${LLAMA_DIR}/ggml/src/ggml-quants.c")
  message(FATAL_ERROR "ggml 소스를 못 찾았다: ${LLAMA_DIR}\n"
                      "  git clone --depth 1 https://github.com/ggml-org/llama.cpp "
                      "${LLAMA_DIR}")
endif()

target_sources(app PRIVATE src/main.c src/ggml_stubs.c
                           ${LLAMA_DIR}/ggml/src/ggml-quants.c)
target_include_directories(app PRIVATE
	${LLAMA_DIR}/ggml/include
	${LLAMA_DIR}/ggml/src
	${LLAMA_DIR}/ggml/src/ggml-cpu)
