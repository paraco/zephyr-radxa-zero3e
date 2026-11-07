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

#
# 부동소수 축약(FMA) 정책.
#
# gcc 와 clang 이 `sumf += a*b` 를 fmadd 로 **묶는 기준이 다르다.** 묶으면
# 중간 반올림이 한 번 줄어 결과가 1 ulp 달라진다. ggml 의 평범한 C 루프
# (예: ggml_compute_forward_ssm_conv_f32 — intrinsic 이 아니라 스칼라 루프다)
# 에서 이것이 드러난다. 상쇄가 있는 합에서는 상대오차로 크게 보인다.
#
# `off` 로 두면 호스트와 Zephyr 가 같은 반올림을 하여 **비트 비교가 가능**해진다.
# 대가는 FMA 를 못 쓰는 것 — 정확성 사다리에서는 재현성이 더 중요하다.
# 성능 측정(L8)에서는 `fast` 로 바꿔 다시 잰다.
set(LLM_GGML_FP_CONTRACT "off" CACHE STRING "ggml 소스의 -ffp-contract (off=재현성, fast=성능)")

#
# repack — 가중치를 Q4_X_X 로 **다시 배치**해 블록 GEMM 커널을 타는 기능.
#
# ⚠ 상류 기본값은 **ON** 이다 (ggml/CMakeLists.txt:152). 우리는 **끈다.**
#   근거는 docs/l6-decisions.md 결정 1 에 있다. 요약:
#     ① 메모리 — 재배치 **사본**이 별도 버퍼 타입에 생긴다. 모델 창 512 MiB 에
#        모델이 507.85 MiB 로 여유가 4.15 MiB 다. 사본이 들어갈 자리가 없고,
#        제자리 적재(결정 3)와 정면으로 충돌한다.
#     ② 한 왕복에 변수 하나 — repack 은 가중치 배치와 matmul 커널을 **동시에**
#        바꾼다. L6·L7 은 모델 그래프를 가리는 칸이다. L8 의 소재다.
#
#   (처음엔 「호스트 i8mm vs 실기 dotprod」를 근거로 들었으나 **틀렸다.**
#    tools/host_golden.sh 가 이미 호스트의 i8mm 을 끄고 있어 양쪽이 같은
#    dotprod 경로다. 그 근거는 철회했다 — 결정 1 참고.)
#
# ⚠ repack.cpp · arch/arm/repack.cpp 는 cmake/ggml-sources.txt 에 **들어 있다.**
#   매크로가 없으면 코드는 링크되지만 ggml-cpu.cpp:64 가 버퍼 타입을 등록하지
#   않아 쓰이지 않는다. 즉 끄는 것은 소스 목록이 아니라 **이 매크로**다.
#
# ⚠ 호스트와 **반드시 같아야 한다.** tools/host_golden.sh 가 LLM_GGML_REPACK 를
#   환경변수로 읽는다 — 한쪽만 바꿀 수 없게 해 두었다. ggml-sources.txt 를
#   단일 출처로 만든 것과 같은 이유다(한쪽만 바뀌어 호스트 골든값과 Zephyr
#   결과가 다른 코드에서 나온 적이 있다).
option(LLM_GGML_REPACK "ggml repack (Q4_0 -> Q4_X_X 재배치) — L8 성능 측정에서만 ON" OFF)

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
	if(LLM_GGML_REPACK)
		target_compile_definitions(app PRIVATE GGML_USE_CPU_REPACK)
		message(WARNING
			"ggml repack ON — 호스트 골든값도 LLM_GGML_REPACK=1 로 다시 만들어라.\n"
			"  모델 창 여유 4.15 MiB 로는 재배치 사본이 들어가지 않는다 (docs/l6-decisions.md).")
	else()
		message(STATUS "ggml repack OFF (상류 기본값은 ON — docs/l6-decisions.md 결정 1)")
	endif()
	# 호환 층을 모든 소스에 강제 포함한다 — 상류를 고치지 않기 위한 수단이다.
	# ⚠ 강제 포함은 **둘** 이다. 둘 다 "상류를 고치지 않는" 수단이다.
	#   ggml_zephyr_compat.h — 없는 선언(sysconf · _SC_PHYS_PAGES · M_PI)
	#   zephyr_future.h      — libstdc++ 가 _GLIBCXX_HAS_GTHREADS 없이 빌드돼
	#                          비워 둔 <future> (docs/l6a-compile-matrix.md)
	#   ⚠ ggml 소스에만 걸면 llama 소스가 다른 전제로 컴파일된다. 둘 다
	#     app 타깃 전체에 건다.
	#
	# ⚠ **`SHELL:` 가 반드시 필요하다.** cmake 의 target_compile_options 는
	#   같은 플래그를 **중복 제거**한다. 그냥 `-include a.h -include b.h` 로
	#   주면 두 번째 `-include` 가 지워져 `-include a.h b.h` 가 되고, gcc 는
	#   b.h 를 **입력 파일**로 읽는다:
	#       fatal error: cannot specify '-o' with '-c' … with multiple files
	#   실제로 겪었다(L6a 첫 빌드). `SHELL:` 는 그 안을 한 덩이로 보내
	#   중복 제거를 막는다.
	target_compile_options(app PRIVATE
		"SHELL:-include ggml_zephyr_compat.h"
		"SHELL:-include zephyr_future.h")

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
			COMPILE_OPTIONS "-mcpu=${LLM_GGML_MCPU}"
			                "-ffp-contract=${LLM_GGML_FP_CONTRACT}")
	endforeach()
endfunction()
