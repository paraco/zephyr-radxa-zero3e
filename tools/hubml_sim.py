#!/usr/bin/env python3
"""hubml_sim — Mac 에서 가짜 Edge 가 되어 실기 허브(hubllm)의 USB 셸로 HUB ML 을 끝까지 돌린다.

    python3 tools/hubml_sim.py [--hours 4] [--uid 4d50444d000000e1] [--port /dev/cu.usbmodemXXXX]

  ml status → ml time set(지금 UTC) → ml rx(HELLO) → tx HELLO_ACK 에서 sid → FEAT(8 벡터 묶음, 1 Hz 가짜 정상,
  부하 0.4/0.8 이 2 시간마다) + HB 를 ml rx 로 → tx ACK 로 받은 수 확인 → ml commit → ml dev ls · ml ds ls →
  ml train → ml model ls → ml ledger 를 받아 사슬 검사(tools/ledger_verify.py 규칙).
  운반은 USB 셸(MLCC 합의 1단계). 실제 Edge(HaLow)는 같은 MCOL/1 데이터그램을 UDP 로 보낸다.
"""
import argparse, base64, glob, os, re, select, struct, sys, time, zlib, hashlib, json
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from ota_test import open_raw                     # noqa: E402
from ledger_verify import verify                  # noqa: E402

HDR = 24
def mcol(typ, sid, stream, seq, pl, edge_ms=0, boot=1):
    h = b'MC' + bytes([1, typ]) + struct.pack('<HBBIQHH', sid, stream, 0, seq, edge_ms, len(pl), boot)
    return h + pl + struct.pack('<I', zlib.crc32(h + pl))

def unmcol(d):
    if d[:3] != b'MC\x01': return None
    typ = d[3]; sid, stream, flags, seq, ems, ln, boot = struct.unpack_from('<HBBIQHH', d, 4)
    if zlib.crc32(d[:HDR + ln]) != struct.unpack_from('<I', d, HDR + ln)[0]: return None
    return typ, sid, stream, seq, d[HDR:HDR + ln]

class Hub:
    def __init__(self, port):
        self.fd = open_raw(port, os.O_RDWR); self.buf = b''
    def cmd(self, c, wait=0.4, until=None, timeout=60):
        # ⚠ 64 B 씩 5 ms 간격 — USB CDC 셸의 입력 링이 작아 1.5 KB 한 줄을 한 번에 쓰면 넘쳐 셸이 줄을 잃는다
        #   (2026-11-09 실기: ml rx FEAT 1,547 자가 전부 사라졌다. hub_chat.py 의 send 와 같은 방식)
        data = (c + '\r').encode()
        for k in range(0, len(data), 64):
            os.write(self.fd, data[k:k + 64]); time.sleep(0.005)
        out, end, last = [], time.time() + timeout, time.time()
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try: self.buf += os.read(self.fd, 65536); last = time.time()
                except BlockingIOError: pass
                while b'\n' in self.buf:
                    l, self.buf = self.buf.split(b'\n', 1)
                    l = re.sub(rb'\x1b\[[0-9;]*[A-Za-z]', b'', l).decode('utf-8', 'replace').strip()
                    i = l.find('HUBML/1')
                    if i >= 0: out.append(l[i:])
                    if until and until in l: return out
            elif not until and time.time() - last > wait:
                return out
        return out

def tx(lines, typ):
    for l in lines:
        m = re.match(r'HUBML/1 tx addr=\d+ b64=(\S+)', l)
        if m:
            r = unmcol(base64.b64decode(m.group(1)))
            if r and r[0] == typ: yield r

