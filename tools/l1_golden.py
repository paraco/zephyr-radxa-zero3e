#!/usr/bin/env python3
"""L1 골든 CRC32 생성기.

apps/llm_l1/src/main.c 의 pattern_word() 와 **같은 식**으로 패턴을 만들고
CRC32 를 계산한다. 식이 바뀌면 양쪽을 같이 바꿔야 한다.

    word[i] = (uint32)(i * 2654435761) ^ 0x6D6F7275      (리틀엔디안)

Zephyr 의 crc32_ieee 는 다항식 0xedb88320 · 초기 ~0 · 최종 반전이라
zlib.crc32 와 같다(subsys/crc/crc32_sw.c 확인). 청크로 나눠 이어 계산하는
것도 zlib 와 의미가 같다.
"""
import argparse
import zlib

import numpy as np

MULT = np.uint32(2654435761)
XOR = np.uint32(0x6D6F7275)


def golden(nbytes: int, chunk_words: int = 1 << 22) -> int:
    assert nbytes % 4 == 0, "4의 배수여야 한다"
    nwords = nbytes // 4
    crc = 0
    done = 0
    while done < nwords:
        n = min(chunk_words, nwords - done)
        idx = np.arange(done, done + n, dtype=np.uint32)
        words = (idx * MULT) ^ XOR
        crc = zlib.crc32(words.astype("<u4").tobytes(), crc)
        done += n
    return crc


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", default="640M",
                    help="영역 크기. 예: 640M, 64M, 0x28000000 (기본 640M)")
    a = ap.parse_args()

    s = a.size.strip()
    if s.lower().endswith("m"):
        nbytes = int(s[:-1]) * 1024 * 1024
    elif s.lower().endswith("k"):
        nbytes = int(s[:-1]) * 1024
    else:
        nbytes = int(s, 0)

    crc = golden(nbytes)
    print(f"크기      {nbytes} B  ({nbytes / 1024 / 1024:g} MiB)")
    print(f"골든 CRC32  0x{crc:08x}")
    print()
    print("빌드에 넣는 법:")
    print(f"  apps/llm_l1/src/main.c 의 L1_GOLDEN_CRC32 를 0x{crc:08x}u 로 바꾼다")


if __name__ == "__main__":
    main()
