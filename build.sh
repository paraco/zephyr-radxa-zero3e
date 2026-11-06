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
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$SDK/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-"

[[ -x "${CROSS_COMPILE}gcc" ]] || { echo "aarch64 툴체인 없음: ${CROSS_COMPILE}gcc" >&2; exit 1; }
# shellcheck disable=SC1091
source "$VENV/bin/activate"

cd "$ZEPHYR_WS"
EXTRA_ARGS=()
if [[ -n "$EXTRA_OVERLAY" ]]; then
  [[ -f "$EXTRA_OVERLAY" ]] || { echo "오버레이 파일 없음: $EXTRA_OVERLAY" >&2; exit 1; }
  EXTRA_ARGS+=("-DEXTRA_DTC_OVERLAY_FILE=$EXTRA_OVERLAY")
  echo "추가 오버레이: $EXTRA_OVERLAY"
fi

west build -p always -b radxa_zero_3e/rk3568 -d "$BUILD_DIR" -s "$APP" \
  -- -DBOARD_ROOT="$HERE" -DDTS_ROOT="$HERE" -DZEPHYR_EXTRA_MODULES="$HERE" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo
echo "산출물:"
ls -la "$BUILD_DIR/zephyr/zephyr.bin" "$BUILD_DIR/zephyr/zephyr.elf" | awk '{printf "  %-50s %s bytes\n",$NF,$5}'
echo "진입 주소:"
"${CROSS_COMPILE}readelf" -h "$BUILD_DIR/zephyr/zephyr.elf" | awk '/Entry point/{print "  "$0}'
echo
echo "⚠ 실기 적재는 아직 미확정입니다 — U-Boot 가 go 할 주소와 sram0 가 맞아야 합니다."
echo "  보드에 올리는 작업은 사람이 합니다."
