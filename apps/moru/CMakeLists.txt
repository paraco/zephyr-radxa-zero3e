# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(moru)

# 빌드 시각. configure 시점에 정해진다 — build.sh 가 -p always 라 매번 갱신되지만,
# ninja 만 직접 돌리면 옛 시각이 남는다.
# __DATE__/__TIME__ 대신 CMake 를 쓰는 이유는 재현 빌드다. ccache 영향은
# 어느 쪽이든 이 두 파일 재컴파일로 같다.
string(TIMESTAMP MORU_BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)
target_compile_definitions(app PRIVATE MORU_BUILD_TIME="${MORU_BUILD_TIME} UTC")

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
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/hwinfo)

target_sources(app PRIVATE src/main.c src/moru_mark.c src/moru_boot.c
                           src/moru_shell.c src/moru_usbd.c)
