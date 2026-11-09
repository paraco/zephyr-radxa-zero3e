#!/usr/bin/env python3
"""ledger_verify — hubml 원장 검사 (정본 규격 MoruLab_MLOps design/MLCC_LEDGER_SPEC.md `mlledger/1`).

사용:  python3 tools/ledger_verify.py <ledger.jsonl | 로그(LEDGER 줄)>
       python3 tools/ledger_verify.py --seed <로그> <출력 폴더>   3자 시험 씨앗(정상·변조·꼬리 잘림·모르는 kind) 만들기
규칙:  줄 해시 = sha256(저장된 줄 바이트, 끝 \\n 제외) · 첫 줄 prev = "0"*64 · seq 1 부터 1 씩 ·
       모르는 kind·칸은 멈추지 않고 사슬만 본다 · 끝 줄이 \\n 없이 끝나면 잘린 꼬리(torn)
판정:  {"ok", "lines", "head", "bad_seq", "torn_bytes", "aliases"}
"""
import hashlib, json, os, sys

def verify(data: bytes):
    torn = 0
    if data and not data.endswith(b'\n'):
        cut = data.rfind(b'\n') + 1
        torn, data = len(data) - cut, data[:cut]
    lines = [l for l in data.split(b'\n') if l]
    prev, alias, bad = '0' * 64, {}, None
    for i, l in enumerate(lines, start=1):
        try:
            j = json.loads(l)
        except ValueError:
            bad = i; break
        if j.get('seq') != i or j.get('prev') != prev:
            bad = i; break
        prev = hashlib.sha256(l).hexdigest()
        if j.get('kind') == 'alias':
            alias[f"{j['uid']}/{j['name']}"] = j['model_id']
    return {'ok': bad is None, 'lines': (bad - 1) if bad else len(lines), 'head': prev, 'bad_seq': bad,
            'torn_bytes': torn, 'aliases': alias}

EXCL = ('v', 'seq', 'prev', 'ts', 'actor', 'sig', 'approval_head')

def verify_approvals(data: bytes, pk_hex: str):
    """mlledger/1 v0.2 §4: 승인 줄에서 본문을 다시 만들어 ed25519(sha256(정규 본문)) 검증 (Mac cryptography)"""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    import base64
    key = Ed25519PublicKey.from_public_bytes(bytes.fromhex(pk_hex))
    n = good = 0
    for l in [l for l in data.split(b'\n') if l]:
        j = json.loads(l)
        if 'approval_head' not in j:
            continue
        n += 1
        body = {k: v for k, v in j.items() if k not in EXCL}
        body['v'] = 1; body['head'] = j['approval_head']
        canon = json.dumps(body, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
        try:
            key.verify(base64.b64decode(j['sig']['b64']), hashlib.sha256(canon).digest()); good += 1
        except Exception as e:
            print(f'✗ seq {j["seq"]} 서명 다름: {canon[:120]!r}')
    return n, good

def from_log(path):
    raw = [l.rstrip(b'\r') for l in open(path, 'rb').read().split(b'\n')]
    got = [l[len(b'LEDGER '):] for l in raw if l.startswith(b'LEDGER ')]
    return b''.join(l + b'\n' for l in got) if got else open(path, 'rb').read()

def seed(log, out):
    os.makedirs(out, exist_ok=True)
    good = from_log(log)
    lines = good.split(b'\n')[:-1]
    cases = {}
    cases['ledger_sample.jsonl'] = good
    t = bytearray(good); k = good.find(b'"size":') + 7
    t[k:k + 1] = b'3' if t[k:k + 1] == b'1' else b'1'
    cases['ledger_tampered.jsonl'] = bytes(t)
    cases['ledger_torn.jsonl'] = good + b'{"v":1,"seq":' + str(len(lines) + 1).encode() + b',"prev":"ab'
    # 모르는 kind 를 사슬에 맞게 덧붙인다 — 읽는 쪽은 멈추지 않아야 한다
    head = hashlib.sha256(lines[-1]).hexdigest()
    extra = json.dumps({'v': 1, 'seq': len(lines) + 1, 'prev': head, 'ts': '2026-11-09T12:00:00Z', 'kind': 'future_thing',
                        'hub': 'x', 'actor': 'hub', 'zzz': {'a': 1}}, separators=(',', ':')).encode()
    cases['ledger_unknown_kind.jsonl'] = good + extra + b'\n'
    exp = {}
    for name, data in cases.items():
        open(os.path.join(out, name), 'wb').write(data)
        exp[name] = verify(data)
    json.dump(exp, open(os.path.join(out, 'ledger_expected.json'), 'w'), indent=1, ensure_ascii=False)
    for n, e in exp.items():
        print(f'{n}: ok={e["ok"]} lines={e["lines"]} bad_seq={e["bad_seq"]} torn={e["torn_bytes"]}')

def main():
    if sys.argv[1] == '--seed':
        return seed(sys.argv[2], sys.argv[3])
    if sys.argv[1] == '--pk':
        data = from_log(sys.argv[3])
        n, good = verify_approvals(data, sys.argv[2])
        print(f'승인 줄 {n} 개 · Mac cryptography 서명 검증 {good}/{n}')
        if n == 0 or good != n:
            sys.exit(1)
        sys.argv = [sys.argv[0], sys.argv[3]]
    r = verify(from_log(sys.argv[1]))
    print(('사슬 정상' if r['ok'] else f'✗ {r["bad_seq"]} 번에서 깨짐') + f' · {r["lines"]} 줄 · 머리 {r["head"][:16]}… · 잘린 꼬리 {r["torn_bytes"]} B · 별칭 ' +
          ', '.join(f'{a}={m}' for a, m in sorted(r['aliases'].items())))
    sys.exit(0 if r['ok'] else 1)

if __name__ == '__main__':
    main()
