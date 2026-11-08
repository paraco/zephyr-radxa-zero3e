#!/usr/bin/env python3
"""hubllm 에 질문을 보내고 답을 흘려 받는다 (HUBLLM/1, apps/hubllm/src/hub.h).

    python3 tools/hub_chat.py "질문"                    # 한 번 (greedy)
    python3 tools/hub_chat.py --temp 0.7 --topk 20 --topp 0.8 --pres 1.5 --seed 7 "질문"
    python3 tools/hub_chat.py --verify "질문"           # 호스트 greedy 와 토큰 열 대조
    python3 tools/hub_chat.py --chat                    # 대화 (KV 이어가기, 빈 줄로 끝)
    python3 tools/hub_chat.py --raw "<|im_start|>user\\nhi<|im_end|>\\n"

기본은 Qwen 채팅 틀을 씌운다:
    <|im_start|>user\\n{질문}<|im_end|>\\n<|im_start|>assistant\\n
--chat 의 둘째 질문부터는 보드 KV 에 이어 붙일 **새 부분만** 보낸다
(보드는 keep=1 이면 마지막 생성 토큰 <|im_end|> 까지 KV 에 넣어 둔다):
    \\n<|im_start|>user\\n{질문}<|im_end|>\\n<|im_start|>assistant\\n

⚠ tio 로 셸 포트를 열어 두면 포트를 못 연다 (ctrl-t q).

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import base64
import os
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from moru_q import find_shell_port  # noqa: E402
from ota_test import open_raw  # noqa: E402

MODEL = os.path.expanduser('~/Projects/PiZZa/models/Qwen3.5-0.8B-Q4_K_M.gguf')
HOST_REF = '/tmp/claude-501/hub_host_ref'	# tools/hub_host_ref.cpp 를 빌드한 것


def send(fd, line):
    data = (line + '\r').encode()
    for k in range(0, len(data), 64):	# 보드 셸 수신 버퍼가 넘치지 않게
        os.write(fd, data[k:k + 64])
        time.sleep(0.005)


def drain(fd, t=0.3):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                os.read(fd, 65536)
            except BlockingIOError:
                pass


def gen(fd, text, a, rid, keep):
    b64 = base64.b64encode(text.encode()).decode()
    opt = f' temp={a.temp} topk={a.topk} topp={a.topp} seed={a.seed}' if a.temp > 0 else ''
    if a.pres or a.freq or a.rep != 1.0:
        opt += f' pres={a.pres} freq={a.freq} rep={a.rep} last_n={a.last_n}'
    send(fd, f'hub gen id={rid} max={a.max} special=1 keep={1 if keep else 0}{opt} b64={b64}')
    buf, prompt_ids, tids = b'', [], []
    deadline = time.time() + a.timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            buf += os.read(fd, 4096)
        except BlockingIOError:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            s = line.decode('utf-8', 'replace').strip()
            i = s.find('HUBLLM/1 ')
            if i < 0:
                continue
            f = s[i:].split(' ')
            kv = dict(x.split('=', 1) for x in f[2:] if '=' in x)
            if kv.get('id') != str(rid):
                continue
            if f[1] == 'prompt':
                prompt_ids = [int(x) for x in kv['ids'].split(',') if x]
            elif f[1] == 'tok':
                tids.append(int(kv['tid']))
                piece = base64.b64decode(f[-1]) if '=' not in f[-1] or f[-1].endswith('=') else b''
                sys.stdout.write(piece.decode('utf-8', 'ignore'))
                sys.stdout.flush()
            elif f[1] in ('done', 'err'):
                print(f'\n\n[{s[i:]}]')
                gen.last_stop = kv.get('stop', '')
                return (f[1] == 'done'), prompt_ids, tids
    print('\n[시간 초과]')
    return False, prompt_ids, tids


def verify(prompt_ids, tids, n):
    if not os.path.exists(HOST_REF):
        print(f'[대조 생략 — {HOST_REF} 없음. tools/hub_host_ref.cpp 머리말대로 빌드]')
        return
    out = subprocess.run([HOST_REF, MODEL, str(n), ','.join(map(str, prompt_ids))],
                         capture_output=True, text=True, timeout=600).stdout
    host = [int(x) for x in out.strip().split('=', 1)[1].split(',') if x]
    k = 0
    while k < min(len(host), len(tids)) and host[k] == tids[k]:
        k += 1
    same = (host == tids)
    print(f'[호스트 대조: {"✓ 전부 같다" if same else "✗ 다르다"} — 일치 {k}/{len(tids)} '
          f'(보드 {len(tids)} · 호스트 {len(host)})]')
    if not same and k < min(len(host), len(tids)):
        print(f'[첫 차이 위치 {k}: 보드 {tids[k]} · 호스트 {host[k]} — '
              f'호스트의 1·2등 차이는 HUB_MARGIN=1 {HOST_REF} … 로 본다]')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('question', nargs='?')
    ap.add_argument('--max', type=int, default=128)
    ap.add_argument('--temp', type=float, default=0.0)
    ap.add_argument('--topk', type=int, default=20)
    ap.add_argument('--topp', type=float, default=0.95)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--pres', type=float, default=0.0, help='presence_penalty (Qwen 권장 1.5)')
    ap.add_argument('--freq', type=float, default=0.0)
    ap.add_argument('--rep', type=float, default=1.0)
    ap.add_argument('--last-n', dest='last_n', type=int, default=64)
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--chat', action='store_true')
    ap.add_argument('--verify', action='store_true')
    ap.add_argument('--timeout', type=float, default=900)
    a = ap.parse_args()

    port = os.environ.get('MORU_SHELL') or find_shell_port()
    if not port:
        print('셸 포트가 없다 (/dev/cu.usbmodem*)', file=sys.stderr)
        return 2
    fd = open_raw(port, os.O_RDWR)
    try:
        send(fd, '')
        drain(fd)
        rid = int(time.time()) % 100000
        if a.chat:
            send(fd, 'hub reset')	# 앞 대화를 비우고 시작한다
            drain(fd, 1.0)
            first = True
            while True:
                try:
                    q = input('\n질문> ').strip()
                except EOFError:
                    break
                if not q:
                    break
                # 앞 답이 <|im_end|>(eos) 로 끝나지 않았으면(max·abort) 여기서 닫는다
                head = '' if first else (
                    '\n' if getattr(gen, 'last_stop', '') == 'eos' else '<|im_end|>\n')
                text = f'{head}<|im_start|>user\n{q}<|im_end|>\n<|im_start|>assistant\n'
                ok, _, _ = gen(fd, text, a, rid, keep=True)
                first = False
                rid += 1
                if not ok:
                    break
            return 0
        if not a.question:
            ap.error('질문이 필요하다 (또는 --chat)')
        text = (a.question.encode().decode('unicode_escape').encode('latin-1').decode()
                if a.raw else f'<|im_start|>user\n{a.question}<|im_end|>\n<|im_start|>assistant\n')
        ok, pids, tids = gen(fd, text, a, rid, keep=False)
        if ok and a.verify:
            if a.temp > 0 or a.pres or a.freq or a.rep != 1.0:
                print('[대조 생략 — 확률 샘플링·벌점은 호스트 greedy 와 비교하지 않는다]')
            else:
                verify(pids, tids, a.max)
        return 0 if ok else 1
    finally:
        os.close(fd)


if __name__ == '__main__':
    sys.exit(main())
