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


# ══ 시범 운영(shadow) SCORE — apps/hubml_q12 (Lm12) · hubml_edge_q 의 기대값 ═══════════════════════════════════════════
# 규약(mcol.h): SCORE 본문 = 머리 16 B(feat_seq u32 · n u8 · flags u8 · rsv u16 · champ_num u32 · chal_num u32) + n × 9 B
#   { champ f32 · chal f32 · vflags u8 }, 길이 = 16 + 9n 정확히, n = 1..8. vflags bit0/1 valid · bit2/3 alarm · bit4~7 무시.
# 요약(ml shadow ls) 규칙 — 허브 C 구현(ml_shell.c cmd_shadow_ls)과 따로 쓴 두 번째 구현:
#   · 깨진 레코드(길이 어긋남 · n 이 0·9 이상) = bad(shadow_day)로만 센다. 어느 쌍에도 안 센다.
#   · 슬롯 유효 = valid 비트 && 점수가 유한(NaN/inf 면 무효 — 그 슬롯의 경보 비트도 무시). 경보 = 유효 && 경보 비트.
#   · valid_both = 두 슬롯 모두 유효. both/champ_only/chal_only/agree 는 valid_both 안에서만 센다
#     (both = 둘 다 경보, champ_only = champion 만 경보, chal_only = challenger 만 경보, agree = 경보 판정이 같은 비율).
#   · alarm_champ/alarm_chal = 그 슬롯이 유효한 벡터 중 경보 수 (상대 슬롯이 무효여도 센다).
#   · mean_* = 유효한 벡터 점수의 평균 · corr = valid_both 벡터의 Pearson (valid_both<2 이거나 분산 0 이면 '-')
#   · 날마다 shadow_day(pairs · pairs_over · bad [· err]) 한 줄 + 쌍(champ_num, chal_num)마다 shadow 한 줄 — 통계는 쌍별로 따로 누적,
#     순서는 벡터 수 많은 순(같으면 먼저 나온 쌍). num 0 이면 champ/chal 은 '-'. 쌍이 32 개를 넘으면 넘은 쌍의 벡터는 어느 줄에도
#     안 넣고 pairs=32 pairs_over=1. 저장소 읽기 오류(잘린 꼬리 등)는 shadow_day 끝 err=<rc> (부분 합계 그대로).
#   · 누적은 double, 순서는 파일(수신) 순서.
SC_UID = '4d50444d000000c1'

def f32bits(x):
    return struct.pack('<f', float(x))   # struct 가 float32 로 둥글린다

def score_body(feat_seq, ents, champ_num, chal_num, flags=0, rsv=0, n=None, extra=0, trim=0):
    """ents = [(champ, chal, vflags)…]. n/extra/trim 으로 깨진 레코드를 만든다 (n 칸 · 뒤에 붙이기 · 뒤에서 자르기)"""
    b = struct.pack('<IBBHII', feat_seq, len(ents) if n is None else n, flags, rsv, champ_num, chal_num)
    for c, h, v in ents:
        b += f32bits(c) + f32bits(h) + bytes([v])
    return b + bytes(extra) if not trim else (b + bytes(extra))[:-trim]

