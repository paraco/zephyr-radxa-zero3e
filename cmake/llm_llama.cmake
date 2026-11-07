# llm_llama.cmake — llama.cpp src/ 의 일부를 Zephyr 앱에 붙인다.
#
# 쓰는 쪽:
#     include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_llama.cmake)
#     llm_add_ggml()          # 먼저 — llama 가 ggml 에 기댄다
#     llm_add_llama(L6A)      # 묶음 이름
#
# ⚠ 상류 수정 0건을 지킨다. 수단은 셋이다:
#     ① 강제 포함 ggml_zephyr_compat.h — 없는 선언을 채운다
#     ② 강제 포함 zephyr_future.h      — libstdc++ 가 비워 둔 <future> 를 채운다
#     ③ 대체 파일                       — llama-mmap.cpp · ggml-backend-reg.cpp
#
# ⚠ ①②는 **ggml 쪽과 공유한다.** llm_ggml.cmake 가 ggml 소스에만 걸면
#   llama 소스가 다른 전제로 컴파일된다.

set(LLM_LLAMA_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(llm_add_llama GROUP)
	set(LLAMA_DIR "${LLM_LLAMA_CMAKE_DIR}/../../upstream/llama.cpp"
	    CACHE PATH "llama.cpp 체크아웃")
	if(NOT EXISTS "${LLAMA_DIR}/src/llama-model-loader.cpp")
		message(FATAL_ERROR "llama.cpp src 없음: ${LLAMA_DIR}")
	endif()

	# ⚠ ENCODING UTF-8 — 없으면 한글 주석이 줄로 쪼개져 목록이 망가진다
	#   (ggml-sources.txt 에서 23개가 108개로 늘어난 적이 있다).
	file(STRINGS "${LLM_LLAMA_CMAKE_DIR}/llama-sources.txt" _lines ENCODING UTF-8)
	set(_srcs "")
	foreach(line ${_lines})
		string(STRIP "${line}" line)
		if(line STREQUAL "" OR line MATCHES "^#")
			continue()
		endif()
		if(line MATCHES "^${GROUP} +(.+)$")
			list(APPEND _srcs "${LLAMA_DIR}/${CMAKE_MATCH_1}")
		endif()
	endforeach()
	list(LENGTH _srcs _n)
	if(_n EQUAL 0)
		message(FATAL_ERROR
			"llama-sources.txt 에 '${GROUP}' 묶음이 없다 — 이름을 확인해라")
	endif()

	# 대체 파일 — 상류를 고치지 않기 위한 수단 ③
	set(_common "${LLM_LLAMA_CMAKE_DIR}/../apps/llm_common")
	list(APPEND _srcs
		"${_common}/llama_mmap_zephyr.cpp"
		"${_common}/ggml_backend_reg_zephyr.cpp"
		# ⚠ 상류가 src/llama.cpp 에 정의한 두 함수를 대신 채운다. 그 파일을
		#   넣으면 src/models/*.cpp 175개가 따라온다 (파일 머리말 참고).
		"${_common}/llama_split_zephyr.cpp")

	message(STATUS "llama 소스 ${_n}개(${GROUP}) + 대체판 3개")

	target_sources(app PRIVATE ${_srcs})
	target_include_directories(app PRIVATE
		${LLAMA_DIR}/src ${LLAMA_DIR}/include ${_common})
	# 로더가 version 헤더를 보지 않지만 llama.h 가 볼 수 있다
	target_compile_definitions(app PRIVATE LLAMA_SHARED=)

	# ⚠ ggml 과 **같은** 플래그를 준다. 한쪽만 다르면 비트 비교가 깨진다.
	llm_ggml_flags(${_srcs})
endfunction()
