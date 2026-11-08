# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
# hubllm = moru + llama. 모델 창(0x58000000)은 L6c 와 같은 공용 overlay.
set(EXTRA_DTC_OVERLAY_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/model_blob.overlay")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hubllm)

# 빌드 시각. configure 시점에 정해진다 — build.sh 가 -p always 라 매번 갱신되지만,
# ninja 만 직접 돌리면 옛 시각이 남는다.
# __DATE__/__TIME__ 대신 CMake 를 쓰는 이유는 재현 빌드다. ccache 영향은
# 어느 쪽이든 이 두 파일 재컴파일로 같다.
string(TIMESTAMP MORU_BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)
target_compile_definitions(app PRIVATE MORU_BUILD_TIME="${MORU_BUILD_TIME} UTC")

#
# ⚠ 작업 트리가 더러운지(커밋 전 변경이 있는지) 박는다.
#
# 왜 필요한가: APP_BUILD_VERSION 은 `git describe` 에 **--dirty 가 없다.**
# 그래서 **커밋 전 트리로 빌드한 이미지가 HEAD 해시로 찍힌다.** 2026-11-07 에
# 실기에서 "8 MiB 상한이 없는 커밋(54da428)" 으로 보이는 이미지가 실제로는
# 상한을 갖고 있어 한참 혼동했다.
#
# 왜 여기서 하는가: Zephyr 의 app_version.h 는 **별도 cmake 스크립트 호출**로
# 생성되고 최상위 -DAPP_BUILD_VERSION 이 그 호출에 전달되지 않는다
# (cmake/gen_version_h.cmake 는 자기 인자만 본다). 게다가 그 매크로는
# **따옴표 없는 토큰**이라 `-dirty` 같은 하이픈을 담기에도 부적합하다.
# 그래서 문자열로 직접 주입한다.
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} status --porcelain
                OUTPUT_VARIABLE MORU_GIT_STATUS
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(MORU_GIT_STATUS STREQUAL "")
  set(MORU_GIT_DIRTY "")
else()
  set(MORU_GIT_DIRTY "-dirty")
endif()
target_compile_definitions(app PRIVATE MORU_GIT_DIRTY="${MORU_GIT_DIRTY}")

# 보드 모델은 moru_boot.c 의 고정 문자열이다.
# DT 루트의 model 속성은 바인딩에 없어 매크로가 생성되지 않고(DT_PROP 불가)
# dt_prop() 도 비어 돌아온다. zephyr.dts 를 정규식으로 긁는 방법은 되지만,
# 이 앱은 이미 보드 전용이고(GPIO0 주소·SoC 문구가 하드코딩) 이 값은 배너
# 장식 한 줄이라 그 취약함을 안고 갈 값이 아니다.
# → 그래서 src/moru.h 의 MORU_MODEL 고정 문자열로 둔다.

# 소스 네 개를 모두 빌드한다 (2026-11-06, B1·B2 완료 상태).
#   main.c       LED 하트비트 + main
#   moru_mark.c  초기 부팅 동기 마커 — uart2 에 직접 (USB 가 무음일 때의 눈)
#   moru_boot.c  Linux dmesg 형식 부팅 메시지 + 로그 타임스탬프 기준점
#   moru_shell.c `moru` 명령 (info · reboot)
#
# ⚠ 과거에 moru_stage.c 가 있었고 2026-11-06 에 삭제했다 — PRE_KERNEL_1 에서
#   device_map() 을 불러 보드를 죽이는 파일이었다. kernel/init.c 의
#   z_mem_manage_init()(:293)이 PRE_KERNEL_1(:560)·PRE_KERNEL_2(:564) 보다
#   뒤, POST_KERNEL(:300) 바로 앞에서 돈다. 진단 장치가 피검체를 죽인
#   사례이고 (docs/bringup-rules.md 규칙 5), 남겨 두면 다음 사람이 빌드에
#   넣고 같은 함정을 밟으므로 지웠다.
# drivers/hwinfo/hwinfo_rk3566.h 를 "hwinfo_rk3566.h" 로 쓸 수 있게 한다.
# (저장소 루트는 CMakeLists.txt 의 두 단계 위)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/hwinfo
  # udc_dwc3_regs.h — moru usb 가 dwc3 레지스터를 직접 읽는다. 드라이버
  # 소스는 건드리지 않고 레지스터 정의만 재사용하므로 중복 정의가 없다.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/usb/udc)

target_sources(app PRIVATE src/main.c src/moru_mark.c src/moru_boot.c
                           src/moru_shell.c src/moru_usbd.c
                           src/moru_htop.c src/moru_fatal.c
                           src/hub_llm.cpp src/hub_shell.c src/sha256.c)

# ── llama (L6c 와 같은 묶음 · ggml 은 -O3) ─────────────────────────────
set(LLM_GGML_OPT_LEVEL "-O3" CACHE STRING "" FORCE)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_ggml.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llm_llama.cmake)
llm_add_ggml()
llm_add_llama(L6B)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../llm_common/ggml_port.c)
llm_ggml_flags(${CMAKE_CURRENT_SOURCE_DIR}/src/hub_llm.cpp)
