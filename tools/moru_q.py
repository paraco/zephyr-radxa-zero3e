#!/usr/bin/env python3
"""보드 셸에 명령을 보내고 응답만 받아 온다 (한 번만).

    python3 tools/moru_q.py "moru info" "kernel heap"

ota_test.py 와 달리 **아무것도 전송하지 않는다** — 상태만 읽는다.
부팅 로그만 보고 "동작한다" 고 판정하지 않기 위한 도구다.

⚠ tio 로 셸 포트를 열어 두면 이 스크립트가 포트를 열 수 없다 (ctrl-t q).

SPDX-License-Identifier: Apache-2.0
"""
import glob
import os
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ota_test import open_raw  # noqa: E402


def find_shell_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        return None
    return ports[0]	# 번호 작은 쪽이 셸 (ota_test.py 와 같은 규약)


def query(fd, cmd, settle=0.6):
    os.write(fd, (cmd + '\r').encode())
    out = b''
    deadline = time.time() + settle
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            except OSError as e:
                # ⚠ 재부팅시키는 명령(`moru reboot`, `moru ota apply`)은 USB
                #   포트를 사라지게 한다 — Errno 6 "Device not configured".
                #   그것은 **정상 동작**이므로 예외로 죽지 않고 알려 준다.
                out += f'\n  [포트가 사라졌다 — 보드가 재부팅한 것으로 보인다: {e}]'.encode()
                break
            if chunk:
                out += chunk
                deadline = time.time() + settle	# 더 오면 기다린다
    return out.decode('utf-8', 'replace')


def main():
    cmds = sys.argv[1:] or ['moru info']
    port = os.environ.get('MORU_SHELL') or find_shell_port()
    if not port:
        print('셸 포트를 못 찾았다 (/dev/cu.usbmodem*). 보드가 붙어 있는가?',
              file=sys.stderr)
        return 2
    print(f'셸 포트: {port}')
    fd = open_raw(port, os.O_RDWR)
    try:
        query(fd, '', settle=0.3)	# 프롬프트 깨우고 버퍼 비우기
        for c in cmds:
            print(f'\n── {c} ' + '─' * max(0, 60 - len(c)))
            for line in query(fd, c).splitlines():
                line = line.strip()
                if line and line != c:
                    print('  ' + line)
    finally:
        os.close(fd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
