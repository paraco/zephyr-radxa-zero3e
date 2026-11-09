#!/usr/bin/env bash
# Edge 쪽(Cortex-M33, 32비트·단정도 FPU) 앱을 QEMU mps2-an521 에서 빌드·실행·판정한다.
#   ./tools/qemu_edge_run.sh apps/hubml_edge_q [제한초]
# Teensy 4.1(Cortex-M7)·RP2350·nRF54L15(M33) 의 대리 — 명령어 집합·float32·32비트 포인터를 같게 맞춘다.
# 실제 MCU 속도는 재지 못한다 (QEMU 는 사이클 정확하지 않다). 판정: "=== Lx PASS|FAIL"
set -uo pipefail
APP=${1:?사용법: $0 apps/hubml_edge_q [제한초]}; LIMIT=${2:-60}
OUT=/tmp/claude-501/q-$(basename "${APP%/}"); LOG=$OUT.log
export ZEPHYR_BASE="${PIZZA_ZEPHYR_WS:-$HOME/zephyrproject}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR="${PIZZA_ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
[[ -f "$HOME/.zephyr-venv/bin/activate" ]] && source "$HOME/.zephyr-venv/bin/activate"
mkdir -p /tmp/claude-501
west build -p always -b mps2/an521/cpu0 -d "$OUT" "$APP" >"$OUT.build.log" 2>&1 || { tail -30 "$OUT.build.log" >&2; exit 3; }
qemu-system-arm -cpu cortex-m33 -machine mps2-an521 -nographic -m 16 -vga none -no-reboot \
	-kernel "$OUT/zephyr/zephyr.elf" >"$LOG" 2>&1 &
PID=$!; trap 'kill -9 $PID 2>/dev/null' EXIT INT TERM
for ((i = 0; i < LIMIT; i++)); do grep -qE '^=== L[0-9a-z]+ (PASS|FAIL)' "$LOG" 2>/dev/null && break; sleep 1; done
cat "$LOG"
grep -qE '^=== L[0-9a-z]+ PASS' "$LOG" && exit 0
grep -qE '^=== L[0-9a-z]+ FAIL' "$LOG" && exit 1
echo "⚠ 판정 문장이 없다" >&2; exit 2
