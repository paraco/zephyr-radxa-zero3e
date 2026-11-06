# ggml 을 Zephyr 앱에 붙인다. 사다리 L4 이후 앱이 include 한다.
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
#   llm_add_ggml()

set(LLM_GGML_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# 실기 보드(radxa_zero_3e/rk3568)는 Zephyr 가 -mcpu=cortex-a55 를 준다.
# QEMU 용 qemu_cortex_a53 보드는 -mcpu=cortex-a53 을 주는데 그것은 ARMv8.0 이라
# **__ARM_FEATURE_DOTPROD 가 꺼진다** — QEMU 를 -cpu cortex-a55 로 돌려도
# ggml 이 스칼라 경로를 고른다. 즉 사다리가 실기와 다른 커널을 시험한다.
# 그래서 ggml 소스에는 실기와 같은 -mcpu 를 명시한다.
set(LLM_GGML_MCPU "cortex-a55" CACHE STRING "ggml 소스에 줄 -mcpu (실기와 맞춘다)")

function(llm_add_ggml)
	set(LLAMA_DIR "${LLM_GGML_CMAKE_DIR}/../../upstream/llama.cpp"
	    CACHE PATH "llama.cpp 체크아웃")
	set(G "${LLAMA_DIR}/ggml")
	if(NOT EXISTS "${G}/src/ggml.c")
		message(FATAL_ERROR
			"ggml 소스 없음: ${LLAMA_DIR}\n"
			"  git clone --depth 1 https://github.com/ggml-org/llama.cpp ${LLAMA_DIR}")
	endif()

	# 소스 목록은 단일 출처다 (cmake/ggml-sources.txt). 호스트 골든 도구도
	# 같은 파일을 읽는다 — 목록이 갈리면 골든값과 결과가 다른 코드에서 나온다.
	#
	# ⚠ ENCODING UTF-8 이 **필수**다. 없으면 file(STRINGS) 가 ASCII 로 읽어
	#   한글 주석의 UTF-8 바이트를 구분자로 삼아 줄을 조각낸다 — 목록이
	#   23개가 아니라 108조각이 되고 "ggml/** 를 못 찾는다" 같은 엉뚱한
	#   오류로 나타난다. 실제로 겪었다.
	file(STRINGS "${LLM_GGML_CMAKE_DIR}/ggml-sources.txt" _lines ENCODING UTF-8)
	set(_srcs "")
	foreach(l ${_lines})
		string(STRIP "${l}" l)
		if(l STREQUAL "" OR l MATCHES "^#" OR l MATCHES "^HOST_ONLY ")
			continue()
		endif()
		list(APPEND _srcs "${G}/${l}")
	endforeach()
	list(LENGTH _srcs _n)
	message(STATUS "ggml 소스 ${_n}개 · -mcpu=${LLM_GGML_MCPU}")

	# ggml-version.h 는 cmake 생성 파일이다 (ggml/src/CMakeLists.txt:216)
	execute_process(COMMAND git -C ${LLAMA_DIR} describe --tags --always
	                OUTPUT_VARIABLE GGML_VERSION
	                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	execute_process(COMMAND git -C ${LLAMA_DIR} rev-parse HEAD
	                OUTPUT_VARIABLE GGML_BUILD_COMMIT
	                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	if(NOT GGML_VERSION)
		set(GGML_VERSION "unknown")
	endif()
	configure_file(${G}/src/ggml-version.h.in
	               ${CMAKE_CURRENT_BINARY_DIR}/ggml-gen/ggml-version.h @ONLY)

	target_sources(app PRIVATE ${_srcs})
	target_include_directories(app PRIVATE
		${G}/include ${G}/src ${G}/src/ggml-cpu
		${CMAKE_CURRENT_BINARY_DIR}/ggml-gen
		${LLM_GGML_CMAKE_DIR}/../apps/llm_common)
	target_compile_definitions(app PRIVATE GGML_USE_CPU GGML_BACKEND_BUILD)
	# 호환 층을 모든 소스에 강제 포함한다 — 상류를 고치지 않기 위한 수단이다.
	target_compile_options(app PRIVATE -include ggml_zephyr_compat.h)

	llm_ggml_flags(${_srcs})
endfunction()

#
# -mcpu 를 **소스 수준**으로 준다.
#
# ⚠ target_compile_options 로는 안 된다. Zephyr 의 -mcpu=cortex-a53 이
#   zephyr_interface 의 INTERFACE 옵션으로 **뒤에** 붙고 gcc 는 마지막 것을
#   쓴다. 실제로 겪었다 — 플래그를 줬는데 __ARM_FEATURE_DOTPROD 가 계속 0 이었다.
#   컴파일 명령의 순서는 [타깃 옵션] [인터페이스 옵션] [소스 옵션] 이라
#   소스 수준이 유일하게 이긴다(작은 프로젝트로 확인했다).
#
function(llm_ggml_flags)
	foreach(f ${ARGN})
		set_property(SOURCE ${f} APPEND PROPERTY
			COMPILE_OPTIONS "-mcpu=${LLM_GGML_MCPU}")
	endforeach()
endfunction()
