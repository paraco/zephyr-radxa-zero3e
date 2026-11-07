#!/usr/bin/env bash
# 사다리 앱 하나의 **호스트 골든값**을 만든다.
#
#   ./tools/host_golden.sh llm_l4
#   ./tools/host_golden.sh llm_l5a
#
# 규약 (앱마다 복제하지 않기 위해 고정한다):
#   그래프   apps/<앱>/src/<접두>_graph.c   ← Zephyr 와 **공유**하는 파일
#   도구     tools/<접두>_golden.c
#   산출물   apps/<앱>/src/<접두>_golden.h
#
# 그래프를 양쪽에 따로 쓰면 전사 오류를 "커널이 다르다" 로 오진한다.
# 그래서 호스트도 **같은 파일**을 컴파일한다.
#
# llama.cpp 를 cmake 로 짓지 않는다. cmake/ggml-sources.txt(단일 출처)를 읽어
# 확장자별로 컴파일한 뒤 링크한다. Zephyr 쪽 cmake/llm_ggml.cmake 도 같은
# 파일을 읽는다 — 목록이 갈리면 골든값과 결과가 **다른 코드**에서 나온다.
#
# ⚠ i8mm 을 끈다. 보드(Cortex-A55)와 QEMU -cpu cortex-a55 에는 i8mm 이 없다.
#   호스트(Apple M 계열)에는 있어 ggml 이 **실기에 없는 커널 경로**를 고른다.
#   dotprod 는 양쪽 다 있으므로 켠다.
set -euo pipefail

APP=${1:?사용법: ./tools/host_golden.sh <앱이름>   예: llm_l5a}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
LLAMA="${LLAMA_DIR:-$(dirname "$REPO")/upstream/llama.cpp}"
G="$LLAMA/ggml"
APPSRC="$REPO/apps/$APP/src"

[[ -d "$APPSRC" ]] || { echo "앱 없음: $APPSRC" >&2; exit 1; }
[[ -f "$G/src/ggml.c" ]] || {
	echo "ggml 소스 없음: $LLAMA" >&2
	echo "  git clone --depth 1 https://github.com/ggml-org/llama.cpp $LLAMA" >&2
	exit 1; }

