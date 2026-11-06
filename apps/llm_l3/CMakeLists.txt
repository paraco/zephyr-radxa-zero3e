cmake_minimum_required(VERSION 3.20.0)
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
