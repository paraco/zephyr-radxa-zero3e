#!/usr/bin/env python3
"""L2/L3 용 시험 GGUF 를 만들고 골든값 헤더를 뽑는다.

**골든값이 내 파서와 독립이다** — 파일은 llama.cpp 의 참조 GGUFWriter 로
만들고, 골든값은 참조 GGUFReader 로 되읽어 뽑는다. 내가 쓴 Zephyr 파서는
그 어느 쪽에도 관여하지 않는다.

한 파일이 두 칸을 받친다:
  L2  헤더 · KV · 텐서 정보(이름·형태·형식·오프셋) · 데이터 시작 정렬
  L3  Q4_0 / Q4_K 블록의 역양자화 (같은 텐서를 그대로 쓴다)

    $ python3 tools/l2_make_gguf.py
      → /tmp/claude-501/l2_test.gguf
      → apps/llm_l2/src/l2_golden.h
"""
import argparse
import os
import pathlib
import sys
import zlib

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
LLAMA = REPO.parent / "upstream" / "llama.cpp"
sys.path.insert(0, str(LLAMA / "gguf-py"))

from gguf import GGUFReader, GGUFValueType, GGUFWriter, GGMLQuantizationType  # noqa: E402
from gguf.quants import quantize  # noqa: E402

ARCH = "moru-l2-test"


