#!/usr/bin/env python3
"""hubllm 1416 MHz 장시간 시험 — 같은 질문을 greedy 로 되풀이해 토큰 열이 매번 같은지 본다.

    python3 tools/hub_soak.py --minutes 10

순서:
  1. hub clk 1104 → 기준 토큰 열을 2 번 (둘이 같아야 한다 — 결정성 확인)
  2. hub clk 1416 → 같은 질문을 --minutes 동안 되풀이
     매번: 토큰 열 = 기준? · guard_fail = 0? · clk = 1416? · tg_tps
  3. hub clk 1104 로 되돌리고 요약. 한 번이라도 어긋나면 그 자리에서 1104 로 내리고 멈춘다.

클럭은 계산 결과를 바꾸지 않으므로(같은 코드·같은 순서) 1416 의 토큰 열은 1104 와
**비트 단위로 같아야** 한다. 다르면 불안정(타이밍 오류)을 의심한다.
결과는 log/hub-soak-<시각>.csv 에도 남긴다.

⚠ 온도 센서(tsadc) 드라이버가 없어 온도는 기록하지 못한다.

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import base64
import os
import select
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from moru_q import find_shell_port  # noqa: E402
from ota_test import open_raw  # noqa: E402

Q = '저압 선로가 활선인지 확인하는 방법을 간단히 알려줘.'


def send(fd, line):
    data = (line + '\r').encode()
    for k in range(0, len(data), 64):
        os.write(fd, data[k:k + 64])
        time.sleep(0.005)


def lines(fd, until, timeout):
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            buf += os.read(fd, 4096)
        except BlockingIOError:
            continue
        while b'\n' in buf:
            ln, buf = buf.split(b'\n', 1)
            s = ln.decode('utf-8', 'replace')
            i = s.find('HUBLLM/1 ')
            if i >= 0:
                s = s[i:].strip()
                yield s
                if until(s):
                    return
    yield 'TIMEOUT'


def kv(s):
    return dict(x.split('=', 1) for x in s.split(' ')[2:] if '=' in x)


def clk(fd, mhz):
    send(fd, f'hub clk {mhz}')
    for s in lines(fd, lambda s: s.startswith('HUBLLM/1 clk') or s.startswith('HUBLLM/1 err'), 30):
        if s.startswith('HUBLLM/1 clk'):
            return kv(s)
        if s.startswith('HUBLLM/1 err') or s == 'TIMEOUT':
            return {'err': s}
    return {'err': 'none'}


def gen(fd, rid, max_gen):
    text = f'<|im_start|>user\n{Q}<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
    b64 = base64.b64encode(text.encode()).decode()
    send(fd, f'hub gen id={rid} max={max_gen} special=1 keep=0 b64={b64}')
    tids = []
    for s in lines(fd, lambda s: (s.startswith('HUBLLM/1 done') or s.startswith('HUBLLM/1 err'))
                   and f'id={rid}' in s, 300):
        if s.startswith('HUBLLM/1 tok') and f'id={rid}' in s:
            tids.append(int(kv(s)['tid']))
        elif s.startswith('HUBLLM/1 done') and f'id={rid}' in s:
            return tids, kv(s)
        elif s.startswith('HUBLLM/1 err') or s == 'TIMEOUT':
            return tids, {'err': s}
    return tids, {'err': 'none'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--minutes', type=float, default=10)
    ap.add_argument('--max', type=int, default=128)
    a = ap.parse_args()
    port = os.environ.get('MORU_SHELL') or find_shell_port()
    if not port:
        print('셸 포트가 없다', file=sys.stderr)
        return 2
    os.makedirs(os.path.join(HERE, '..', 'log'), exist_ok=True)
    csvp = os.path.join(HERE, '..', 'log', time.strftime('hub-soak-%Y%m%dT%H%M%S.csv'))
    out = open(csvp, 'w')
    out.write('run,clk,elapsed_s,n_gen,same_as_ref,tg_tps,ttft_ms,guard_fail,mhz\n')
    fd = open_raw(port, os.O_RDWR)
    rid = int(time.time()) % 100000
    bad = 0
    try:
        send(fd, '')
        time.sleep(0.5)
        try:
            os.read(fd, 65536)
        except BlockingIOError:
            pass
        c = clk(fd, 1104)
        print(f'[1104] {c}')
        ref, d = gen(fd, rid, a.max); rid += 1
        ref2, d2 = gen(fd, rid, a.max); rid += 1
        print(f'[기준 1104] {len(ref)} 토큰 · tg {d.get("tg_tps")} · 두 번째와 같다: {ref == ref2}')
        out.write(f'0,1104,0,{len(ref)},{int(ref == ref2)},{d.get("tg_tps")},{d.get("ttft_ms")},'
                  f'{d.get("guard_fail")},{c.get("mhz")}\n')
        if ref != ref2 or not ref:
            print('⚠ 1104 에서도 두 번이 다르다 — 시험의 전제가 깨졌다. 멈춘다')
            return 1
        c = clk(fd, 1416)
        print(f'[1416] {c}')
        if c.get('state') != '1416':
            print('⚠ 1416 으로 올라가지 않았다 — 멈춘다 (UART2 로그의 [clk] 줄을 본다)')
            return 1
        t0, run = time.time(), 0
        while time.time() - t0 < a.minutes * 60:
            run += 1
            tids, d = gen(fd, rid, a.max); rid += 1
            same = (tids == ref)
            el = time.time() - t0
            ok = same and d.get('clk') == '1416' and d.get('guard_fail') == '0' and 'err' not in d
            print(f'  {run:3d}  {el:6.0f} s  {len(tids):3d} 토큰  {"같다" if same else "**다르다**"}'
                  f'  tg {d.get("tg_tps")}  clk {d.get("clk")}  guard_fail {d.get("guard_fail")}'
                  f'{"" if ok else "  ⚠"}')
            out.write(f'{run},{d.get("clk")},{el:.0f},{len(tids)},{int(same)},{d.get("tg_tps")},'
                      f'{d.get("ttft_ms")},{d.get("guard_fail")},\n')
            out.flush()
            if not ok:
                bad += 1
                k = next((i for i in range(min(len(tids), len(ref))) if tids[i] != ref[i]), None)
                print(f'  ⚠ 어긋남 — 첫 차이 {k} · {d}')
                break
        c = clk(fd, 1104)
        print(f'[끝 → 1104] {c}')
        print(f'\n요약: 1416 에서 {run} 회 · {(time.time() - t0) / 60:.1f} 분 · 어긋남 {bad} · CSV {csvp}')
        return 1 if bad else 0
    finally:
        os.close(fd)
        out.close()


if __name__ == '__main__':
    sys.exit(main())
