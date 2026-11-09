"""Compares two PNG images with the renderer's tolerance.

  compare_images.py A.png B.png [--diff DIFF.png]

A is the actual image, B the expected one. They match when they have the
same size, at least 98% of their pixels are within 8/255 on every channel
(RGBA), and the mean absolute difference over all channels is at most
2/255 (docs/milestone-specs.md M7, "Legacy image comparison"), and, so that
a missing sprite or one drawn with the fallback texture cannot pass on the
rest of the image (a drawn pixel is more than 8/255 from the background,
B's most frequent colour; an object is an 8-connected group of one image's
drawn pixels):
  - at least 90% of the pixels drawn in either image are within 8/255
    (ADR-065);
  - every object of either image of at least 8 pixels has at least 25% of
    its pixels within 8/255 (ADR-069);
  - at most 8 solid mismatches, pixels that with all 8 neighbours are drawn
    in one image and background in the other (ADR-069);
  - at most 8 pixels of A show the fallback magenta (255, 0, 255, 255)
    where B does not (ADR-069).
tests/render_gpu/support/image_compare.h applies the same rule to the
golden images and render_shot --compare. On a mismatch the per-channel
difference, scaled by 8, is written as an opaque PNG (default: next to B,
named <B>.diff.png). Exit status 0 when they match, 1 when not, 2 on a
file error. Python 3, stdlib only (8-bit non-interlaced PNGs: grey, RGB,
palette, grey with alpha, RGBA).
"""

import argparse
import collections
import pathlib
import struct
import sys
import zlib

PIXEL_TOLERANCE = 8
MIN_WITHIN_FRACTION = 0.98
MAX_MEAN_ABS_DIFF = 2.0
MIN_DRAWN_WITHIN_FRACTION = 0.90
MIN_OBJECT_WITHIN_FRACTION = 0.25
MIN_OBJECT_PIXELS = 8
MAX_SOLID_MISMATCHES = 8
MAX_FALLBACK_PIXELS = 8
FALLBACK_COLOUR = bytes((255, 0, 255, 255))
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


def background(rgba):
    """The most frequent RGBA colour (the first one on a tie)."""
    counts = collections.Counter(
        rgba[p:p + 4] for p in range(0, len(rgba), 4))
    return counts.most_common(1)[0][0] if counts else bytes(4)


def _off(pixels, p, colour):
    return any(abs(pixels[p + c] - colour[c]) > PIXEL_TOLERANCE
               for c in range(4))


def objects(width, height, mask):
    """The 8-connected groups of the pixel indices in the set |mask|."""
    seen = set()
    groups = []
    for start in sorted(mask):
        if start in seen:
            continue
        seen.add(start)
        stack = [start]
        group = []
        while stack:
            p = stack.pop()
            group.append(p)
            x, y = p % width, p // width
            for ny in range(max(0, y - 1), min(height, y + 2)):
                for nx in range(max(0, x - 1), min(width, x + 2)):
                    q = ny * width + nx
                    if q in mask and q not in seen:
                        seen.add(q)
                        stack.append(q)
        groups.append(group)
    return groups


def compare(a, b):
    """Statistics of two (width, height, rgba) images; B is the expected
    one, whose most frequent colour is the background."""
    (wa, ha, pa), (wb, hb, pb) = a, b
    result = {"same_size": (wa, ha) == (wb, hb), "pixels": wa * ha,
              "within": 0, "mean_abs_diff": 0.0, "max_diff": 0,
              "drawn": 0, "drawn_within": 0, "objects": 0,
              "worst_object_within": 1.0, "worst_object_pixels": 0,
              "solid_mismatches": 0, "fallback_pixels": 0}
    if not result["same_size"]:
        return result
    back = background(pb)
    total = 0
    within = 0
    worst_all = 0
    within_set = set()
    drawn_a = set()
    drawn_b = set()
    fallback = 0
    for p in range(0, len(pa), 4):
        worst = 0
        for c in range(4):
            delta = abs(pa[p + c] - pb[p + c])
            total += delta
            if delta > worst:
                worst = delta
        index = p // 4
        if worst <= PIXEL_TOLERANCE:
            within += 1
        if worst > worst_all:
            worst_all = worst
        off_a = _off(pa, p, back)
        off_b = _off(pb, p, back)
        if off_a:
            drawn_a.add(index)
        if off_b:
            drawn_b.add(index)
        if (off_a or off_b) and worst <= PIXEL_TOLERANCE:
            within_set.add(index)
        if (not _off(pa, p, FALLBACK_COLOUR) and
                _off(pb, p, FALLBACK_COLOUR)):
            fallback += 1
    drawn = drawn_a | drawn_b
    result["within"] = within
    result["mean_abs_diff"] = total / max(1, len(pa))
    result["max_diff"] = worst_all
    result["drawn"] = len(drawn)
    result["drawn_within"] = len(within_set)
    result["fallback_pixels"] = fallback
    for mask in (drawn_a, drawn_b):
        for group in objects(wa, ha, mask):
            if len(group) < MIN_OBJECT_PIXELS:
                continue
            fraction = sum(1 for q in group if q in within_set) / len(group)
            result["objects"] += 1
            if fraction < result["worst_object_within"]:
                result["worst_object_within"] = fraction
                result["worst_object_pixels"] = len(group)
    mismatched = drawn_a ^ drawn_b
    solid = 0
    for p in mismatched:
        x, y = p % wa, p // wa
        if x == 0 or y == 0 or x == wa - 1 or y == ha - 1:
            continue
        if all(p + dy * wa + dx in mismatched
               for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
            solid += 1
    result["solid_mismatches"] = solid
    return result


def drawn_fraction(result):
    if result["drawn"] == 0:
        return 1.0
    return result["drawn_within"] / result["drawn"]


def matches(result):
    if not result["same_size"]:
        return False
    fraction = result["within"] / max(1, result["pixels"])
    return (fraction >= MIN_WITHIN_FRACTION and
            result["mean_abs_diff"] <= MAX_MEAN_ABS_DIFF and
            drawn_fraction(result) >= MIN_DRAWN_WITHIN_FRACTION and
            result["worst_object_within"] >= MIN_OBJECT_WITHIN_FRACTION and
            result["solid_mismatches"] <= MAX_SOLID_MISMATCHES and
            result["fallback_pixels"] <= MAX_FALLBACK_PIXELS)


def describe(result):
    if not result["same_size"]:
        return "the sizes differ"
    fraction = result["within"] / max(1, result["pixels"])
    return (f"{100 * fraction:.3f}% of {result['pixels']} pixels within "
            f"{PIXEL_TOLERANCE}/255 (need {100 * MIN_WITHIN_FRACTION:.0f}%), "
            f"mean absolute difference {result['mean_abs_diff']:.3f}/255 "
            f"(at most {MAX_MEAN_ABS_DIFF}), largest {result['max_diff']}; "
            f"{100 * drawn_fraction(result):.3f}% of {result['drawn']} drawn "
            f"pixels within (need {100 * MIN_DRAWN_WITHIN_FRACTION:.0f}%); "
            f"worst of {result['objects']} objects "
            f"{100 * result['worst_object_within']:.1f}% of "
            f"{result['worst_object_pixels']} pixels within (need "
            f"{100 * MIN_OBJECT_WITHIN_FRACTION:.0f}%); "
            f"{result['solid_mismatches']} solid mismatches (at most "
            f"{MAX_SOLID_MISMATCHES}); {result['fallback_pixels']} "
            f"fallback-magenta pixels (at most {MAX_FALLBACK_PIXELS})")


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
