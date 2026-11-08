#!/usr/bin/env python3
"""hubllm 에 질문을 보내고 답을 흘려 받는다 (HUBLLM/1, apps/hubllm/src/hub.h).

    python3 tools/hub_chat.py "저압 선로가 활선인지 어떻게 알 수 있나?"
    python3 tools/hub_chat.py --max 200 --raw "<|im_start|>user\\nhi<|im_end|>\\n"

기본은 Qwen 채팅 틀을 씌운다 (L7b 'chat' 과 같은 꼴):
    <|im_start|>user\\n{질문}<|im_end|>\\n<|im_start|>assistant\\n
--raw 면 그대로 보낸다. 특수 토큰은 parse_special=1 로 읽힌다.

⚠ tio 로 셸 포트를 열어 두면 포트를 못 연다 (ctrl-t q).

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import base64
import os
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from moru_q import find_shell_port  # noqa: E402
from ota_test import open_raw  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('question')
    ap.add_argument('--max', type=int, default=128)
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--timeout', type=float, default=600)
    a = ap.parse_args()

    text = a.question.encode().decode('unicode_escape').encode('latin-1').decode() if a.raw else (
        f'<|im_start|>user\n{a.question}<|im_end|>\n<|im_start|>assistant\n')
    b64 = base64.b64encode(text.encode()).decode()
    rid = int(time.time()) % 100000
    cmd = f'hub gen id={rid} max={a.max} special=1 b64={b64}\r'

    port = os.environ.get('MORU_SHELL') or find_shell_port()
    if not port:
        print('셸 포트가 없다 (/dev/cu.usbmodem*)', file=sys.stderr)
        return 2
    fd = open_raw(port, os.O_RDWR)
    try:
        os.write(fd, b'\r')
        time.sleep(0.3)
        try:
            os.read(fd, 65536)
        except BlockingIOError:
            pass
        os.write(fd, cmd.encode())
        buf = b''
        out = b''
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
                s = s[i:]
                f = s.split(' ')
                if f[1] == 'tok' and f'id={rid}' in f:
                    piece = base64.b64decode(f[-1]) if not f[-1].startswith('t=') else b''
                    out += piece
                    sys.stdout.write(piece.decode('utf-8', 'ignore'))
                    sys.stdout.flush()
                elif f[1] == 'prompt':
                    print(f'[{s}]', file=sys.stderr)
                elif f[1] in ('done', 'err'):
                    print(f'\n\n[{s}]')
                    return 0 if f[1] == 'done' else 1
        print('\n[시간 초과]')
        return 3
    finally:
        os.close(fd)


if __name__ == '__main__':
    sys.exit(main())
