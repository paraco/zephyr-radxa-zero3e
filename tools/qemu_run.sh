#!/usr/bin/env bash
# QEMU 에서 사다리 앱 하나를 빌드해 돌리고 PASS/FAIL 판정까지 한다.
#
#   ./tools/qemu_run.sh apps/llm_l1 [제한초]
#
# -cpu cortex-a55 를 쓴다 — 우리 보드와 MIDR 이 같고(0x412fd050) dotprod 가
# 있으며 i8mm 이 없다. -cpu max 는 i8mm 이 있어 실기에 없는 커널 경로를
# 고르게 하므로 쓰지 않는다 (docs/llm-hub-test-plan.md).
#
# 판정: 출력에서 "=== Lx PASS" 를 찾으면 0, "=== Lx FAIL" 이면 1,
#       둘 다 없으면 2(제한시간 초과 또는 부팅 실패) — 조용한 죽음을 구분한다.
set -uo pipefail

APP=${1:?사용법: ./tools/qemu_run.sh apps/llm_l1 [제한초]}
LIMIT=${2:-180}
APP=${APP%/}
NAME=$(basename "$APP")
OUT=/tmp/claude-501/q-$NAME
LOG=$OUT.log

command -v qemu-system-aarch64 >/dev/null || { echo "qemu-system-aarch64 없음" >&2; exit 3; }

# build.sh 와 같은 규칙으로 작업공간·SDK·venv 를 잡는다
ZEPHYR_WS="${PIZZA_ZEPHYR_WS:-$HOME/zephyrproject}"
SDK="${PIZZA_ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
VENV="${PIZZA_ZEPHYR_VENV:-$HOME/.zephyr-venv}"
export ZEPHYR_BASE="$ZEPHYR_WS/zephyr"
# ⚠ QEMU 사다리는 **SDK 툴체인**으로 짓는다. cross-compile 로는
# TOOLCHAIN_HAS_GLIBCXX=n 이 되어 C++ 앱이 <vector> 를 못 찾는다
# (cmake/modules/kconfig.cmake:134). L0 에서 실제로 겪었다.
export ZEPHYR_TOOLCHAIN_VARIANT=${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-$SDK}
[[ -f "$VENV/bin/activate" ]] && source "$VENV/bin/activate"
mkdir -p /tmp/claude-501

echo "── 빌드: $APP → $OUT"
west build -p always -b qemu_cortex_a53 -d "$OUT" "$APP" >"$OUT.build.log" 2>&1 || {
	echo "빌드 실패 — 마지막 30줄:" >&2; tail -30 "$OUT.build.log" >&2; exit 3; }
echo "   ok  ($(grep -c . "$OUT.build.log") 줄 로그)"

QEMU_ARGS=(-cpu cortex-a55 -machine virt,secure=on,gic-version=3
	-m 2G -nographic -no-reboot -kernel "$OUT/zephyr/zephyr.elf")
# 블롭이 있으면 계획서가 고정한 0x60000000 에 넣는다 (실기 fatload 와 같은 주소)
if [[ -n "${L_BLOB:-}" ]]; then
	[[ -f $L_BLOB ]] || { echo "L_BLOB 파일 없음: $L_BLOB" >&2; exit 3; }
	QEMU_ARGS+=(-device "loader,file=$L_BLOB,addr=0x60000000,force-raw=on")
	echo "── 블롭: $L_BLOB → 0x60000000"
elif [[ -f "$APP/NEEDS_BLOB" ]]; then
	# 이 앱은 블롭을 **읽는다**. 없이 돌리면 전부 0 을 읽는데 역양자화 결과가
	# ±0.0 이라 그럴듯해 보여 "포팅이 틀렸다" 로 오진한다 — 실제로 겪었다.
	# 그래서 경고가 아니라 **실패**다.
	#
	# ⚠ 판정 기준이 app.overlay 의 model_blob 선언이 아니라 **표식 파일**이다.
	#   L1·L4·L5a·L5b 는 창을 선언하지만 읽지 않으므로 overlay 로 판정하면
	#   매번 거짓 경고가 뜬다. 오경보가 반복되면 진짜 경고도 묻힌다.
	echo "$NAME 은 블롭을 읽는데 L_BLOB 이 없다 — 전부 0 을 읽게 된다" >&2
	echo "   L_BLOB=<파일> $0 $APP" >&2
	exit 3
fi

echo "── 실행 (제한 ${LIMIT}s)"
# macOS 에 timeout 이 없을 수 있다 — 직접 감시한다.
# ⚠ trap 이 없으면 Ctrl-C 로 스크립트가 죽어도 비동기 QEMU 가 **살아남는다**
#   (SIGINT 가 무시된 채로 띄워진다). 어떤 경로로 끝나도 정리한다.
cleanup() { [[ -n "${PID:-}" ]] && kill -9 "$PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

qemu-system-aarch64 "${QEMU_ARGS[@]}" >"$LOG" 2>&1 &
PID=$!
for ((i = 0; i < LIMIT; i++)); do
	kill -0 $PID 2>/dev/null || break
	grep -qE '^=== L[0-9a-z]+ (PASS|FAIL)' "$LOG" 2>/dev/null && break
	sleep 1
done
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null

echo "────────── 출력 ──────────"
cat "$LOG"
echo "──────────────────────────"
echo "로그: $LOG"

if grep -qE '^=== L[0-9a-z]+ PASS' "$LOG"; then
	exit 0
elif grep -qE '^=== L[0-9a-z]+ FAIL' "$LOG"; then
	exit 1
else
	echo "⚠ 판정 문장이 없다 — 제한시간 초과이거나 부팅 전에 죽었다" >&2
	exit 2
fi