# ⚠ `ls … | head -1` 를 쓰면 set -e -o pipefail 에서 실패 시 **조용히 종료**하고
#   아래 진단문에 닿지 못한다. glob 을 직접 쓴다.
shopt -s nullglob
GRAPHS=("$APPSRC"/*_graph.c)
shopt -u nullglob
if [[ ${#GRAPHS[@]} -eq 0 ]]; then
	echo "$APPSRC 에 *_graph.c 가 없다" >&2; exit 1
fi
if [[ ${#GRAPHS[@]} -gt 1 ]]; then
	echo "$APPSRC 에 *_graph.c 가 ${#GRAPHS[@]}개다 — 하나만 두어라:" >&2
	printf '  %s\n' "${GRAPHS[@]}" >&2; exit 1
fi
GRAPH="${GRAPHS[0]}"
PREFIX=$(basename "$GRAPH" _graph.c)
TOOL="$HERE/${PREFIX}_golden.c"
OUT="$APPSRC/${PREFIX}_golden.h"
[[ -f "$TOOL" ]] || { echo "도구 없음: $TOOL" >&2; exit 1; }

WORK=/tmp/claude-501/hg-$APP
mkdir -p "$WORK/gen" "$WORK/obj"
rm -f "$WORK"/obj/*.o

# ggml-version.h 는 cmake 생성 파일이다 (ggml/src/CMakeLists.txt:216)
VER=$(git -C "$LLAMA" describe --tags --always 2>/dev/null || echo unknown)
COMMIT=$(git -C "$LLAMA" rev-parse HEAD 2>/dev/null || echo unknown)
sed -e "s/@GGML_VERSION@/$VER/" -e "s/@GGML_BUILD_COMMIT@/$COMMIT/" \
	"$G/src/ggml-version.h.in" > "$WORK/gen/ggml-version.h"

SRCS=()
while IFS= read -r line || [[ -n "$line" ]]; do	# || 절: 마지막 줄에 개행이 없어도 읽는다
	line="${line%%$'\r'}"
	# cmake 쪽은 string(STRIP) 을 한다. 여기서도 앞뒤 공백을 지워야 같은 결과가 된다.
	line="${line#"${line%%[![:space:]]*}"}"
	line="${line%"${line##*[![:space:]]}"}"
	[[ -z "$line" || "$line" == \#* ]] && continue
	if [[ "$line" == "HOST_ONLY "* ]]; then
		SRCS+=("$G/${line#HOST_ONLY }")   # 호스트에는 std::mutex 가 있다
	else
		SRCS+=("$G/$line")
	fi
done < "$REPO/cmake/ggml-sources.txt"

INC=(-I"$G/include" -I"$G/src" -I"$G/src/ggml-cpu" -I"$WORK/gen" -I"$APPSRC"
     -I"$REPO/apps/llm_common")
DEF=(-DGGML_USE_CPU -DGGML_BACKEND_BUILD)
# ⚠ repack 은 **Zephyr 쪽과 반드시 같아야 한다.** 한쪽만 켜면 호스트 골든값이
#   다른 코드에서 나오고 그 사실이 드러나지 않는다. 기본은 끔 —
#   근거는 docs/l6-decisions.md 결정 1 (cmake/llm_ggml.cmake 의 LLM_GGML_REPACK).
if [ "${LLM_GGML_REPACK:-0}" != "0" ]; then
	DEF+=(-DGGML_USE_CPU_REPACK)
	echo "⚠ repack ON — Zephyr 도 -DLLM_GGML_REPACK=ON 으로 빌드해야 한다" >&2
fi
# ⚠ -ffp-contract 를 양쪽에서 똑같이 맞춘다. gcc 와 clang 이 `sumf += a*b` 를
#   fmadd 로 묶는 기준이 달라 1 ulp 가 갈리고, 상쇄가 있는 합에서는 상대오차로
#   크게 보인다. off 로 두면 비트 비교가 가능해진다.
#   Zephyr 쪽 기본값은 cmake/llm_ggml.cmake 의 LLM_GGML_FP_CONTRACT 다.
FP_CONTRACT=${LLM_FP_CONTRACT:-off}
FEAT=(-Xclang -target-feature -Xclang -i8mm "-ffp-contract=$FP_CONTRACT")

# ⚠ Zephyr 쪽 컴파일 명령과 **같은 조건**으로 맞춘다. 한쪽만 달라지면
#   "골든값과 결과가 다른 코드에서 나온" 상태가 되고 그 사실이 드러나지 않는다.
#
#   -fno-strict-aliasing : Zephyr 가 준다 (compile_commands.json 으로 확인).
#   -U__ARM_NEON__       : clang 은 __ARM_NEON__ 을 정의하고 aarch64 gcc 는
#                          **하지 않는다**. 상류 ggml-cpu/simd-gemm.h:12 가
#                          `defined(__AVX512F__) || defined(__ARM_NEON__)` 로
#                          타일 크기를 고르므로, 그대로 두면 호스트는 4x4,
#                          보드는 2x2 경로를 돈다. 지금 그래프는 flash-attn 을
#                          쓰지 않아 드러나지 않지만 L6 에서 터진다.
#                          (__ARM_NEON 은 양쪽 다 정의되므로 건드리지 않는다 —
#                           arm_neon.h 포함이 그것에 걸려 있다.)
FEAT+=(-fno-strict-aliasing -U__ARM_NEON__)

echo "── $APP : ggml ${#SRCS[@]}개 + $(basename "$GRAPH") + $(basename "$TOOL")"
# ⚠ 확장자별로 따로 컴파일한다. c++ 드라이버에 .c 를 섞으면 -x none 으로도
#   C++ 로 끌려가 "const void* 를 typed 포인터로 초기화 못 한다" 가 쏟아진다.
OBJS=(); n=0
for s in "$TOOL" "$GRAPH" "${SRCS[@]}"; do
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
c++ -O2 -o "$WORK/golden" "${OBJS[@]}" -lm
# ⚠ $OUT 에 바로 쓰면 생성기가 중간에 죽을 때 **커밋된 헤더가 잘린 채 남는다.**
#   임시 파일에 쓰고 성공했을 때만 옮긴다.
"$WORK/golden" > "$WORK/out.h"
mv "$WORK/out.h" "$OUT"
echo "── 골든 헤더: $OUT  ($(grep -c . "$OUT") 줄)"
echo "   llama.cpp: $(git -C "$LLAMA" rev-parse --short HEAD)"
