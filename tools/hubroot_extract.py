#!/usr/bin/env python3
"""hubroot_extract — QEMU 로그의 FILE_BEGIN/FILE64/FILE_END 블록으로 허브 root 폴더를 되살린다 (Studio 폴더 열기 시험용).

사용:  python3 tools/hubroot_extract.py <로그> <출력 폴더>
"""
import base64, os, sys

def main():
    log, out = sys.argv[1], sys.argv[2]
    cur, size, chunks, n = None, 0, [], 0
    for l in open(log, errors='replace').read().splitlines():
        l = l.rstrip('\r')
        if l.startswith('FILE_BEGIN '):
            _, cur, size = l.split(' '); size = int(size); chunks = []
        elif l.startswith('FILE64 ') and cur:
            chunks.append(l[7:])
        elif l == 'FILE_END' and cur:
            data = base64.b64decode(''.join(chunks))
            assert len(data) == size, f'{cur}: {len(data)} != {size}'
            p = os.path.join(out, cur); os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data); n += 1; cur = None
    print(f'{n} 개 파일 → {out}')

if __name__ == '__main__':
    main()
