#!/usr/bin/env bash
#
# build.sh — Radxa ZERO 3E (RK3566) 용 Zephyr 애플리케이션 빌드
#
#   ./rk3566/build.sh                          # hello_world
#   ./rk3566/build.sh zephyr/samples/subsys/shell/shell_module
#   ./rk3566/build.sh <앱경로> <빌드디렉터리>
#   ./rk3566/build.sh <앱경로> <빌드디렉터리> <추가오버레이>
#
# 진단 빌드 (콘솔·셸을 UART2 로 되돌린다 — main 은 USB 다):
#   ./rk3566/build.sh "$PWD/apps/moru" /tmp/claude-501/rz3e-diag \
#                     "$PWD/apps/moru/diag-uart.overlay"
#
# 이 보드는 out-of-tree 라 BOARD_ROOT 를 넘겨야 한다. SoC 는 인트리 rk3568
# 정의를 재사용하므로 보드 식별자가 `radxa_zero_3e/rk3568` 이다.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEPHYR_WS="${PIZZA_ZEPHYR_WS:-$HOME/zephyrproject}"
SDK="${PIZZA_ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
VENV="${PIZZA_ZEPHYR_VENV:-$HOME/.zephyr-venv}"

APP="${1:-$ZEPHYR_WS/zephyr/samples/hello_world}"
BUILD_DIR="${2:-/tmp/claude-501/rz3e-build}"
EXTRA_OVERLAY="${3:-}"

export ZEPHYR_BASE="$ZEPHYR_WS/zephyr"
# ⚠ 툴체인 변종 — 호출자가 미리 정해 두면 그것을 존중한다.
#
#   기본은 cross-compile 이다. moru 앱이 그것으로 부팅하는 것이 확인돼 있어
#   기본값을 바꾸지 않는다.
#
#   그런데 **C++ 앱은 cross-compile 로 빌드되지 않는다.** 그 변종에서는
#   TOOLCHAIN_HAS_GLIBCXX 가 n 이 되어(cmake/modules/kconfig.cmake:134)
#   GLIBCXX_LIBCPP 의 의존성이 깨지고, 조용히 MINIMAL_LIBCPP 로 떨어져
#   <vector> 조차 못 찾는다. 실제로 llm_l0 에서 겪었다.
#
#   그래서 C++ 앱은 SDK 툴체인으로 부른다:
#     ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1 \
#       ./build.sh "$PWD/apps/llm_l0"
if [[ -z "${ZEPHYR_TOOLCHAIN_VARIANT:-}" ]]; then
  export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
fi
if [[ "$ZEPHYR_TOOLCHAIN_VARIANT" == "cross-compile" ]]; then
  export CROSS_COMPILE="$SDK/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-"
  [[ -x "${CROSS_COMPILE}gcc" ]] || { echo "aarch64 툴체인 없음: ${CROSS_COMPILE}gcc" >&2; exit 1; }
else
  export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$SDK}"
fi
echo "툴체인: $ZEPHYR_TOOLCHAIN_VARIANT"
# shellcheck disable=SC1091
source "$VENV/bin/activate"

cd "$ZEPHYR_WS"
EXTRA_ARGS=()
if [[ -n "$EXTRA_OVERLAY" ]]; then
  [[ -f "$EXTRA_OVERLAY" ]] || { echo "오버레이 파일 없음: $EXTRA_OVERLAY" >&2; exit 1; }
  EXTRA_ARGS+=("-DEXTRA_DTC_OVERLAY_FILE=$EXTRA_OVERLAY")
  echo "추가 오버레이: $EXTRA_OVERLAY"
  # 같은 이름의 .conf 가 있으면 함께 적용한다 (예: diag-uart.overlay +
  # diag-uart.conf). 진단 빌드는 셸 백엔드를 폴링으로 되돌려야 한다.
  EXTRA_CONF="${EXTRA_OVERLAY%.overlay}.conf"
  if [[ -f "$EXTRA_CONF" ]]; then
    EXTRA_ARGS+=("-DEXTRA_CONF_FILE=$EXTRA_CONF")
    echo "추가 설정:   $EXTRA_CONF"
  fi
fi

west build -p always -b radxa_zero_3e/rk3568 -d "$BUILD_DIR" -s "$APP" \
  -- -DBOARD_ROOT="$HERE" -DDTS_ROOT="$HERE" -DZEPHYR_EXTRA_MODULES="$HERE" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo
echo "산출물:"
ls -la "$BUILD_DIR/zephyr/zephyr.bin" "$BUILD_DIR/zephyr/zephyr.elf" | awk '{printf "  %-50s %s bytes\n",$NF,$5}'

# ── OTA 상한 여유 ──────────────────────────────────────────────────────
#
# ⚠ 왜 빌드 때 알려야 하나: sdcard/boot.cmd 의
#   `fatwrite … zephyr.bin 0x40000` 이 **고정 크기**다. 그래서 OTA 로 올릴
#   수 있는 이미지에 상한이 있다. 펌웨어의 `moru ota begin` 이 그 상한을
#   넘는 크기를 거부하므로 조용히 잘리지는 않는다
#   (apps/moru/src/moru_shell.c 의 MORU_OTA_MAX_SIZE).
#
#   문제는 **탈출구가 카드 작업**이라는 점이다. 상한을 올리려면 boot.cmd 를
#   고쳐 boot.scr 을 다시 만들어 **카드에 직접** 써야 한다 — OTA 로 피하려던
#   바로 그 일이다. 그러니 상한에 닿은 뒤에 알면 늦다. 여기서 미리 알린다.
#
#   U-Boot 가 크기를 DRAM 헤더에서 읽어 쓰면 상한이 필요 없지만, 이 U-Boot
#   에는 `setexpr` 가 없어 헤더의 값을 변수로 만들 수 없다. 그래서 고정이다.
OTA_CAP=$((256 * 1024))
BIN_SIZE=$(wc -c < "$BUILD_DIR/zephyr/zephyr.bin" | tr -d ' ')
PCT=$((BIN_SIZE * 100 / OTA_CAP))
printf "OTA 상한: %d / %d bytes (%d%%)" "$BIN_SIZE" "$OTA_CAP" "$PCT"
if [ "$BIN_SIZE" -gt "$OTA_CAP" ]; then
	printf "  ❌ 넘었다 — OTA 로 올릴 수 없다\n"
	echo "   boot.cmd 의 0x40000 을 키우고 boot.scr 을 다시 만들어 카드에 쓴다"
	echo "   (moru_shell.c 의 MORU_OTA_MAX_SIZE 도 같이 맞춘다 — 두 값이 일치해야 한다)"
elif [ "$PCT" -ge 90 ]; then
	printf "  ⚠ 여유가 거의 없다\n"
	echo "   상한을 올리려면 카드에 boot.scr 을 다시 써야 한다 — 미리 준비한다"
else
	printf "  여유 %d bytes\n" "$((OTA_CAP - BIN_SIZE))"
fi
echo "진입 주소:"
"${CROSS_COMPILE}readelf" -h "$BUILD_DIR/zephyr/zephyr.elf" | awk '/Entry point/{print "  "$0}'
echo
echo "⚠ 실기 적재는 아직 미확정입니다 — U-Boot 가 go 할 주소와 sram0 가 맞아야 합니다."
echo "  보드에 올리는 작업은 사람이 합니다."
