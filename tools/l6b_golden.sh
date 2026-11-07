#!/usr/bin/env bash
#
# l6b_golden.sh — L6b 의 호스트 골든값을 뽑는다.
#
#   ./tools/l6b_golden.sh [모델.gguf] > apps/llm_l6b/src/golden.inc
#
# ── 왜 별도 빌드인가 ────────────────────────────────────────────────────
#
# 기존 `upstream/llama.cpp/build` 의 바이너리는 **보드와 다른 코드**다.
# 스스로 찍는다:
#     CPU : … MATMUL_INT8 = 1 | DOTPROD = 1 | ACCELERATE = 1 | REPACK = 1
# i8mm · repack · Accelerate 가 전부 켜져 있고 Metal 도 들어 있다.
# `--device none -ngl 0` 은 Metal 만 뗀다. 다섯 가지가 모두 matmul 수치에
# 영향을 주므로, 그대로 비교하면 불일치가 포팅 결함인지 빌드 차이인지
# 가릴 수 없다. docs/l6b-golden-plan.md 참고.
#
# 그래서 `build-golden` 을 쓴다 — ggml 이 보는 ARM 매크로 8개가 보드와 같다.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
LLAMA="${LLAMA_DIR:-$(dirname "$REPO")/upstream/llama.cpp}"
MODEL="${1:-$HOME/Projects/PiZZa/models/Qwen3.5-0.8B-Q4_K_M.gguf}"
BIN="$LLAMA/build-golden/bin/llama-eval-callback"

# L6b 는 토크나이저를 쓰지 않는다. 토큰을 **고정**한다.
#   llama-tokenize -p "hi" --ids  →  [5834]
TOKEN="${L6B_TOKEN:-5834}"
PROMPT="${L6B_PROMPT:-hi}"

if [ ! -x "$BIN" ]; then
	cat >&2 <<MSG
골든 바이너리가 없다: $BIN

보드와 **같은 기능 집합**으로 만들어야 한다 (docs/l6b-golden-plan.md):

  cmake -S "$LLAMA" -B "$LLAMA/build-golden" -DCMAKE_BUILD_TYPE=Release \\
    -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF \\
    -DGGML_CPU_REPACK=OFF -DGGML_LLAMAFILE=OFF -DGGML_NATIVE=OFF \\
    -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 \\
    -DCMAKE_C_FLAGS=-ffp-contract=off -DCMAKE_CXX_FLAGS=-ffp-contract=off \\
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF
  cmake --build "$LLAMA/build-golden" -j --target llama-eval-callback
MSG
	exit 1
fi

WORK="${TMPDIR:-/tmp}/l6b-golden"
mkdir -p "$WORK"
LOG="$WORK/eval.log"

echo "골든 바이너리: $BIN" >&2
echo "모델:          $MODEL" >&2
echo "토큰:          $TOKEN ('$PROMPT')" >&2

#
# ⚠ **보드와 같은 설정으로 돌려야 한다.** 그래프가 설정에 따라 달라진다 —
#   기본값에서는 노드가 1,017개인데 `-c 256 -b 1 -ub 1` 에서는 1,431개다.
#   노드 이름(`l_out-3` 등)도 갈릴 수 있으므로 골든을 보드 설정에서 뽑는다.
#
#   보드가 쓰는 값 (apps/llm_l6b/src/main.cpp):
#     n_ctx   32 를 주면 llama.cpp 가 **256 으로 올린다** (최소 단위)
#     n_batch 1 · n_ubatch 1 · n_threads 1
#     flash-attn 끔 (기본이 끔이므로 플래그 없음)
N_CTX="${L6B_N_CTX:-256}"
echo "설정:          n_ctx=$N_CTX n_batch=1 n_ubatch=1 n_threads=1" >&2

"$BIN" -m "$MODEL" -p "$PROMPT" -n 1 -t 1 \
       -c "$N_CTX" -b 1 -ub 1 > "$LOG" 2>&1 || {
	echo "eval-callback 실패 — $LOG 를 보라" >&2; exit 2; }

# ── 관문: 기능 집합이 보드와 같은가 ────────────────────────────────────
#
# ⚠ 이것을 건너뛰면 **잘못된 골든값을 조용히** 쓴다. 플래그가 안 먹은
#   빌드는 겉보기에 똑같이 동작한다.
SI=$(grep -m1 "system_info" "$LOG" || true)
if [ -z "$SI" ]; then
	echo "❌ system_info 줄이 없다 — 바이너리가 제대로 돌지 않았다" >&2; exit 3
fi
echo "기능 집합: ${SI#*system_info: }" >&2
BAD=0
for f in MATMUL_INT8 REPACK ACCELERATE LLAMAFILE MTL BLAS; do
	if printf '%s' "$SI" | grep -q "$f"; then
		echo "❌ $f 가 켜져 있다 — 보드에는 없다. 골든값을 믿을 수 없다" >&2
		BAD=1
	fi
done
for f in DOTPROD FP16_VA ARM_FMA NEON; do
	if ! printf '%s' "$SI" | grep -q "$f"; then
		echo "❌ $f 가 없다 — 보드에는 있다. 골든값을 믿을 수 없다" >&2
		BAD=1
	fi
done
[ "$BAD" -eq 0 ] || { echo "   build-golden 을 위 cmake 로 다시 만들어라" >&2; exit 4; }
echo "✓ 기능 집합이 보드와 같다" >&2

# ── 노드별 sum 추출 ────────────────────────────────────────────────────
# 노드별 sum 추출 — 별도 파일이다 (heredoc 안에 파이썬을 넣으면 인용이 중첩돼
# 깨진다, 실제로 겪었다).
exec python3 "$HERE/l6b_extract.py" "$LOG" "$TOKEN"