def build(path: pathlib.Path) -> None:
    w = GGUFWriter(path, ARCH)

    # ── KV — 종류를 골고루 넣어 파서의 분기를 전부 지나가게 한다
    w.add_uint32("moru.u32", 0xDEADBEEF)
    w.add_float32("moru.f32", 1.5)            # 정확히 표현되는 값을 쓴다
    w.add_bool("moru.bool_true", True)
    w.add_bool("moru.bool_false", False)
    w.add_string("moru.str", "모루랩/MoruLab")   # UTF-8 다바이트
    w.add_array("moru.arr_u32", [1, 2, 3, 4, 5])
    w.add_array("moru.arr_str", ["alpha", "beta", "감마"])

    # ── 텐서 — 차원 수 1~4, 형식 F32/F16/Q4_0/Q4_K
    rng = np.random.default_rng(0x6D6F7275)

    # f32 1차원
    w.add_tensor("t.f32.1d", np.arange(64, dtype=np.float32))
    # f32 2차원
    w.add_tensor("t.f32.2d", rng.standard_normal((8, 32)).astype(np.float32))
    # f16 3차원
    w.add_tensor("t.f16.3d", rng.standard_normal((2, 4, 32)).astype(np.float16))
    # f32 4차원
    w.add_tensor("t.f32.4d", rng.standard_normal((2, 2, 4, 16)).astype(np.float32))

    # ── 양자화 텐서 — L3 가 이 둘을 그대로 쓴다.
    #
    # raw_shape 은 넘기지 않는다. add_tensor_info 가 **바이트 형태**를 받아
    # quant_shape_from_byte_shape() 로 논리 형태를 되돌린다(gguf_writer.py:364).
    # 논리 형태를 넘기면 "32 는 Q4_0 형식 크기 18 의 배수가 아니다" 로 막힌다.

    # Q4_0 (블록 32개 원소 / 18 B) — gguf-py 에 양자화기가 있다
    src_q40 = rng.standard_normal((4, 32)).astype(np.float32)
    w.add_tensor("t.q4_0", quantize(src_q40, GGMLQuantizationType.Q4_0),
                 raw_dtype=GGMLQuantizationType.Q4_0)

    # Q4_K (슈퍼블록 256개 원소 / 144 B) — gguf-py 는 **역양자화만** 있다
    # (quants.py:131 NotImplementedError). 그래서 블록을 직접 짠다.
    #
    # 이게 오히려 L3 에 더 맞다 — 바이트를 내가 정하고 **참조 역양자화기**가
    # 골든 f32 를 주므로 내 구현과 완전히 독립이고, 진짜 양자화기가 뱉지 않는
    # 극단값까지 시험할 수 있다.
    #
    # ⚠ 임의 바이트를 쓰면 안 된다. d/dmin 이 fp16 이라 지수부가 전부 1인
    #   바이트가 나오면 NaN/Inf 가 되어 골든값이 NaN 이 된다. 스케일만 고정하고
    #   나머지(scales 12 B + qs 128 B)는 결정적 난수로 채운다.
    #
    #   블록 구조 (144 B): d fp16 | dmin fp16 | scales[12] 6비트 | qs[128] 4비트
    q4k_rows = []
    for d, dmin in ((np.float16(0.0125), np.float16(-0.5)),
                    (np.float16(-0.03), np.float16(0.25))):   # 음수 스케일도 넣는다
        blk = bytearray()
        blk += np.float16(d).tobytes()
        blk += np.float16(dmin).tobytes()
        blk += bytes(rng.integers(0, 256, size=12, dtype=np.uint8))
        blk += bytes(rng.integers(0, 256, size=128, dtype=np.uint8))
        assert len(blk) == 144, len(blk)
        q4k_rows.append(bytes(blk))
    q4k = np.frombuffer(b"".join(q4k_rows), dtype=np.uint8).reshape(2, 144)
    w.add_tensor("t.q4_k", q4k, raw_dtype=GGMLQuantizationType.Q4_K)

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def emit_golden(gguf: pathlib.Path, header: pathlib.Path) -> None:
    r = GGUFReader(gguf, "r")
    raw = gguf.read_bytes()
    crc = zlib.crc32(raw)

    ver = int(r.fields["GGUF.version"].parts[-1][0])
    ntensor = int(r.fields["GGUF.tensor_count"].parts[-1][0])
    nkv = int(r.fields["GGUF.kv_count"].parts[-1][0])

    lines = [
        "/*",
        " * l2_golden.h — 생성 파일이다. 손으로 고치지 않는다.",
        " *",
        " *   $ python3 tools/l2_make_gguf.py",
        " *",
        " * 출처: llama.cpp 의 참조 GGUFWriter 로 만든 파일을 참조 GGUFReader 로",
        " * 되읽어 뽑았다. apps/llm_l2 의 파서와 독립이다.",
        f" * llama.cpp: {os.popen(f'git -C {LLAMA} rev-parse --short HEAD').read().strip()}",
        " */",
        "#ifndef L2_GOLDEN_H",
        "#define L2_GOLDEN_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define L2_FILE_SIZE      {len(raw)}u",
        f"#define L2_FILE_CRC32     0x{crc:08x}u",
        f"#define L2_VERSION        {ver}u",
        f"#define L2_TENSOR_COUNT   {ntensor}u",
        f"#define L2_KV_COUNT       {nkv}u",
        f"#define L2_ALIGNMENT      {int(r.alignment)}u",
        f"#define L2_DATA_OFFSET    {int(r.data_offset)}u",
        "",
        "struct l2_golden_tensor {",
        "\tconst char *name;",
        "\tuint32_t    n_dims;",
        "\tuint64_t    dims[4];",
        "\tuint32_t    type;",
        "\tuint64_t    offset;",
        "};",
        "",
        "static const struct l2_golden_tensor l2_tensors[] = {",
    ]

    for f in r.tensors:
        # ReaderTensor.shape 은 dims 를 **뒤집은** 값이다(gguf_reader.py:359
        # np_dims = tuple(reversed(...))). 파일에 적힌 순서를 그대로 쓰려면
        # tensor_info 필드에서 읽는다. 텐서 정보 필드는 r.fields 에 들어가지
        # 않고 ReaderTensor.field 로만 온다.
        fld = f.field
        name_len = int(fld.parts[0][0])
        n_dims = int(fld.parts[2][0])
        dims = [int(x) for x in fld.parts[3]]
        dims += [0] * (4 - len(dims))
        ttype = int(fld.parts[4][0])
        toff = int(fld.parts[5][0])
        assert name_len == len(f.name.encode())
        lines.append(
            f'\t{{ "{f.name}", {n_dims}u, {{ {", ".join(f"{d}u" for d in dims)} }}, '
            f"{ttype}u, {toff}u }},"
        )

    lines += [
        "};",
        "",
        "/* KV 중 값까지 확인하는 것들 */",
    ]

    def kvdef(macro: str, key: str):
        fld = r.fields[key]
        t = fld.types[0]
        if t == GGUFValueType.UINT32:
            lines.append(f"#define {macro}_VAL  0x{int(fld.parts[-1][0]):08x}u")
        elif t == GGUFValueType.BOOL:
            lines.append(f"#define {macro}_VAL  {int(fld.parts[-1][0])}")
        elif t == GGUFValueType.STRING:
            s = bytes(fld.parts[-1]).decode()
            lines.append(f'#define {macro}_VAL  "{s}"')
        lines.append(f'#define {macro}_KEY  "{key}"')
        lines.append(f"#define {macro}_TYPE {int(t)}u")

    kvdef("L2_KV_ARCH", "general.architecture")
    kvdef("L2_KV_U32", "moru.u32")
    kvdef("L2_KV_STR", "moru.str")
    kvdef("L2_KV_BOOL_T", "moru.bool_true")
    kvdef("L2_KV_BOOL_F", "moru.bool_false")

    arr = r.fields["moru.arr_u32"]
    lines.append(f'#define L2_KV_ARR_KEY   "moru.arr_u32"')
    lines.append(f"#define L2_KV_ARR_ELEMT {int(arr.types[1])}u")
    lines.append(f"#define L2_KV_ARR_LEN   {len(arr.data)}u")

    f32 = r.fields["moru.f32"]
    lines.append(f'#define L2_KV_F32_KEY   "moru.f32"')
    lines.append(f"#define L2_KV_F32_BITS  0x{int(np.float32(float(f32.parts[-1][0])).view(np.uint32)):08x}u")

    lines += ["", "#endif /* L2_GOLDEN_H */", ""]
    header.write_text("\n".join(lines), encoding="utf-8")

    print(f"파일      {gguf}  ({len(raw)} B)")
    print(f"CRC32     0x{crc:08x}")
    print(f"텐서 {ntensor}개 · KV {nkv}개 · 정렬 {int(r.alignment)} · 데이터 시작 {int(r.data_offset)}")
    print(f"골든 헤더  {header}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/claude-501/l2_test.gguf")
    ap.add_argument("--header", default=str(REPO / "apps/llm_l2/src/l2_golden.h"))
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    hdr = pathlib.Path(a.header)
    hdr.parent.mkdir(parents=True, exist_ok=True)
    build(out)
    emit_golden(out, hdr)


if __name__ == "__main__":
    main()
