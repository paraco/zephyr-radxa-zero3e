#!/usr/bin/env python3
"""USB suspend/resume 시험 — 관측은 uart2, 조작은 USB 셸.

    python3 tools/pm_test.py [--seconds 90] [--no-apply] [--raw]

왜 이 구조인가
--------------
셸·콘솔이 USB 에 있어서 **USB 가 죽으면 관측도 함께 죽는다.** suspend 를
받으려면 호스트가 포트를 유휴로 봐야 하는데, 포트를 열어 두면 유휴가 아니다.
그래서 조작만 USB 로 하고(보낸 뒤 즉시 닫는다), 관측은 전부 uart2 로 받는다.
app.overlay 의 `chosen zephyr,log-uart` 가 그 길을 만든다.

⚠ 이 보드는 **같은 USB-C OTG 포트에서 PD 로 급전된다**(VCC 미연결).
  그래서 관측 어댑터를 보드와 **같은 허브에 꽂으면 안 된다** — 보드가
  재부팅할 때의 전류 변동으로 어댑터가 버스에서 떨어진다(실측 2회, 둘 다
  OTA apply 재부팅 직후). 어댑터는 맥에 직접, 또는 다른 허브에 꽂는다.
  그래도 떨어질 수 있다고 보고 이 스크립트는 **다시 열어 붙는다.**

판정
----
  resume [link-to-ON]      정상 경로가 돈다 — 설계가 성립한다
  resume [usb-reset]       리셋만이 건져 냈다. 리셋이 오지 않는 suspend 에서는
                           영구히 막힌다는 뜻이므로 2단계를 다시 설계해야 한다
  suspend 뒤 resume 없음   막혔다. 전원을 다시 넣으면 pm 꺼짐으로 돌아온다
  Pre-kernel 줄이 다시 나옴 재부팅했다 — suspend 와 무관한 전원 사건일 수 있다

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import glob
import os
import re
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ota_test import open_raw  # noqa: E402

UART_GLOB = '/dev/cu.usbserial*'
USB_GLOB = '/dev/cu.usbmodem*'

# uart2 는 모든 로그를 받으므로 그대로 두면 읽을 수 없다. 관심 있는 줄만 남긴다.
KEEP = re.compile(r'(pm |suspend|resume|USB reset|connected|Pre-kernel|'
                  r'MoruLab Zephyr RTOS 0|is up|FATAL|ASSERT|E: )')
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def usb_ports():
    return sorted(glob.glob(USB_GLOB))


def uart_path():
    m = sorted(glob.glob(UART_GLOB))
    return m[0] if m else None


def open_uart():
    """uart2 어댑터를 연다. 없거나 아직 준비 안 됐으면 None.

    ⚠ 열거 직후에는 tcsetattr 가 EINVAL 을 낸다(실측). 영구 실패가 아니므로
      예외로 보고 다음 주기에 다시 시도한다.
    """
    p = uart_path()
    if p is None:
        return None
    try:
        return open_raw(p, os.O_RDWR, 115200)
    except Exception:
        return None


def shell(cmd, wait=1.0):
    """USB 셸에 한 줄 보내고 **바로 닫는다.**

    열어 둔 채로는 호스트가 포트를 유휴로 보지 않아 suspend 가 오지 않는다.
    """
    p = usb_ports()
    if not p:
        return False
    try:
        fd = open_raw(p[0], os.O_RDWR)
    except Exception:
        return False
    # ⚠ 프롬프트를 먼저 깨운다 — 포트를 열고 곧바로 쓰면 명령이 닿지 않는다
    #   (실측: apply 가 전달되지 않아 "적용 실패" 로 오진했다)
    os.write(fd, b'\r\n')
    time.sleep(0.3)
    os.write(fd, cmd.encode() + b'\r\n')
    time.sleep(wait)
    os.close(fd)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=90.0,
                    help='pm on 뒤 관측할 시간')
    ap.add_argument('--no-apply', action='store_true',
                    help='OTA apply·재부팅을 건너뛰고 지금 펌웨어로 시험')
    ap.add_argument('--raw', action='store_true',
                    help='uart2 를 거르지 않고 전부 찍는다')
    args = ap.parse_args()

    u = open_uart()
    if u is None:
        print('⚠ uart2 어댑터를 찾지 못했다 (%s).' % UART_GLOB)
        print('  어댑터는 보드와 **다른** USB 포트에 꽂는다 — 같은 허브면')
        print('  보드 재부팅 전류에 떨어진다.')
        return 1
    while select.select([u], [], [], 0)[0]:
        os.read(u, 4096)
    print('uart2: %s 열림' % uart_path())

    if not args.no_apply:
        if not shell('moru ota apply', 1.5):
            print('⚠ USB 셸 포트가 없다 — tio/Xconsole 이 쥐고 있지 않은지 본다')
            return 1
        print('apply 보냄 — 재부팅을 기다린다')
        t = time.time()
        while time.time() - t < 60 and len(usb_ports()) >= 2:
            time.sleep(0.5)
        t = time.time()
        while time.time() - t < 90 and len(usb_ports()) < 2:
            time.sleep(0.5)
        time.sleep(3.0)
        if len(usb_ports()) < 2:
            print('⚠ USB 포트가 돌아오지 않았다')

    if not shell('moru usb pm on', 1.5):
        print('⚠ pm on 을 보내지 못했다')
        return 1
    print('### pm on 보냈고 USB 포트를 닫았다 — %.0f 초 동안 uart2 만 본다'
          % args.seconds)
    print('=' * 60)

    buf = b''
    shown = 0
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        if u is None:
            u = open_uart()
            if u is None:
                time.sleep(1.0)
                continue
            print('[%5.1fs] uart2 다시 붙었다' % (time.time() - t0))
        if not select.select([u], [], [], 0.5)[0]:
            continue
        try:
            d = os.read(u, 8192)
        except OSError:
            os.close(u)
            u = None
            print('[%5.1fs] ⚠ uart2 가 떨어졌다 — 다시 열기를 시도한다'
                  % (time.time() - t0))
            continue
        if not d:
            continue
        buf += d
        while b'\n' in buf[shown:]:
            i = buf.index(b'\n', shown)
            line = ANSI.sub('', buf[shown:i].decode('utf-8', 'replace')).rstrip()
            shown = i + 1
            if args.raw or KEEP.search(line):
                print('[%5.1fs] %s' % (time.time() - t0, line))
                sys.stdout.flush()

    if u is not None:
        os.close(u)
    print('=' * 60)
    txt = ANSI.sub('', buf.decode('utf-8', 'replace'))
    sus = len(re.findall(r'suspend \(L2\)', txt))
    res = re.findall(r'resume \[([a-z-]+)\]', txt)
    boots = len(re.findall(r'Pre-kernel', txt))
    print('suspend(L2) %d회 · resume %d회 %s · 부팅 %d회'
          % (sus, len(res), sorted(set(res)) or '', boots))
    if sus == 0:
        print('→ suspend 가 오지 않았다. 포트를 쥐고 있는 것이 없는지,')
        print('  호스트가 이 포트를 재우는지 확인한다.')
    elif len(res) < sus:
        print('→ ⚠ resume 이 모자라다. 막혔을 수 있다 — 전원을 다시 넣으면')
        print('  pm 꺼짐 기본값으로 돌아온다.')
    elif 'link-to-ON' in res:
        print('→ ✅ 정상 경로(link-to-ON)가 돈다.')
    else:
        print('→ ⚠ 정상 경로가 돌지 않았다. 리셋이 건져 낸 것뿐이라면,')
        print('  리셋 없는 suspend 에서는 영구히 막힌다. 설계를 다시 본다.')
    if boots:
        print('→ 재부팅 %d회. suspend 와 무관한 전원 사건일 수 있다 —' % boots)
        print('  보드는 같은 USB-C 포트에서 급전된다.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
