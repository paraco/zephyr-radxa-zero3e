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


def open_raw(path, flags, baud=115200):
    '''raw 모드로 열고 **보율까지 설정**한다.

    ⚠ 보율 설정을 빠뜨리면 CDC(USB)에서는 무해하지만 **UART 에서는 한
      글자도 못 읽는다.** 진단 빌드는 셸이 UART2(115200)에 있으므로
      필수다 — 실제로 이것을 빠뜨려 셸 출력이 전부 비었다.
    '''
    fd = os.open(path, flags | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    spd = getattr(termios, 'B%d' % baud, None)
    if spd is not None:
        a[4] = spd   # ispeed
        a[5] = spd   # ospeed
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
    '''받은 것을 거의 그대로 보여 준다.

    ⚠ 필터를 공격적으로 두면 드라이버 로그를 버린다 — 진단에서 그것이
    가장 중요한 줄이다. ANSI 색만 지우고 빈 줄만 접는다.
    '''
    import re
    print('── %s ' % title + '─' * max(0, 58 - len(title)))
    text = re.sub(r'\x1b\[[0-9;]*m', '', text)
    prev_blank = False
    for ln in text.splitlines():
        ln = ln.rstrip()
        if not ln:
            if not prev_blank:
                print()
            prev_blank = True
            continue
        prev_blank = False
        print('  ' + ln)
    print()


def main():
    import argparse
    ap = argparse.ArgumentParser(description='OTA 수신 자동 시험')
    ap.add_argument('file', nargs='?', help='보낼 파일 (기본 sdcard/zephyr.bin)')
    ap.add_argument('--shell', help='셸 포트. 진단 빌드면 UART '
                                    '(예: /dev/cu.usbserial-0001)')
    ap.add_argument('--data', help='OTA 데이터 포트 (기본: usbmodem 둘째)')
    ap.add_argument('--bytes', type=int, default=0,
                    help='앞에서 N 바이트만 보낸다 (0=전체). 진단용 — '
                         'CDC 로그를 INF 로 켠 상태에서는 64 정도만 보낸다')
    ap.add_argument('--wait', type=float, default=3.0,
                    help='전송 뒤 로그를 모을 시간(초)')
    ap.add_argument('--baud', type=int, default=115200,
                    help='셸 포트 보율. UART 셸(진단 빌드)에 필수')
    ap.add_argument('--chunk', type=int, default=64,
                    help='한 번에 write 할 바이트. ⚠ 크게 하면 커널 TTY '
                         '버퍼에만 쌓이고 USB 로 나가지 않는다(실측). '
                         'bulk mps(64)에 맞추고 매번 tcdrain 한다')
    ap.add_argument('--drain', action='store_true', default=True,
                    help='청크마다 tcdrain 으로 실제 송신을 기다린다')
    args = ap.parse_args()

    path = args.file or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'sdcard', 'zephyr.bin')
    path = os.path.abspath(path)

    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    sh_port = args.shell
    data_port = args.data
    if data_port is None:
        if len(ports) < 2:
            print('CDC 포트가 둘이 아니다: %r' % ports)
            print('보드가 켜져 있는지, 두 번째 CDC 가 열거됐는지 확인한다.')
            return 1
        data_port = ports[1]
    if sh_port is None:
        if not ports:
            print('CDC 포트가 없다.')
            return 1
        sh_port = ports[0]
    print('셸   포트: %s' % sh_port)
    print('데이터 포트: %s' % data_port)

    data = open(path, 'rb').read()
    if args.bytes > 0:
        data = data[:args.bytes]
    size = len(data)
    crc = zlib.crc32(data) & 0xffffffff
    print('보낼 파일: %s' % path)
    print('크기 %d 바이트 · CRC32 %08x' % (size, crc))
    print()

    try:
        sh = open_raw(sh_port, os.O_RDWR, args.baud)
    except OSError as e:
        print('셸 포트를 열 수 없다 (%s) — tio 가 잡고 있으면 먼저 닫는다.' % e)
        return 1

    try:
        drain(sh, 0.5)
        show('moru usb (전송 전)', shell_cmd(sh, 'moru usb'))
        show('moru ota begin', shell_cmd(sh, 'moru ota begin %d %08x' % (size, crc)))

        print('── 전송 ' + '─' * 52)
        fd = open_raw(data_port, os.O_WRONLY)   # CDC — 보율 무관
        t0 = time.time()
        n = 0
        eagain = 0
        # ⚠ 청크를 크게 하면 커널 TTY 버퍼에만 들어가고 USB 로 나가지
        #   않는다. 4096 으로 보냈을 때 호스트는 "EAGAIN 0회로 다 보냈다"
        #   고 했지만 보드는 0 바이트였다. bulk mps(64)에 맞추고 매번
        #   tcdrain 으로 실제 송신을 기다린다.
        chunk = max(1, args.chunk)
        while n < size and time.time() - t0 < 180:
            try:
                w = os.write(fd, data[n:n + chunk])
                n += w
                if args.drain:
                    termios.tcdrain(fd)
            except OSError as e:
                import errno as E
                if e.errno in (E.EAGAIN, E.EWOULDBLOCK):
                    eagain += 1
                    time.sleep(0.002)
                    continue
                raise
        el = time.time() - t0
        os.close(fd)
        print('  보낸 바이트 %d / %d · %.2f 초 · %.0f B/s' % (n, size, el, n / max(el, 1e-3)))
        print('  EAGAIN %d회 (장치가 흐름 제어를 한 횟수 — 받고 있다는 뜻)' % eagain)
        print()

        # 전송 직후의 드라이버 로그를 그대로 모은다 — 수신 경로 진단의 핵심
        show('전송 직후 로그 (드라이버가 찍는 것)', drain(sh, args.wait))

        show('moru ota status', shell_cmd(sh, 'moru ota status', 3.0))
        if args.bytes == 0:
            show('moru ota verify', shell_cmd(sh, 'moru ota verify', 12.0))
        else:
            print('(--bytes 로 일부만 보냈으므로 verify 는 건너뛴다)\n')
    finally:
        os.close(sh)
    return 0


if __name__ == '__main__':
    sys.exit(main())