def shadow_lines(uid, days, files, errs=None, max_pairs=32):
    """files = {day: [(type, body bytes)…]} (파일 안 순서, 중복·잘린 꼬리는 이미 뺀 것). errs = {day: 'err=%d 칸에 쓸 문자열'}.
    반환 줄 목록: 날마다 shadow_day 줄 + 쌍마다 shadow 줄(벡터 수 많은 순, 같으면 먼저 나온 쌍), 끝에 shadow_done"""
    import math
    out = []
    for day in days:
        pairs = {}              # (champ_num, chal_num) -> 누적 (삽입 순서 = 먼저 나온 순서)
        bad, over = 0, False
        for typ, body in files.get(day, []):
            if typ != 11:
                continue
            if len(body) < 16 or not (1 <= body[4] <= 8) or len(body) != 16 + 9 * body[4]:
                bad += 1
                continue
            key = struct.unpack_from('<II', body, 8)
            if key not in pairs:
                if len(pairs) >= max_pairs:
                    over = True         # 넘은 쌍은 어느 줄에도 안 넣는다
                    continue
                pairs[key] = dict(n=0, vb=0, ac=0, ah=0, both=0, co=0, ho=0, same=0, nc=0, nh=0,
                                  sc=0.0, sh=0.0, sx=0.0, sy=0.0, sxx=0.0, syy=0.0, sxy=0.0)
            q = pairs[key]
            for i in range(body[4]):
                c, h = struct.unpack_from('<ff', body, 16 + 9 * i)
                v = body[16 + 9 * i + 8]
                cv = bool(v & 1) and math.isfinite(c)
                hv = bool(v & 2) and math.isfinite(h)
                ca = cv and bool(v & 4)
                ha = hv and bool(v & 8)
                q['n'] += 1
                if cv:
                    q['nc'] += 1; q['sc'] += c; q['ac'] += ca
                if hv:
                    q['nh'] += 1; q['sh'] += h; q['ah'] += ha
                if cv and hv:
                    q['vb'] += 1
                    q['both'] += ca and ha; q['co'] += ca and not ha; q['ho'] += ha and not ca; q['same'] += (ca == ha)
                    q['sx'] += c; q['sy'] += h; q['sxx'] += c * c; q['syy'] += h * h; q['sxy'] += c * h
        out.append('HUBML/1 shadow_day uid=%s day=%s pairs=%d pairs_over=%d bad=%d%s' % (
            uid, day, len(pairs), 1 if over else 0, bad, (' err=' + errs[day]) if errs and day in errs else ''))
        def tag(num):
            return '%s:%d' % (uid, num) if num else '-'
        for (cn, hn), q in sorted(pairs.items(), key=lambda kv: -kv[1]['n']):    # sorted 는 안정 — 같으면 먼저 나온 쌍
            corr = '-'
            vb = q['vb']
            if vb >= 2:
                vx, vy = vb * q['sxx'] - q['sx'] * q['sx'], vb * q['syy'] - q['sy'] * q['sy']
                if vx > 0 and vy > 0:
                    corr = '%.4f' % ((vb * q['sxy'] - q['sx'] * q['sy']) / math.sqrt(vx * vy))
            out.append('HUBML/1 shadow uid=%s day=%s champ=%s chal=%s n=%d valid_both=%d alarm_champ=%d alarm_chal=%d '
                       'both=%d champ_only=%d chal_only=%d agree=%s mean_champ=%s mean_chal=%s corr=%s' % (
                           uid, day, tag(cn), tag(hn), q['n'], vb, q['ac'], q['ah'], q['both'], q['co'], q['ho'],
                           '%.4f' % (q['same'] / vb) if vb else '-', '%.4f' % (q['sc'] / q['nc']) if q['nc'] else '-',
                           '%.4f' % (q['sh'] / q['nh']) if q['nh'] else '-', corr))
    out.append('HUBML/1 shadow_done n=%d' % len(days))
    return out

