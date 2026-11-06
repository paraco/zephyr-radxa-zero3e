#!/usr/bin/env bash
# L4 골든값 — 호스트에서 apps/llm_l4/src/l4_graph.c 를 그대로 돌린다.
#
#   ./tools/l4_golden.sh
#
# llama.cpp 를 cmake 로 짓지 않는다. 상류 소스 목록을 그대로 한 번에 묶는다
# (apps/llm_l4/CMakeLists.txt 와 같은 목록 — 한쪽만 바뀌면 결과가 갈린다).
#
# ⚠ i8mm 을 끈다. 우리 보드(Cortex-A55)와 QEMU -cpu cortex-a55 에는 i8mm 이
#   없다. 호스트(Apple M 계열)에는 있어 ggml 이 **실기에 없는 커널 경로**를
#   골라 Q4_0 행렬곱 결과가 달라질 수 있다. dotprod 는 양쪽 다 있으므로 켠다.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
LLAMA="${LLAMA_DIR:-$(dirname "$REPO")/upstream/llama.cpp}"
G="$LLAMA/ggml"
WORK=/tmp/claude-501/l4
OUT="$REPO/apps/llm_l4/src/l4_golden.h"
APPSRC="$REPO/apps/llm_l4/src"

[[ -f "$G/src/ggml.c" ]] || { echo "ggml 소스 없음: $LLAMA" >&2; exit 1; }
mkdir -p "$WORK/gen"

# ggml-version.h 는 cmake 생성 파일이다 (ggml/src/CMakeLists.txt:216)
VER=$(git -C "$LLAMA" describe --tags --always 2>/dev/null || echo unknown)
COMMIT=$(git -C "$LLAMA" rev-parse HEAD 2>/dev/null || echo unknown)
sed -e "s/@GGML_VERSION@/$VER/" -e "s/@GGML_BUILD_COMMIT@/$COMMIT/" \
	"$G/src/ggml-version.h.in" > "$WORK/gen/ggml-version.h"

# 소스 목록은 **단일 출처**다 — cmake/ggml-sources.txt.
# Zephyr 쪽 cmake/llm_ggml.cmake 도 같은 파일을 읽는다. 목록이 갈리면
# 골든값과 Zephyr 결과가 **다른 코드**에서 나온다. 실제로 한쪽에만 tiled/ 를
# 넣어 링크가 깨진 적이 있다.
SRCS=()
while IFS= read -r line; do
	line="${line%%$'\r'}"
	[[ -z "$line" || "$line" == \#* ]] && continue
	if [[ "$line" == "HOST_ONLY "* ]]; then
		SRCS+=("$G/${line#HOST_ONLY }")   # 호스트에는 std::mutex 가 있다
	else
		SRCS+=("$G/$line")
	fi
done < "$REPO/cmake/ggml-sources.txt"
echo "── ggml 소스 ${#SRCS[@]}개 (cmake/ggml-sources.txt)"


# 실기에 맞춘 기능 집합: dotprod 있고 i8mm 없다.
FEAT=(-Xclang -target-feature -Xclang -i8mm)

echo "── 호스트 도구 컴파일 (i8mm 끔)"
# ⚠ 확장자별로 따로 컴파일한다. c++ 드라이버에 .c 를 섞으면 -x none 으로도
#   C++ 로 끌려가 "const void* 를 typed 포인터로 초기화 못 한다" 가 쏟아진다.
INC=(-I"$G/include" -I"$G/src" -I"$G/src/ggml-cpu" -I"$WORK/gen" -I"$APPSRC")
DEF=(-DGGML_USE_CPU -DGGML_BACKEND_BUILD)
OBJS=()
rm -f "$WORK"/obj/*.o 2>/dev/null || true
mkdir -p "$WORK/obj"
n=0
for s in "$HERE/l4_golden.c" "$APPSRC/l4_graph.c" "${SRCS[@]}"; do
	n=$((n + 1))
	o="$WORK/obj/$n-$(basename "$s").o"
	case "$s" in
	*.cpp) cc_cmd=(c++ -std=c++17) ;;
	*)     cc_cmd=(cc  -std=c17)   ;;
	esac
	"${cc_cmd[@]}" -O2 -c -o "$o" "$s" "${INC[@]}" "${DEF[@]}" "${FEAT[@]}" \
		2>&1 | grep -E "error" | head -3 || true
	[[ -f "$o" ]] || { echo "컴파일 실패: $s" >&2; exit 1; }
	OBJS+=("$o")
done
c++ -O2 -o "$WORK/l4_golden" "${OBJS[@]}"

[[ -x "$WORK/l4_golden" ]] || { echo "링크 실패" >&2; exit 1; }
"$WORK/l4_golden" > "$OUT"
echo "── 골든 헤더: $OUT  ($(grep -c . "$OUT") 줄)"
echo "   llama.cpp: $(git -C "$LLAMA" rev-parse --short HEAD)"
