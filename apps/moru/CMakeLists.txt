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

# ⚠ 1단계 격리 시험 중이다. main.c 만 빌드한다.
#
#   src/moru_stage.c 는 빼둔다 — PRE_KERNEL_1 에서 device_map() 을 불렀고
#   그건 불법이다. kernel/init.c 를 보면 z_mem_manage_init()(:293)이
#   PRE_KERNEL_1(:560)·PRE_KERNEL_2(:564) 보다 뒤, POST_KERNEL(:300) 바로
#   앞에서 돈다. 즉 메모리 관리자가 초기화되기 전에 매핑을 요청한 것이다.
#   진단 장치가 피검체를 죽였다. 고쳐서 되살리려면 POST_KERNEL 이후로
#   옮겨야 하고, 그러면 PRE_KERNEL 단계는 표시할 수 없다.
#
#   src/moru_boot.c 도 빼둔다 — PRE_KERNEL_1/0 에서 로그 타임스탬프를
#   바꾸므로 역시 후보다. 셸이 먼저 뜨는 것을 확인한 뒤 되돌려 넣는다.
target_sources(app PRIVATE src/main.c)
# target_sources(app PRIVATE src/moru_boot.c src/moru_stage.c)
