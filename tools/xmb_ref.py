#!/usr/bin/env python3
"""xmb_ref — .xmb(XMB1, kind 1) 파이썬 기준 평가기 · QEMU/실기 로그 대조.

사용:  python3 tools/xmb_ref.py <로그>
  로그의 XMB64 줄(모델 base64)과 PV 줄(특징 34 개 + 장비가 계산한 확률, float32 16진)을 읽어
  같은 모델을 파이썬(numpy float32)으로 다시 계산해 비교한다. 규칙은 펌웨어 eval_buf ·
  Studio XgbEvaluator 와 같다: strict `<` · 범위 밖 특징 0 · 깊이 64 · logf(base/(1-base+1e-7)) ·
  시그모이드 안전 분기. 허용 오차는 MPDM 팩 parity_tol 1e-5.
"""
import base64, struct, sys, zlib
import numpy as np

TOL = 1e-5

def parse(b):
    assert b[:4] == b'XMB1', 'magic'
    assert zlib.crc32(b[:60]) == struct.unpack_from('<I', b, 60)[0], 'hdr crc'
    kind, nfeat = b[5], b[6]
    ntree, = struct.unpack_from('<H', b, 8)
    nnode, = struct.unpack_from('<I', b, 10)
    base, thr = struct.unpack_from('<ff', b, 14)
    contract, = struct.unpack_from('<H', b, 22)
    assert kind == 1 and nfeat == contract
    o = 64
    roots = struct.unpack_from('<%dI' % ntree, b, o); o += 4 * ntree
    nodes = []
    for i in range(nnode):
        f, l, r, leaf, _, t, v = struct.unpack_from('<hhhBBff', b, o); o += 16
        nodes.append((f, l, r, leaf, np.float32(t), np.float32(v)))
    gold = []
    for g in range(4):
        vals = struct.unpack_from('<%df' % (nfeat + 1), b, o); o += 4 * (nfeat + 1)
        gold.append(vals)
    assert zlib.crc32(b[64:o]) == struct.unpack_from('<I', b, o)[0], 'body crc'
    return dict(roots=roots, nodes=nodes, base=np.float32(base), thr=np.float32(thr),
                contract=contract, gold=gold, meta=b[28:52].split(b'\0')[0].decode(errors='replace'))

def sigmoid_safe(z):
    z = np.float32(z)
    if z >= 0:
        e = np.float32(np.exp(-z, dtype=np.float32))
        return np.float32(np.float32(1) / (np.float32(1) + e))
    e = np.float32(np.exp(z, dtype=np.float32))
    return np.float32(e / (np.float32(1) + e))

def predict(m, x):
    s = np.float32(0)
    for r in m['roots']:
        idx, d = r, 0
        while 0 <= idx < len(m['nodes']) and d < 64:
            f, l, rr, leaf, t, v = m['nodes'][idx]
            if leaf:
                s = np.float32(s + v)
                break
            xv = np.float32(x[f]) if 0 <= f < m['contract'] else np.float32(0)
            idx = l if xv < t else rr
            d += 1
    base = m['base']
    bl = np.float32(np.log(base / (np.float32(1) - base + np.float32(1e-7)), dtype=np.float32))
    return sigmoid_safe(np.float32(bl + s))

def parse_md(b):
    """kind=3 통계 기준선 (subsys/hubml/include/hubml/xmb.h 의 설명과 같은 배치)"""
    assert b[:4] == b'XMB1' and b[5] == 3, 'kind 3 아님'
    assert zlib.crc32(b[:60]) == struct.unpack_from('<I', b, 60)[0], 'hdr crc'
    n = b[6]; o = 64
    nreg, regf = b[o], b[o + 1]; o += 4
    edges = struct.unpack_from('<3f', b, o); o += 12
    tri = n * (n + 1) // 2
    regs = []
    for r in range(nreg):
        mean = np.array(struct.unpack_from('<%df' % n, b, o), np.float32); o += 4 * n
        sd = np.array(struct.unpack_from('<%df' % n, b, o), np.float32); o += 4 * n
        L = np.array(struct.unpack_from('<%df' % tri, b, o), np.float32); o += 4 * tri
        thr, = struct.unpack_from('<f', b, o); o += 4
        regs.append((mean, sd, L, np.float32(thr)))
    gold = [struct.unpack_from('<%df' % (n + 1), b, o + 4 * (n + 1) * g) for g in range(4)]
    o += 4 * (n + 1) * 4
    assert zlib.crc32(b[64:o]) == struct.unpack_from('<I', b, o)[0], 'body crc'
    return dict(n=n, nreg=nreg, regf=regf, edges=[np.float32(e) for e in edges], regs=regs, gold=gold)

def score_md(m, x):
    x = np.array(x, np.float32)
    r = 0
    while r < m['nreg'] - 1 and x[m['regf']] >= m['edges'][r]:
        r += 1
    mean, sd, L, thr = m['regs'][r]
    y = np.zeros(m['n'], np.float32); d2 = np.float32(0); o = 0
    for i in range(m['n']):
        t = np.float32(x[i] - mean[i])
        for k in range(i):
            t = np.float32(t - np.float32(L[o + k] * y[k]))
        y[i] = np.float32(t / L[o + i])
        d2 = np.float32(d2 + np.float32(y[i] * y[i]))
        o += i + 1
    return np.float32(d2 / thr)

def f32(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]

def main():
    lines = open(sys.argv[1], errors='replace').read().splitlines()
    b64 = ''.join(l.split(' ', 1)[1] for l in lines if l.startswith('XMB64 '))
    blob = base64.b64decode(b64)
    m = parse(blob)
    print(f'모델 {len(blob)} B · {m["meta"]} · 트리 {len(m["roots"])} · 노드 {len(m["nodes"])} · 문턱 {m["thr"]:.4f}')
    worst, n = 0.0, 0
    for g in m['gold']:
        d = abs(float(predict(m, g[:-1])) - g[-1]); worst = max(worst, d); n += 1
    for l in lines:
        if not l.startswith('PV '):
            continue
        h = l.split()[1:]
        x = [f32(v) for v in h[:m['contract']]]
        dev = f32(h[m['contract']])
        d = abs(float(predict(m, x)) - dev); worst = max(worst, d); n += 1
    ok = n > 4 and worst <= TOL
    print(f'kind 1 대조 {n} 개(골든 4 + PV) · 최대 |Δ| {worst:.3e} · {"일치" if ok else "불일치"} (허용 {TOL})')
    md64 = ''.join(l.split(' ', 1)[1] for l in lines if l.startswith('XMD64 '))
    if md64:
        md = parse_md(base64.b64decode(md64))
        w3, n3 = 0.0, 0
        for g in md['gold']:
            got = float(score_md(md, g[:-1])); w3 = max(w3, abs(got - g[-1]) / max(1.0, abs(got))); n3 += 1
        for l in lines:
            if l.startswith('PV '):
                h = l.split()[1:]
                if len(h) < md['n'] + 2:
                    continue
                x = [f32(v) for v in h[:md['n']]]
                dev = f32(h[md['n'] + 1])
                got = float(score_md(md, x)); w3 = max(w3, abs(got - dev) / max(1.0, abs(got))); n3 += 1
        ok3 = n3 > 4 and w3 <= TOL
        print(f'kind 3 대조 {n3} 개 · 최대 상대 |Δ| {w3:.3e} · {"일치" if ok3 else "불일치"} '
              f'(구간 {md["nreg"]} · 문턱 {[round(float(r[3]), 2) for r in md["regs"]]})')
        ok = ok and ok3
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
