#!/usr/bin/env python3
"""hubml_testdata — QEMU Lm2 로그에서 3자 시험 씨앗 파일을 뽑는다 (Studio · 허브 · Edge 가 같은 파일로 대조).

사용:  python3 tools/hubml_testdata.py /tmp/claude-501/q-hubml_q2.log subsys/hubml/testdata
만드는 것:
  golden_kind1.xmb      Isolation Forest (kind 1) — 평가기 규칙 대조용 (Edge 모델로는 쓰지 않는다)
  golden_kind3.xmb      통계 기준선 (kind 3) — Edge 용
  pv.json               시험 벡터 64 개(정상 32 + 1X ×1.6 32) + 장비가 계산한 kind1 확률 · kind3 점수 (float32 16진)
  pv_vectors.inc        같은 내용의 C 초기화 목록 (Edge 쪽 시험 앱이 쓴다)
  sample_feat.mcs       수집 저장 파일 표본 (HELLO 뒤 FEAT 2 데이터그램 = 16 벡터)
  sample_feat_expected.json  그 파일을 풀었을 때 나와야 하는 값
"""
import base64, datetime, json, os, struct, sys, zlib

def lines_of(log):
    return open(log, errors='replace').read().splitlines()

def b64_block(lines, tag):
    return base64.b64decode(''.join(l.split(' ', 1)[1] for l in lines if l.startswith(tag + ' ')))

def main():
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    L = lines_of(log)
    k1, k3, mcs = b64_block(L, 'XMB64'), b64_block(L, 'XMD64'), b64_block(L, 'MCS64')
    for name, blob in (('golden_kind1.xmb', k1), ('golden_kind3.xmb', k3), ('sample_feat.mcs', mcs)):
        open(os.path.join(out, name), 'wb').write(blob)
    pv = []
    for l in L:
        if l.startswith('PV '):
            h = l.split()[1:]
            pv.append({'x': h[:34], 'kind1_prob': h[34], 'kind3_score': h[35]})
    json.dump({'note': 'float32 비트를 16진으로. x 34 개 = MFEAT34 순서. 앞 32 개 정상, 뒤 32 개는 x[6](ord_1x) ×1.6',
               'tol': 1e-5, 'vectors': pv}, open(os.path.join(out, 'pv.json'), 'w'), indent=1)
    with open(os.path.join(out, 'pv_vectors.inc'), 'w') as f:
        f.write('/* tools/hubml_testdata.py 가 만든다 — 손으로 고치지 않는다. {x[34], kind1_prob, kind3_score} float32 비트 */\n')
        for v in pv:
            f.write('{ ' + ', '.join('0x' + h for h in v['x'] + [v['kind1_prob'], v['kind3_score']]) + ' },\n')
    # 저장 파일 표본 풀기
    assert mcs[:4] == b'MCS1'
    uid, = struct.unpack_from('<Q', mcs, 4)
    o, recs = 16, []
    while o + 2 <= len(mcs):
        n, = struct.unpack_from('<H', mcs, o); o += 2
        d = mcs[o:o + n]; o += n
        assert d[:3] == b'MC\x01'
        ln, = struct.unpack_from('<H', d, 20)
        assert zlib.crc32(d[:24 + ln]) == struct.unpack_from('<I', d, 24 + ln)[0]
        typ, sid, stream, flags, seq, ems = d[3], *struct.unpack_from('<HBBIQ', d, 4)
        boot, = struct.unpack_from('<H', d, 22)
        pl = d[24:24 + ln]
        nv, contract = pl[0], pl[1]
        vecs = [['%08x' % struct.unpack_from('<I', pl, 4 + i * (contract * 4 + 4) + 4 * k)[0] for k in range(contract)]
                for i in range(nv)]
        recs.append({'type': typ, 'edge_sid': sid, 'stream': stream, 'flags': flags, 'seq': seq,
                     'edge_ms': ems, 'boot_id': boot, 'len': ln, 'n': nv, 'contract': contract, 'vectors': vecs})
    gen = {}
    for l in L:
        if l.startswith('MCSV '):
            p = l.split(); gen[(int(p[1]), int(p[2]))] = p[3:]
    for r in recs:
        for i, v in enumerate(r['vectors']):
            assert gen[(r['seq'], i)] == v, '저장 파일과 만든 값이 다르다'
    json.dump({'format': 'MCS1 머리 16 B(magic·uid u64·stream·ver·rsv) + [u16 길이][MCOL/1 데이터그램]…',
               'uid': '%016x' % uid, 'stream': mcs[12], 'records': recs}, open(os.path.join(out, 'sample_feat_expected.json'), 'w'), indent=1)
    print(f'kind1 {len(k1)} B · kind3 {len(k3)} B · pv {len(pv)} · 표본 {len(mcs)} B / 레코드 {len(recs)} (만든 값과 일치)')