def score_scenario(outdir):
    """q12 시나리오 (.inc) + Edge 기대 SCORE 바이트 (.inc)"""
    import numpy as np          # score 모드에서만 (다른 모드는 표준 라이브러리만)
    D = ['20261110', '20261111', '20261112', '20261113', '20261114', '20261115']
    TORN = 3                    # 20261113: 마지막 SCORE 레코드의 꼬리를 자른다 (시험이 fs_truncate) → shadow_day err=
    rng = np.random.default_rng(12)
    def scores(k, base, jit=0.15):
        """챔피언 점수 k 개 + 도전자 점수 (챔피언과 상관 + 잡음). float32 로 고정"""
        c = np.float32(base) * (0.4 + 1.2 * rng.random(k))
        h = np.float32(0.8) * c + np.float32(jit) * rng.standard_normal(k)
        return [(float(np.float32(a)), float(np.float32(b))) for a, b in zip(c, h)]
    def ents(sc, valid=(1, 1), q=None):
        o = []
        for i, (c, h) in enumerate(sc):
            v = 0
            if q and q[i]:
                o.append((0.0, 0.0, 0)); continue
            if valid[0]: v |= 1 | (4 if c >= 1.0 else 0)
            if valid[1]: v |= 2 | (8 if h >= 1.0 else 0)
            o.append((c if valid[0] else 0.0, h if valid[1] else 0.0, v))
        return o
    R = []          # (day 번호, type, 본문, 재전송 횟수)
    seq = [0]
    def add(day, body, typ=11, resend=0):
        R.append((day, typ, body, resend)); seq[0] += 1
    def pairs_of(day):
        return [r for r in R if r[0] == day]
    fs = lambda: seq[0]
    # ── 20261110: 쌍 (1,2) 두 묶음 · 쌍 (1,3) 한 묶음 · EVT 형식 아닌 레코드 · 중복 재전송 · champ 만 valid · quality≠0 ──
    add(0, score_body(fs(), ents(scores(8, 1.0)), 1, 2), resend=1)            # 같은 데이터그램 재전송 (ingest 가 거른다)
    sc = scores(8, 1.1)
    add(0, score_body(fs(), ents(sc, q=[0, 0, 1, 0, 0, 0, 0, 1]), 1, 2))        # quality≠0 두 벡터 → 두 valid 0
    add(0, bytes(24), typ=6)                                                  # MCOL_T_EVT (다른 형식 — 요약이 무시)
    add(0, score_body(fs(), ents(scores(8, 0.9), valid=(1, 0)), 1, 0))        # challenger 빈 슬롯 (num 0, 점수칸 0)
    add(0, score_body(fs(), ents(scores(8, 1.0)), 1, 3))                      # 쌍 (1,3) — 가장 많은 쌍은 (1,2)
    # ── 20261111: champion 빈 묶음 · 정상 묶음 · 깨진 것들 · NaN/inf · 상위 비트 켜짐 ──
    add(1, score_body(fs(), ents(scores(8, 1.0), valid=(0, 1)), 0, 5))        # champion 빈 묶음
    add(1, score_body(fs(), ents(scores(8, 1.2)), 7, 5))
    add(1, score_body(fs(), ents(scores(8, 1.0)), 7, 5, trim=1))              # 길이 1 모자람 → bad
    add(1, score_body(fs(), ents(scores(8, 1.0)), 7, 5, extra=1))             # 길이 1 넘침 → bad
    add(1, score_body(fs(), [], 7, 5, n=0))                                   # n=0 → bad
    add(1, score_body(fs(), ents(scores(8, 1.0)), 7, 5, n=9))                 # n=9 (본문은 8 개분) → bad
    add(1, score_body(fs(), ents(scores(9, 1.0)), 7, 5))                      # n=9 로 길이가 맞아도 → bad
    add(1, score_body(fs(), ents(scores(8, 1.0))[:3], 7, 5, n=3, extra=1))    # n=3 인데 길이 어긋남 → bad
    nan, inf = float('nan'), float('inf')
    a = ents(scores(8, 1.0))
    a[1] = (nan, a[1][1], a[1][2] | 1 | 4)                                    # champion NaN + valid + alarm 비트 → 무효, 경보 무시
    a[4] = (a[4][0], inf, a[4][2] | 2 | 8)                                    # challenger inf + valid + alarm 비트
    a[6] = (-inf, nan, 15)                                                    # 둘 다 비유한
    add(1, score_body(fs(), a, 7, 5))
    b = ents(scores(8, 1.0))
    b = [(c, h, v | 0xf0) for c, h, v in b]
    add(1, score_body(fs(), b, 7, 5, flags=0xff, rsv=0xabcd))                 # flags · rsv · vflags 상위 비트 → 무시하고 센다
    # ── 20261112: 한 벡터(corr '-') · 모두 무효(valid_both 0) ──
    add(2, score_body(fs(), ents(scores(1, 1.0)), 9, 4))
    add(2, score_body(fs(), [(0.0, 0.0, 0)] * 8, 0, 0))                       # 둘 다 빈 슬롯
    # ── 20261113 (torn): 쌍 (1,2) 세 묶음 — 마지막은 파일 꼬리가 잘려 읽히지 않는다 (앞 둘만 합계, shadow_day err=) ──
    add(3, score_body(fs(), ents(scores(8, 1.0)), 1, 2))
    add(3, score_body(fs(), ents(scores(8, 1.1)), 1, 2))
    add(3, score_body(fs(), ents(scores(8, 1.2)), 1, 2))
    # ── 20261114: 벡터 수 동률 두 쌍 (먼저 나온 (5,6) 이 앞) + 큰 유한 점수(1e12 대) 쌍 + 한 벡터 모자란 쌍 ──
    big = [(float(np.float32(1e12 * (1 + 0.1 * i))), float(np.float32(0.9e12 * (1 + 0.1 * i) + (i % 3) * 1e10))) for i in range(8)]
    add(4, score_body(fs(), ents(big), 5, 6))
    add(4, score_body(fs(), ents(scores(8, 1.0)), 3, 4))
    add(4, score_body(fs(), ents(scores(4, 1.0)), 8, 6))
    add(4, score_body(fs(), ents(scores(4, 1.1)), 8, 6))                       # (8,6) 8 벡터 — 앞 두 쌍과도 동률, 세 번째
    # ── 20261115: 서로 다른 쌍 34 개 (n=1 씩) — 33·34 번째 쌍은 어느 줄에도 안 들어가고 pairs=32 pairs_over=1.
    #    쌍 (1,100) 은 두 번 나와 벡터 2 개 → 맨 앞 줄, 넘은 쌍(33)이 다시 나와도 줄에 안 들어간다 ──
    for i in range(34):
        add(5, score_body(fs(), ents(scores(1, 1.0)), i + 1, 100))
    add(5, score_body(fs(), ents(scores(1, 1.0)), 1, 100))
    add(5, score_body(fs(), ents(scores(1, 1.0)), 33, 100))
    files = {}
    for d, typ, body, rs_ in R:
        files.setdefault(D[d], []).append((typ, body))
    files[D[TORN]] = files[D[TORN]][:-1]                                       # 잘린 마지막 레코드는 읽히지 않는다
    days = D + ['20261116']                                                   # 마지막은 파일 없는 날
    lines = shadow_lines(SC_UID, days, files, errs={D[TORN]: '%d'})          # %d = 시험이 -EBADMSG 로 채운다 (libc 마다 값이 다르다)
    unk = shadow_lines('4d50444d000000ff', D[:1], {})                         # 장비 없음 → n=0 줄
    o = ['/* tools/hubml_testdata.py --score-inc 가 만든다 — 손으로 고치지 않는다 (규약·요약 규칙은 그 파일의 shadow_lines 주석) */',
         '#define SS_UID_S "%s"' % SC_UID,
         '#define SS_NREC %d' % len(R),
         '#define SS_TORN_DAY %d' % TORN,
         'static const int SS_DAY_NREC[] = { %s };' % ', '.join(str(len(pairs_of(d))) for d in range(len(D))),
         'struct ss_rec { uint8_t day; uint8_t type; uint8_t resend; uint16_t len; uint8_t body[104]; };',
         'static const char *const SS_DAYS[] = { %s };' % ', '.join('"%s"' % d for d in D),
         'static const struct ss_rec SS_REC[] = {']
    for d, typ, body, rs_ in R:
        assert len(body) <= 104, len(body)
        o.append('\t{ %d, %d, %d, %d, { %s } },' % (d, typ, rs_, len(body), ', '.join(str(x) for x in body)))
    o.append('};')
    o.append('#define SS_DAYS_ARG "%s"' % ','.join(days))
    o.append('static const char *const SS_LINES[] = {')
    o += ['\t"%s",' % l for l in lines]
    o.append('};')
    o.append('#define SS_UNK_ARG "4d50444d000000ff"')
    o.append('#define SS_UNK_DAY "%s"' % D[0])
    o.append('static const char *const SS_UNK_LINES[] = {')
    o += ['\t"%s",' % l for l in unk]
    o.append('};')
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, 'score_scenario.inc'), 'w').write('\n'.join(o) + '\n')
    for l in lines:
        print(l)
    # ── Edge 기대 SCORE 바이트: pv.json 의 벡터 → kind 3 (golden_kind3.xmb) 로 점수 → SCORE 본문 ──
    from xmb_ref import parse_md, score_md
    md = parse_md(open(os.path.join(outdir, 'golden_kind3.xmb'), 'rb').read())
    pv = json.load(open(os.path.join(outdir, 'pv.json')))['vectors']
    xs = [[struct.unpack('<f', struct.pack('<I', int(h, 16)))[0] for h in v['x']] for v in pv]
    cases = [  # (feat_seq, 슬롯 채움 bit0 champ bit1 chal, champ_num, chal_num, 첫 PV 번호, quality)
        (100, 3, 1, 2, 28, [0, 0, 1, 0, 0, 0, 0, 0]),   # 둘 다 채움 · 정상→1X ×1.6 경계 근처 · quality≠0 한 벡터
        (101, 1, 4, 0, 28, [0] * 8),                    # champion 만 (challenger 빈 슬롯 → valid 0)
        (102, 2, 0, 9, 60, [0] * 4),                    # challenger 만 (champion 빈 슬롯)  — n=4 묶음
        (103, 0, 0, 0, 0, [0] * 8),                     # 둘 다 빈 슬롯
    ]
    e = ['/* tools/hubml_testdata.py --score-inc 가 만든다 — 손으로 고치지 않는다 (golden_kind3.xmb · pv.json · xmb_ref.score_md 로 계산한 SCORE 본문) */',
         'struct es_case { uint32_t feat_seq; uint8_t slots; uint32_t champ_num, chal_num; uint8_t first, n; uint8_t q[8]; uint8_t len; uint8_t body[88]; };',
         'static const struct es_case ES_CASE[] = {']
    for fseq, slots, cn, hn, first, q in cases:
        n = len(q)
        ents_ = []
        for i in range(n):
            x = xs[first + i]
            v, c, h = 0, 0.0, 0.0
            if not q[i]:
                if slots & 1:
                    c = float(score_md(md, x)); v |= 1 | (4 if c >= 1.0 else 0)
                if slots & 2:
                    h = float(score_md(md, x)); v |= 2 | (8 if h >= 1.0 else 0)
            ents_.append((c, h, v))
        body = score_body(fseq, ents_, cn if slots & 1 else 0, hn if slots & 2 else 0)
        e.append('\t{ %d, %d, %d, %d, %d, %d, { %s }, %d, { %s } },' % (
            fseq, slots, cn, hn, first, n, ', '.join(str(x) for x in q + [0] * (8 - n)), len(body), ', '.join(str(x) for x in body)))
        print('edge case', fseq, 'alarms champ/chal', sum(1 for _, _, v in ents_ if v & 4), sum(1 for _, _, v in ents_ if v & 8), 'len', len(body))
    e.append('};')
    open(os.path.join(outdir, 'score_edge_expected.inc'), 'w').write('\n'.join(e) + '\n')

if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--score-inc':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        score_scenario(sys.argv[2])
    elif len(sys.argv) > 2 and sys.argv[1] == '--gate-inc':
        gate_scenario(sys.argv[2])
    else:
        main()
