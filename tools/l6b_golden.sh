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
python3 - "$LOG" "$TOKEN" <<'PY'
import re, sys, io
log, token = sys.argv[1], int(sys.argv[2])
lines = io.open(log, encoding='utf-8', errors='replace').read().splitlines()

nodes, cur = [], None
for ln in lines:
    m = re.match(r'common_debug_cb_eval:\s+(\S+) = \((\w+)\)\s+(\w+)\(', ln)
    if m:
        cur = (m.group(1), m.group(3))
    elif cur and ln.strip().startswith('sum = '):
        nodes.append((cur[0], cur[1], float(ln.strip()[6:])))
        cur = None

if not nodes:
    sys.exit("노드를 하나도 못 읽었다 — eval-callback 출력 형식이 바뀌었나?")

# L6b 가 보는 지점. 하나라도 어긋나면 그 사이를 좁힌다.
#   docs/l6b-golden-plan.md 의 "범위" 참고.
want = ['model.input_embed', 'norm-0', 'attn_norm-0',
        'l_out-0', 'l_out-3', 'l_out-23', 'norm', 'result_norm', 'result_output']
by_name = {}
for n, op, s in nodes:
    by_name.setdefault(n, (op, s))

print("/* tools/l6b_golden.sh 가 만든다 — 손으로 고치지 않는다. */")
print("/* 출처: upstream/llama.cpp/build-golden (보드와 같은 기능 집합). */")
print(f"/* 토큰 {token} · 노드 전체 {len(nodes)}개 중 아래 지점을 대조한다. */")
print()
print(f"#define L6B_TOKEN {token}")
print(f"#define L6B_N_NODES {len(nodes)}")
print()
print("static const struct { const char *name; const char *op; float sum; } l6b_golden[] = {")
missing = []
for w in want:
    if w in by_name:
        op, s = by_name[w]
        print(f'\t{{ "{w}", "{op}", {s:.6f}f }},')
    else:
        missing.append(w)
print("};")
if missing:
    print()
    print("/* ⚠ 골든 로그에 없는 이름: " + ", ".join(missing) + " */")
    print("/*   qwen35 의 노드 이름이 예상과 다르다. 아래 전체 목록에서 고른다. */")
    sys.stderr.write("⚠ 못 찾은 이름: " + ", ".join(missing) + "\n")
    sys.stderr.write("   첫 40개 노드 이름:\n")
    for n, op, s in nodes[:40]:
        sys.stderr.write(f"     {n:30s} {op:12s} {s}\n")
PY