# ══ 정상 관문(normal_gate) 걸러내기 기준 구현 — apps/hubml_q11 (Lm11) 의 기대값 ═══════════════════════════════════════
# 규칙은 Studio MlccAnalysis.cpp readDaySeries/readDayMerged 를 옮긴 것이다 (C 구현과 따로 쓴 두 번째 구현 — 대조용).
#  · 앵커 = 그 .mcs 파일이 있는 날짜 폴더의 anchor.jsonl 중 time_set=1 · boot 같음 · |edge 차| 최소(같으면 앞 줄)
#  · utc0 = anchor.hub + (rec.edge - anchor.edge), 벡터 i>0 이면 utc += dt[i] (dt[0] 은 더하지 않음; quality≠0 의 dt 도 누적)
#  · 앵커 없는 벡터는 뺀다(unanchored) — Studio 는 순서로 추정하지만 여기서는 뺀다 (의도적 차이)
#  · 남김: (UTC 날짜, 하루 안 ms) 가 어떤 include [시작,끝) 안이고 어떤 exclude 안도 아님. include 밖 = outside, exclude = excluded
#  · 읽을 파일 = include 날짜 D 마다 D · D+1 · D+2 의 합집합(정렬)
UTC = datetime.timezone.utc

def T(day, hh=0, mm=0, ss=0, ms=0):
    d = datetime.datetime(int(day[:4]), int(day[4:6]), int(day[6:8]), tzinfo=UTC)
    return int(d.timestamp()) * 1000 + ((hh * 60 + mm) * 60 + ss) * 1000 + ms

def hm_min(s):
    h, m = s.split(':')
    return int(h) * 60 + int(m)

def read_days(include):
    out = set()
    for d, _a, _b in include:
        dt = datetime.date(int(d[:4]), int(d[4:6]), int(d[6:8]))
        for k in range(3):
            out.add((dt + datetime.timedelta(days=k)).strftime('%Y%m%d'))
    return sorted(out)

def gate_ref(include, exclude, anchors, recs):
    """anchors: {day: [(boot, edge, hub, time_set)…]} · recs: {day: [{boot, edge, n, dt, q, id0}…]} (파일 안 순서).
    반환 (남긴 id 목록(읽는 순서), {'unanchored','outside','excluded'})"""
    inc = [(d, hm_min(a), hm_min(b)) for d, a, b in include]
    exc = [(d, hm_min(a), hm_min(b)) for d, a, b, _r in exclude]
    kept, cnt = [], {'unanchored': 0, 'outside': 0, 'excluded': 0, 'bad': 0}
    for day in read_days(include):
        an = [a for a in anchors.get(day, []) if a[3]]
        for r in recs.get(day, []):
            best = None
            for a in an:
                if a[0] != r['boot']:
                    continue
                if best is None or abs(a[1] - r['edge']) < abs(best[1] - r['edge']):
                    best = a
            utc = best[2] + (r['edge'] - best[1]) if best else None
            for i in range(r['n']):
                if i > 0 and best:
                    utc += r['dt'][i]
                if r['q'][i] != 0 or r.get('nf', [0] * 8)[i]:
                    cnt['bad'] += 1   # quality≠0 + 비유한
                    continue
                if not best:
                    cnt['unanchored'] += 1
                    continue
                t = datetime.datetime.fromtimestamp(utc / 1000, tz=UTC)
                ds = t.strftime('%Y%m%d')
                ms = ((t.hour * 60 + t.minute) * 60 + t.second) * 1000 + t.microsecond // 1000
                if not any(ds == d and a * 60000 <= ms < b * 60000 for d, a, b in inc):
                    cnt['outside'] += 1
                elif any(ds == d and a * 60000 <= ms < b * 60000 for d, a, b in exc):
                    cnt['excluded'] += 1
                else:
                    kept.append(r['id0'] + i)
    return kept, cnt