def gen(t, rng):
    load = (0.8 if (t // 7200) % 2 else 0.4) + 0.02 * rng.standard_normal()
    z = rng.standard_normal(4); x = np.empty(34)
    for i in range(34):
        base = 1.0 + 0.1 * i / 34
        m = base * (0.5 + load) if 16 <= i <= 25 else base * (1.0 + 0.2 * load)
        m += sum(0.05 * base * np.sin(i * (j + 1) + 1.0) * z[j] for j in range(4))
        x[i] = m + 0.02 * base * rng.standard_normal()
    x[26] = 1.0 + 0.005 * rng.standard_normal(); x[27] = load
    return x.astype(np.float32)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', type=float, default=4); ap.add_argument('--uid', default='4d50444d000000e1')
    ap.add_argument('--port', default=None)
    a = ap.parse_args()
    port = a.port or sorted(glob.glob('/dev/cu.usbmodem*'))[0]
    hub, uid = Hub(port), int(a.uid, 16)
    print(f'셸 {port} · 장비 {a.uid} · {a.hours} 시간치')
    print(hub.cmd('ml status --kv', wait=2.0))
    now = int(time.time() * 1000)
    print(hub.cmd(f'ml time set {now}'))
    hello = struct.pack('<QIHBBII', uid, 0x0100, 34, 34, 0, 0x5e5a0001, 0x4d463334)
    sid = None
    for _ in range(5):
        r = list(tx(hub.cmd('ml rx b64=' + base64.b64encode(mcol(1, 0, 0, 0, hello)).decode(), wait=0.5), 2))
        if r: sid = struct.unpack_from('<H', r[0][4])[0]; break
    assert sid, 'HELLO_ACK 없음'
    print(f'HELLO_ACK sid={sid}')
    rng = np.random.default_rng(777); nvec = int(a.hours * 3600); ndg = nvec // 8
    acks, t0 = {}, time.time()
    for sq in range(ndg):
        xs = [gen(sq * 8 + i, rng) for i in range(8)]
        pl = bytes([8, 34, 0, 0]) + b''.join(x.tobytes() + bytes([0, 0]) + struct.pack('<H', 1000) for x in xs)
        lines = hub.cmd('ml rx b64=' + base64.b64encode(mcol(3, sid, 1, sq, pl, sq * 8000)).decode(), wait=0.0, timeout=0.05)
        if sq % 5 == 0:
            lines += hub.cmd('ml rx b64=' + base64.b64encode(mcol(7, sid, 4, sq // 5, bytes(24))).decode(), wait=0.0, timeout=0.05)
        for r in tx(lines, 8):
            for o in range(0, len(r[4]), 9):
                acks[r[4][o]] = struct.unpack_from('<I', r[4], o + 1)[0]
        if sq % 200 == 0: print(f'  FEAT {sq}/{ndg} · ACK feat {acks.get(1, 0)}', flush=True)
    dt = time.time() - t0
    lines = hub.cmd('ml commit', wait=1.5)
    for r in tx(lines, 8):
        for o in range(0, len(r[4]), 9): acks[r[4][o]] = struct.unpack_from('<I', r[4], o + 1)[0]
    print(f'보냄 FEAT {ndg} 데이터그램({nvec} 벡터) · {dt:.1f} s ({ndg * 1180 / dt / 1024:.0f} KiB/s) · 마지막 ACK feat={acks.get(1)} hb={acks.get(4)}')
    for l in hub.cmd('ml dev ls --kv', wait=1.0): print(' ', l)
    for l in hub.cmd(f'ml ds ls uid={a.uid} --kv', wait=1.0): print(' ', l)
    day = time.strftime('%Y%m%d', time.gmtime(now / 1000))
    tr = hub.cmd(f'ml train uid={a.uid} days={day}', until='HUBML/1 train', timeout=300)
    for l in tr: print(' ', l)
    for l in hub.cmd(f'ml model ls uid={a.uid}', wait=1.0): print(' ', l)
    led = hub.cmd('ml ledger since=1 max=200', until='ledger_done', timeout=60)
    data = b''.join(base64.b64decode(re.search(r'b64=(\S+)', l).group(1)) + b'\n' for l in led if l.startswith('HUBML/1 ledger seq='))
    v = verify(data)
    done = [l for l in led if 'ledger_done' in l]
    ok_head = bool(done) and f'head={v["head"]}' in done[0]
    print(f'원장 {v["lines"]} 줄 · 사슬 {"정상" if v["ok"] else "깨짐"} · head 일치 {ok_head} · 별칭 {v["aliases"]}')
    good = acks.get(1) == ndg and any('verdict=pass' in l for l in tr) and v['ok'] and ok_head
    print('=== hubml_sim', 'PASS' if good else 'FAIL')
    sys.exit(0 if good else 1)

if __name__ == '__main__':
    main()
