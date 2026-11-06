#!/usr/bin/env bash
# L3 골든값을 만든다 — **ggml 의 C 구현을 호스트에서 그대로 호출해서**.
#
#   ./tools/l3_golden.sh
#
# 파이썬 역양자화기(gguf-py/gguf/quants.py)를 쓰지 않는다. L3 의 허용 오차가
# 0(비트 일치)인데 파이썬과 C 는 연산 결합 순서가 달라 반올림이 달라질 수 있다.
# 골든값은 우리가 포팅하는 바로 그 코드에서 나와야 한다.
#
# llama.cpp 전체를 cmake 로 짓지 않는다 — ggml-quants.c 한 파일과 스텁 하나만
# 묶는다. 비어 있는 심볼 넷(ggml_abort / ggml_row_size / ggml_type_name /
# ggml_type_size)은 **양자화 경로만** 쓰는 것들이고, 스텁은 불리면 abort 한다.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
LLAMA="${LLAMA_DIR:-$(dirname "$REPO")/upstream/llama.cpp}"
GGUF="${1:-/tmp/claude-501/l2_test.gguf}"
WORK=/tmp/claude-501/l3
OUT="$REPO/apps/llm_l3/src/l3_golden.h"

[[ -f "$LLAMA/ggml/src/ggml-quants.c" ]] || {
	echo "ggml 소스 없음: $LLAMA" >&2
	echo "  git clone --depth 1 https://github.com/ggml-org/llama.cpp $LLAMA" >&2
	exit 1; }
[[ -f "$GGUF" ]] || { echo "GGUF 없음: $GGUF — ./.venv/bin/python tools/l2_make_gguf.py" >&2; exit 1; }

mkdir -p "$WORK"
cat > "$WORK/stubs.c" <<'STUB'
/* ggml-quants.c 의 양자화 경로만 쓰는 심볼들. 역양자화는 쓰지 않는다.
 * 불려도 조용히 틀린 값이 나오지 않게 큰 소리로 죽는다. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "ggml.h"
void ggml_abort(const char *file, int line, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	fprintf(stderr, "ggml_abort %s:%d: ", file, line);
	vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
	va_end(ap); abort();
}
static void nope(const char *w) { fprintf(stderr, "스텁이 불렸다: %s — 역양자화 경로가 아니다\n", w); abort(); }
size_t ggml_row_size(enum ggml_type t, int64_t n) { (void)t; (void)n; nope("ggml_row_size"); return 0; }
const char *ggml_type_name(enum ggml_type t) { (void)t; nope("ggml_type_name"); return ""; }
size_t ggml_type_size(enum ggml_type t) { (void)t; nope("ggml_type_size"); return 0; }
STUB

echo "── 호스트 도구 컴파일"
cc -O2 -std=c17 -o "$WORK/l3_golden" \
	-I"$LLAMA/ggml/include" -I"$LLAMA/ggml/src" -I"$LLAMA/ggml/src/ggml-cpu" \
	"$HERE/l3_golden.c" "$LLAMA/ggml/src/ggml-quants.c" "$WORK/stubs.c"

# 오프셋은 l2_golden.h 에서 읽는다 — 손으로 적지 않는다
G="$REPO/apps/llm_l2/src/l2_golden.h"
dat=$(grep -oE 'L2_DATA_OFFSET +[0-9]+' "$G" | awk '{print $2}')
q40=$(grep -oE '"t\.q4_0", [^}]*\}, [0-9]+u, [0-9]+u' "$G" | grep -oE '[0-9]+u *\}?$' | tr -d 'u ')
q4k=$(grep -oE '"t\.q4_k", [^}]*\}, [0-9]+u, [0-9]+u' "$G" | grep -oE '[0-9]+u *\}?$' | tr -d 'u ')
: "${dat:?l2_golden.h 에서 데이터 시작 오프셋을 못 읽었다}"
: "${q40:?l2_golden.h 에서 t.q4_0 오프셋을 못 읽었다}"
: "${q4k:?l2_golden.h 에서 t.q4_k 오프셋을 못 읽었다}"
echo "   데이터 시작 $dat · t.q4_0 $q40 · t.q4_k $q4k"

"$WORK/l3_golden" "$GGUF" "$dat" "$q40" "$q4k" > "$OUT"
echo "── 골든 헤더: $OUT  ($(grep -c . "$OUT") 줄)"
echo "   llama.cpp: $(git -C "$LLAMA" rev-parse --short HEAD)"