def gate_scenario(path):
    D = ['20261109', '20261110', '20261111', '20261112', '20261113', '20261114']
    anchors = {d: [] for d in D}
    recs = {d: [] for d in D}
    nid = [1]
    def A(day, boot, edge, hub, ts=1):
        anchors[day].append((boot, edge, hub, ts))
    def R(day, boot, utc0, anchor, dt=None, q=None, n=8, edge=None, nf=None):
        """utc0 를 앵커(boot, edge0, hub0)로 되돌려 edge 를 정한다 (anchor=None 이면 edge 를 직접)"""
        if edge is None:
            edge = anchor[0] + (utc0 - anchor[1])
        recs[day].append({'boot': boot, 'edge': edge, 'n': n, 'dt': dt or [7777] + [1000] * 7, 'q': q or [0] * 8,
                          'nf': nf or [0] * 8, 'id0': nid[0]})
        nid[0] += 8
    # 1109 (D-1: 읽지 않는다) — UTC 1110 11:00 이지만 안 읽으니 어디에도 안 센다
    A('20261109', 5, 1000000, T('20261110', 9)); R('20261109', 5, T('20261110', 11), (1000000, T('20261110', 9)))
    # 1110: boot5 앵커 A1(ts=1) · 미끼 A0(ts=0, 더 가까움)
    a1 = (1000000, T('20261110', 9))
    A('20261110', 5, a1[0], a1[1]); A('20261110', 5, 6000000, T('20261110', 20), 0)
    R('20261110', 5, T('20261110', 9, 59, 56), a1)                                    # 시작 경계 [10:00
    R('20261110', 5, T('20261110', 11, 59, 56), a1)                                   # 끝 경계 12:00)
    R('20261110', 5, T('20261110', 10, 29, 56), a1)                                   # exclude 시작 10:30
    R('20261110', 5, T('20261110', 10, 39, 56), a1)                                   # exclude 끝 10:40)
    R('20261110', 5, T('20261110', 10, 39, 57, 500), a1, dt=[7777, 1000, 2000, 1000, 1000, 1000, 1000, 1000],
      q=[0, 0, 1, 0, 0, 0, 0, 0])                                                      # dt 누적 (quality≠0 의 dt 포함)
    R('20261110', 5, T('20261110', 11, 30), a1, nf=[0, 0, 1, 0, 0, 0, 0, 0])             # 비유한(NaN) 특징 한 벡터
    R('20261110', 9, 0, None, edge=123456)                                             # 앵커 없는 boot
    # 1111: boot5 B1 · B3(같은 간격, 다른 시각 — 동률은 앞 줄) · boot6 C1 · boot7 C2 · boot8 C3(UTC 1110)
    b1 = (50000000, T('20261111', 8))
    A('20261111', 5, b1[0], b1[1]); A('20261111', 5, b1[0] + 7200000, T('20261112', 5))
    A('20261111', 6, 10000000, T('20261111', 12)); A('20261111', 7, 100000, T('20261111', 0))
    A('20261111', 8, 100000000, T('20261110', 12))
    R('20261111', 5, T('20261111', 9), b1)                                              # 동률 앵커 → 앞 줄(B1)
    R('20261111', 5, T('20261111', 3), b1)
    R('20261111', 6, T('20261111', 12, 59, 56), (10000000, T('20261111', 12)))         # exclude [12:00,13:00) 끝 경계
    R('20261111', 6, T('20261111', 23, 59, 56), (10000000, T('20261111', 12)))         # 24:00 끝 · UTC 가 D+1 로 넘어감
    R('20261111', 7, T('20261111', 0), (100000, T('20261111', 0)))                      # 00:00 시작 경계
    R('20261111', 8, T('20261110', 11, 30), (100000000, T('20261110', 12)))             # D+1 파일 · UTC D (G1·G2 모두 들어감)
    R('20261111', 8, T('20261110', 12, 30), (100000000, T('20261110', 12)))             # D+1 파일 · UTC D 지만 include 밖
    # 1112: boot5 는 ts=0 앵커뿐 → unanchored · boot6 는 ts=1 앵커 (UTC 1111 14:00)
    A('20261112', 5, 0, T('20261112', 1), 0); A('20261112', 6, 5000000, T('20261111', 14))
    R('20261112', 5, 0, None, edge=10)
    R('20261112', 6, T('20261111', 14), (5000000, T('20261111', 14)))
    # 1113 (D+2 of 1111) · 1114 (D+3: 읽지 않는다)
    A('20261113', 6, 0, T('20261111', 8)); R('20261113', 6, T('20261111', 8), (0, T('20261111', 8)))
    A('20261114', 6, 0, T('20261111', 10)); R('20261114', 6, T('20261111', 10), (0, T('20261111', 10)))
    G = {
        'G1': ([('20261110', '10:00', '12:00'), ('20261111', '00:00', '24:00')],
               [('20261110', '10:30', '10:40', 'fault_suspect'), ('20261111', '12:00', '13:00', 'maintenance')]),
        'G2': ([('20261110', '10:00', '12:00')], []),
    }
    def js(iv):
        return '[' + ','.join('[' + ','.join('"%s"' % x for x in e) + ']' for e in iv) + ']'
    o = ['/* tools/hubml_testdata.py --gate-inc 가 만든다 — 손으로 고치지 않는다 (Studio 규칙의 기준 구현 gate_ref 로 계산한 기대값) */']
    o.append('static const char *const SC_DAYS[] = { %s };' % ', '.join('"%s"' % d for d in D))
    o.append('struct sc_anchor { uint8_t day; uint16_t boot; int64_t edge, hub; uint8_t ts; };')
    o.append('static const struct sc_anchor SC_ANCH[] = {')
    for di, d in enumerate(D):
        for b, e, h, t in anchors[d]:
            o.append('	{ %d, %d, %dLL, %dLL, %d },' % (di, b, e, h, t))
    o.append('};')
    o.append('struct sc_rec { uint8_t day; uint16_t boot; uint64_t edge; uint8_t n; uint16_t dt[8]; uint8_t q[8]; uint8_t nf[8]; uint32_t id0; };')
    o.append('static const struct sc_rec SC_REC[] = {')
    for di, d in enumerate(D):
        for r in recs[d]:
            o.append('	{ %d, %d, %dULL, %d, { %s }, { %s }, { %s }, %d },' % (di, r['boot'], r['edge'], r['n'],
                     ', '.join(map(str, r['dt'])), ', '.join(map(str, r['q'])), ', '.join(map(str, r['nf'])), r['id0']))
    o.append('};')
    for name, (inc, exc) in G.items():
        kept, cnt = gate_ref(inc, exc, anchors, recs)
        o.append('#define SC_%s_INC "%s"' % (name, js(inc).replace('"', '\\"')))
        o.append('#define SC_%s_EXC "%s"' % (name, js(exc).replace('"', '\\"')))
        o.append('#define SC_%s_BAD %d\n#define SC_%s_UNANCH %d\n#define SC_%s_OUTSIDE %d\n#define SC_%s_EXCLUDED %d' % (
            name, cnt['bad'], name, cnt['unanchored'], name, cnt['outside'], name, cnt['excluded']))
        o.append('static const uint32_t SC_%s_KEPT[] = { %s };' % (name, ', '.join(map(str, kept))))
        print(name, 'kept', len(kept), cnt, 'read', read_days(inc))
    # 큰 사례 (학습 한 번 끝까지): uid B, 20261120 하루 앞 4000 초 · boot3 앵커 하나 + 앵커 없는 boot4 2 묶음
    bd = '20261120'
    banch = {bd: [(3, 0, T(bd, 0), 1)]}
    brecs = {bd: [{'boot': 3, 'edge': 8000 * r, 'n': 8, 'dt': [1000] * 8, 'q': [0] * 8, 'id0': 8 * r} for r in range(500)] +
             [{'boot': 4, 'edge': 5000000 + 8000 * k, 'n': 8, 'dt': [1000] * 8, 'q': [0] * 8, 'id0': 4000 + 8 * k} for k in range(2)]}
    # D+1 · D+2 파일에도 1120 UTC 벡터 한 묶음씩 (읽혀서 남지만 merkle 에는 들어가지 않는다)
    for dd, hh, mm in (('20261121', 0, 50), ('20261122', 0, 55)):
        banch[dd] = [(3, 0, T(bd, hh, mm), 1)]
        brecs[dd] = [{'boot': 3, 'edge': 0, 'n': 8, 'dt': [1000] * 8, 'q': [0] * 8, 'nf': [0, 0, 0, 1, 0, 0, 0, 0] if dd == '20261121' else [0] * 8, 'id0': 9000}]
    binc, bexc = [(bd, '00:00', '01:00')], [(bd, '00:10', '00:20', 'fault_suspect')]
    bk, bc = gate_ref(binc, bexc, banch, brecs)
    b1inc = [(bd, '00:00', '00:30')]
    b1k, b1c = gate_ref(b1inc, [], banch, brecs)
    o.append('#define SC_B_HUB %dLL' % T(bd))
    o.append('#define SC_B_DAY "%s"\n#define SC_B_INC "%s"\n#define SC_B_EXC "%s"' % (bd, js(binc).replace('"', '\\"'), js(bexc).replace('"', '\\"')))
    o.append('#define SC_B1_INC "%s"' % js(b1inc).replace('"', '\\"'))
    o.append('#define SC_B_BAD %d\n#define SC_B_KEPT %d\n#define SC_B_UNANCH %d\n#define SC_B_OUTSIDE %d\n#define SC_B_EXCLUDED %d' % (bc['bad'], len(bk), bc['unanchored'], bc['outside'], bc['excluded']))
    o.append('#define SC_B1_KEPT %d' % len(b1k))
    # 관문 없는 예전 경로 (days=): 앵커와 무관하게 quality 0 인 것 전부
    o.append('#define SC_B_ALL %d' % sum(r['n'] for r in brecs[bd]))   # days=20261120 경로: 그 날 파일만
    print('B kept', len(bk), bc, 'B1 kept', len(b1k))
    open(path, 'w').write('\n'.join(o) + '\n')

if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--gate-inc':
        gate_scenario(sys.argv[2])
    else:
        main()
