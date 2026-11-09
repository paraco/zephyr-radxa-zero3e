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
import base64, json, os, struct, sys, zlib

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

if __name__ == '__main__':
    main()
