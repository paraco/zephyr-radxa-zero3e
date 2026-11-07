#!/usr/bin/env python3
"""l6b_cmp_golden.py — 골든 두 벌을 비교한다.

    python3 tools/l6b_cmp_golden.py a.inc b.inc

── 왜 필요한가 ──────────────────────────────────────────────────────────

L6b 에서 층 0~15 는 0~14 ppb 로 맞는데 층 16~23 이 1e-3 까지 갈렸다.
`qwen35.cpp` 에 층별 분기가 없으므로(마지막 층 제외) 24개 층이 같은 코드를
돌고, 민감한 연산은 `sigmoid`·`silu`·`softplus` — 전부 libm 이다.

가설은 "libm 차이(~1 ulp)가 깊이에 따라 증폭된다" 지만, **그것을 주장으로
두면 안 된다.** 가릴 방법: **호스트 자신의 두 빌드**를 비교한다. 같은 소스를
`-O3` 와 `-O2` 로만 다르게 빌드해 층별 값이 얼마나 갈리는지 본다.

  · 호스트 두 벌이 층 16~23 에서 **비슷한 크기로** 갈리면 →
    그 구간은 수치적으로 그만큼 민감하다는 뜻이고, 보드의 결과는 **모델
    자신의 잡음 안**이다. 포팅 결함이 아니다.
  · 호스트 두 벌이 **전부 비트 일치**하면 → 민감한 게 아니다.
    보드 쪽에 원인이 있다는 뜻이므로 더 파야 한다.

둘 다 "그럴 것 같다" 를 숫자로 바꾼다.
"""
import re
import sys


def load(path):
    rows = {}
    pat = re.compile(r'\{\s*"([^"]+)",\s*(\d+),\s*"([^"]+)",\s*([-\d.e+]+)f\s*\}')
    with open(path, encoding="utf-8") as fh:
        for ln in fh:
            m = pat.search(ln)
            if m:
                rows[(m.group(1), int(m.group(2)))] = (m.group(3), float(m.group(4)))
    return rows


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    keys = [k for k in a if k in b]
    if not keys:
        sys.exit("겹치는 지점이 없다")
    print("%-20s %-4s %16s %16s %12s" % ("노드", "nth", sys.argv[1].split('/')[-1],
                                         sys.argv[2].split('/')[-1], "차이 ppb"))
    worst = 0.0
    nbit = 0
    for k in keys:
        _, va = a[k]
        _, vb = b[k]
        d = abs(va - vb)
        scale = max(abs(va), 1.0)
        ppb = d / scale * 1e9
        worst = max(worst, ppb)
        if d == 0.0:
            nbit += 1
        print("%-20s %-4d %16.6f %16.6f %12.0f" % (k[0], k[1], va, vb, ppb))
    print()
    print("지점 %d개 · 비트 일치 %d개 · 최대 차이 %.0f ppb" % (len(keys), nbit, worst))
    print()
    if nbit == len(keys):
        print("→ 두 벌이 **완전히 같다**. 이것은 '민감하지 않다' 가 아니라")
        print("  **교란이 들어가지 않았다** 는 뜻이다 (같은 순서로 같은 계산을 했다).")
        print("  민감도를 재려면 실제로 값을 흔드는 변경이어야 한다")
        print("  (예: -ffp-contract=fast 는 FMA 축약으로 ~1 ulp 를 넣는다).")
    elif worst < 1000:
        print("→ 교란이 들어갔는데도 %.0f ppb 뿐이다. 이 구간은 민감하지 **않다**." % worst)
        print("  보드의 1e-3 갈림은 **설명되지 않는다** — 포팅 쪽을 더 파야 한다.")
    else:
        print("→ 호스트 두 빌드만으로도 %.0f ppb 갈린다." % worst)
        print("  그 구간은 수치적으로 그만큼 민감하다는 뜻이다.")
        print("  보드의 갈림이 이 범위 안이면 **모델 자신의 잡음**이고 포팅 결함이 아니다.")


if __name__ == "__main__":
    main()
