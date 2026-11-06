#!/usr/bin/env python3
"""OTA 수신 자동 시험 — 셸 포트와 데이터 포트를 함께 다룬다.

    python3 tools/ota_test.py [보낼파일]

기본 파일은 sdcard/zephyr.bin 이다. 하는 일:
  1. CDC 포트 둘을 찾는다 (번호 작은 쪽이 셸, 큰 쪽이 OTA 데이터)
  2. `moru usb` 로 dwc3 상태를 찍는다 (DALEPENA 로 EP 설정 판정)
  3. 파일 크기·CRC32 를 계산해 `moru ota begin` 을 보낸다
  4. 데이터 포트로 부어 넣는다 (O_NONBLOCK — 블록하지 않는다)
  5. `moru ota status` · `moru ota verify` 로 결과를 읽는다

⚠ 사람이 tio 로 셸 포트를 열어 두면 **이 스크립트가 포트를 열 수 없다.**
  먼저 tio 를 닫는다 (ctrl-t q).

⚠ 보드에 쓰는 것은 RAM(0x50000000)뿐이고 SD 카드는 건드리지 않는다.
  적용 경로(boot.scr)가 아직 없으므로 되돌릴 것이 없다.

SPDX-License-Identifier: Apache-2.0
"""
import glob
import os
import select
import sys
import termios
import time
import tty
import zlib


def open_raw(path, flags):
    fd = os.open(path, flags | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def drain(fd, secs):
    out = b''
    t0 = time.time()
    while time.time() - t0 < secs:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                out += os.read(fd, 8192)
            except OSError:
                pass
    return out.decode('utf-8', 'replace')


def shell_cmd(fd, line, wait=2.5):
    os.write(fd, (line + '\r\n').encode())
    return drain(fd, wait)


def show(title, text):
    print('── %s ' % title + '─' * max(0, 58 - len(title)))
    for ln in text.splitlines():
        ln = ln.rstrip()
        if ln and not ln.startswith('Moru >') or '>' in ln and len(ln) > 8:
            print('  ' + ln)
    print()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'sdcard', 'zephyr.bin')
    path = os.path.abspath(path)

    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if len(ports) < 2:
        print('CDC 포트가 둘이 아니다: %r' % ports)
        print('보드가 켜져 있는지, 두 번째 CDC 가 열거됐는지 확인한다.')
        return 1
    sh_port, data_port = ports[0], ports[1]
    print('셸   포트: %s' % sh_port)
    print('데이터 포트: %s' % data_port)

    data = open(path, 'rb').read()
    size = len(data)
    crc = zlib.crc32(data) & 0xffffffff
    print('보낼 파일: %s' % path)
    print('크기 %d 바이트 · CRC32 %08x' % (size, crc))
    print()

    try:
        sh = open_raw(sh_port, os.O_RDWR)
    except OSError as e:
        print('셸 포트를 열 수 없다 (%s) — tio 가 잡고 있으면 먼저 닫는다.' % e)
        return 1

    try:
        drain(sh, 0.5)
        show('moru usb (전송 전)', shell_cmd(sh, 'moru usb'))
        show('moru ota begin', shell_cmd(sh, 'moru ota begin %d %08x' % (size, crc)))

        print('── 전송 ' + '─' * 52)
        fd = open_raw(data_port, os.O_WRONLY)
        t0 = time.time()
        n = 0
        eagain = 0
        while n < size and time.time() - t0 < 60:
            try:
                n += os.write(fd, data[n:n + 4096])
            except OSError as e:
                import errno as E
                if e.errno in (E.EAGAIN, E.EWOULDBLOCK):
                    eagain += 1
                    time.sleep(0.005)
                    continue
                raise
        el = time.time() - t0
        os.close(fd)
        print('  보낸 바이트 %d / %d · %.2f 초 · %.0f B/s' % (n, size, el, n / max(el, 1e-3)))
        print('  EAGAIN %d회 (장치가 흐름 제어를 한 횟수 — 받고 있다는 뜻)' % eagain)
        print()

        time.sleep(1.0)
        show('moru ota status', shell_cmd(sh, 'moru ota status', 3.0))
        show('moru ota verify', shell_cmd(sh, 'moru ota verify', 12.0))
    finally:
        os.close(sh)
    return 0


if __name__ == '__main__':
    sys.exit(main())
