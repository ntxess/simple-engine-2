"""Compares two PNG images with the renderer's tolerance.

  compare_images.py A.png B.png [--diff DIFF.png]

Two images match when they have the same size, at least 98% of their
pixels are within 8/255 on every channel (RGBA), and the mean absolute
difference over all channels is at most 2/255 (docs/milestone-specs.md M7,
"Legacy image comparison"; tests/render_gpu/support/image_compare.h applies
the same rule to the golden images). On a mismatch the per-channel
difference, scaled by 8, is written as an opaque PNG (default: next to B,
named <B>.diff.png). Exit status 0 when they match, 1 when not, 2 on a
file error. Python 3, stdlib only (8-bit non-interlaced PNGs: grey, RGB,
palette, grey with alpha, RGBA).
"""

import argparse
import pathlib
import struct
import sys
import zlib

PIXEL_TOLERANCE = 8
MIN_WITHIN_FRACTION = 0.98
MAX_MEAN_ABS_DIFF = 2.0
SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PngError(Exception):
    pass


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """(width, height, RGBA bytes) of an 8-bit non-interlaced PNG."""
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(SIGNATURE):
        raise PngError(f"{path}: not a PNG")
    pos = len(SIGNATURE)
    width = height = color_type = None
    palette = b""
    transparency = b""
    idat = []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8 or interlace != 0:
                raise PngError(f"{path}: only 8-bit non-interlaced PNGs")
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            transparency = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if width is None:
        raise PngError(f"{path}: no IHDR")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise PngError(f"{path}: colour type {color_type} not supported")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    at = 0
    for _ in range(height):
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                line[i] = (line[i] + _paeth(left, up, up_left)) & 0xFF
            elif kind != 0:
                raise PngError(f"{path}: bad filter {kind}")
        rows.append(bytes(line))
        previous = line
    rgba = bytearray(width * height * 4)
    out = 0
    for line in rows:
        for x in range(width):
            if color_type == 6:
                rgba[out:out + 4] = line[x * 4:x * 4 + 4]
            elif color_type == 2:
                rgba[out:out + 4] = line[x * 3:x * 3 + 3] + b"\xff"
            elif color_type == 0:
                g = line[x]
                rgba[out:out + 4] = bytes((g, g, g, 255))
            elif color_type == 4:
                g, a = line[x * 2], line[x * 2 + 1]
                rgba[out:out + 4] = bytes((g, g, g, a))
            else:
                index = line[x]
                r, g, b = palette[index * 3:index * 3 + 3]
                a = transparency[index] if index < len(transparency) else 255
                rgba[out:out + 4] = bytes((r, g, b, a))
            out += 4
    return width, height, bytes(rgba)


def write_png(path, width, height, rgba):
    """Writes RGBA bytes as an 8-bit PNG (filter 0)."""
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride]
                   for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    pathlib.Path(path).write_bytes(
        SIGNATURE + chunk(b"IHDR", header) +
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def compare(a, b):
    """Statistics of two (width, height, rgba) images."""
    (wa, ha, pa), (wb, hb, pb) = a, b
    result = {"same_size": (wa, ha) == (wb, hb), "pixels": wa * ha,
              "within": 0, "mean_abs_diff": 0.0, "max_diff": 0}
    if not result["same_size"]:
        return result
    total = 0
    within = 0
    worst_all = 0
    for p in range(0, len(pa), 4):
        worst = 0
        for c in range(4):
            delta = abs(pa[p + c] - pb[p + c])
            total += delta
            if delta > worst:
                worst = delta
        if worst <= PIXEL_TOLERANCE:
            within += 1
        if worst > worst_all:
            worst_all = worst
    result["within"] = within
    result["mean_abs_diff"] = total / max(1, len(pa))
    result["max_diff"] = worst_all
    return result


def matches(result):
    if not result["same_size"]:
        return False
    fraction = result["within"] / max(1, result["pixels"])
    return (fraction >= MIN_WITHIN_FRACTION and
            result["mean_abs_diff"] <= MAX_MEAN_ABS_DIFF)


def describe(result):
    if not result["same_size"]:
        return "the sizes differ"
    fraction = result["within"] / max(1, result["pixels"])
    return (f"{100 * fraction:.3f}% of {result['pixels']} pixels within "
            f"{PIXEL_TOLERANCE}/255 (need {100 * MIN_WITHIN_FRACTION:.0f}%), "
            f"mean absolute difference {result['mean_abs_diff']:.3f}/255 "
            f"(at most {MAX_MEAN_ABS_DIFF}), largest {result['max_diff']}")


def diff_image(a, b):
    (wa, ha, pa), (wb, hb, pb) = a, b
    width, height = min(wa, wb), min(ha, hb)
    out = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            ia = (y * wa + x) * 4
            ib = (y * wb + x) * 4
            io = (y * width + x) * 4
            for c in range(3):
                out[io + c] = min(255, abs(pa[ia + c] - pb[ib + c]) * 8)
            out[io + 3] = 255
    return width, height, bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--diff")
    args = parser.parse_args()
    try:
        a = read_png(args.a)
        b = read_png(args.b)
    except (OSError, PngError, zlib.error) as error:
        print(f"compare_images: {error}")
        return 2
    result = compare(a, b)
    print(f"compare_images: {args.a} ({a[0]}x{a[1]}) vs {args.b} "
          f"({b[0]}x{b[1]}): {describe(result)}")
    if matches(result):
        print("compare_images: match")
        return 0
    diff = args.diff or str(pathlib.Path(args.b).with_suffix("")) + ".diff.png"
    write_png(diff, *diff_image(a, b))
    print(f"compare_images: MISMATCH; difference written to {diff}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
