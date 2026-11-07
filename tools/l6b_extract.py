#!/usr/bin/env python3
"""l6b_extract.py — eval-callback 로그에서 L6b 의 골든 지점을 뽑는다.

    python3 tools/l6b_extract.py <eval.log> <token>

tools/l6b_golden.sh 가 부른다. 별도 파일인 이유는 셸 heredoc 안에
파이썬을 넣으면 인용이 중첩돼 깨지기 때문이다(실제로 겪었다).
"""
import io
import re
import sys


def main() -> int:
    log, token = sys.argv[1], int(sys.argv[2])
    lines = io.open(log, encoding="utf-8", errors="replace").read().splitlines()

    nodes, cur = [], None
    pat = re.compile(r"common_debug_cb_eval:\s+(\S+) = \((\w+)\)\s+(\w+)\(")
    for ln in lines:
        m = pat.match(ln)
        if m:
            cur = (m.group(1), m.group(3))
        elif cur and ln.strip().startswith("sum = "):
            nodes.append((cur[0], cur[1], float(ln.strip()[6:])))
            cur = None

    if not nodes:
        sys.stderr.write("노드를 하나도 못 읽었다 — 출력 형식이 바뀌었나?\n")
        return 2

    # ⚠ **이름이 유일하지 않다.** 1,017개 노드에 서로 다른 이름이 933개다.
    #   예: `norm-0` 이 3번 나오고 값이 전부 다르다
    #       (-35.549904 · 184.643585 · -8.183719).
    #   이름만으로 찾으면 2·3번째를 1번째 골든과 비교해 **멀쩡한 결과가
    #   FAIL 로 나온다** — 실제로 겪었다. (이름, 몇 번째) 를 키로 쓴다.
    seen: dict = {}
    ordered = []
    for n, op, s in nodes:
        k = seen.get(n, 0)
        seen[n] = k + 1
        ordered.append((n, k, op, s))

    # 대조 지점 — **층마다 하나씩** 둔다. 갈리는 곳을 좁히려면 중간이 필요하다.
    #   qwen35 의 어텐션 층은 3·7·11·15·19·23 이고 나머지 18개는 DeltaNet 이다.
    want = ["model.input_embed", "norm-0", "attn_norm-0"]
    want += ["l_out-%d" % i for i in range(24)]
    want += ["norm", "result_norm", "result_output"]

    rows = [(n, k, op, sm) for (n, k, op, sm) in ordered if n in want]

    out = sys.stdout
    out.write("/* tools/l6b_golden.sh 가 만든다 — 손으로 고치지 않는다. */\n")
    out.write("/* 출처: upstream/llama.cpp/build-golden (보드와 같은 기능 집합). */\n")
    out.write("/* 토큰 %d · 노드 전체 %d개 (서로 다른 이름 %d개). */\n"
              % (token, len(nodes), len(seen)))
    out.write("/* ⚠ 이름이 유일하지 않다 — nth(같은 이름 중 몇 번째)가 키의 일부다. */\n\n")
    out.write("#define L6B_TOKEN %d\n" % token)
    out.write("#define L6B_N_NODES %d\n\n" % len(nodes))
    out.write("static const struct { const char *name; int nth; "
              "const char *op; float sum; } l6b_golden[] = {\n")
    for n, k, op, sm in rows:
        out.write('\t{ "%s", %d, "%s", %.6ff },\n' % (n, k, op, sm))
    out.write("};\n")

    missing = [w for w in want if not any(n == w for n, _, _, _ in rows)]
    if missing:
        out.write("\n/* ⚠ 골든 로그에 없는 이름: %s */\n" % ", ".join(missing))
        sys.stderr.write("⚠ 못 찾은 이름: %s\n" % ", ".join(missing))
    sys.stderr.write("✓ 대조 지점 %d개\n" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
