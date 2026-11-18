"""Tests compare_images.py on synthetic PNGs (CTest tools.compare_images).

Writes small RGBA images with compare_images.write_png, reads them back,
and checks the tolerance rule at its edges: identical images, a uniform
shift inside the per-pixel tolerance that still breaks the mean, a few
far-off pixels below and above 2%, a size mismatch, an image missing its
sparse drawn pixels, a sprite missing, added, recoloured or shifted by one
pixel, overlapping sprites in another order, a sprite missing from an
overlapping group, the fallback magenta inside a group (ADR-069), and the
diff image of a mismatch. Python 3, stdlib only.
"""

import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import compare_images  # noqa: E402  pylint: disable=wrong-import-position


def image(width, height, value):
    return width, height, bytes([value] * (width * height * 4))


def with_pixels(base, count, value):
    width, height, data = base
    out = bytearray(data)
    for p in range(count):
        out[p * 4:p * 4 + 4] = bytes([value] * 4)
    return width, height, bytes(out)


BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)
BLUE = (40, 60, 200, 255)
GREY = (150, 150, 150, 255)
MAGENTA = (255, 0, 255, 255)


def canvas(width, height, colour):
    return [width, height, bytearray(bytes(colour) * (width * height))]


def rect(image, x0, y0, w, h, colour):
    width = image[0]
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            image[2][(y * width + x) * 4:(y * width + x + 1) * 4] = bytes(
                colour)
    return image


def frozen(image):
    return image[0], image[1], bytes(image[2])


def sprite_cases(check):
    """The object, solid-mismatch and fallback conditions (ADR-069), as in
    tests/render_gpu/image_compare_test.cc."""
    def result(actual, expected):
        return compare_images.compare(frozen(actual), frozen(expected))

    def two_squares():
        return rect(rect(canvas(200, 100, BLACK), 20, 20, 30, 30, WHITE),
                    120, 40, 30, 30, BLUE)

    expected = two_squares()
    same = result(expected, expected)
    check(compare_images.matches(same) and same["objects"] == 4,
          "two squares match themselves (4 objects)")
    missing = result(rect(two_squares(), 120, 40, 30, 30, BLACK), expected)
    check(missing["worst_object_within"] == 0.0 and
          missing["worst_object_pixels"] == 900 and
          not compare_images.matches(missing), "a missing sprite fails")
    added = result(rect(two_squares(), 80, 5, 6, 6, WHITE), expected)
    check(added["worst_object_pixels"] == 36 and
          not compare_images.matches(added), "an added sprite fails")
    recoloured = result(rect(two_squares(), 120, 40, 30, 30, GREY), expected)
    check(not compare_images.matches(recoloured),
          "a recoloured sprite fails")
    shifted = rect(rect(canvas(200, 100, BLACK), 21, 20, 30, 30, WHITE),
                   120, 41, 30, 30, BLUE)
    shifted = result(shifted, expected)
    check(shifted["solid_mismatches"] == 0 and
          compare_images.matches(shifted),
          "sprites shifted by one pixel match")

    order_b = rect(rect(canvas(200, 100, BLACK), 20, 20, 40, 40, WHITE),
                   55, 30, 40, 40, BLUE)
    order_a = rect(rect(canvas(200, 100, BLACK), 55, 30, 40, 40, BLUE),
                   20, 20, 40, 40, WHITE)
    order = result(order_a, order_b)
    check(order["drawn_within"] < order["drawn"] and
          order["solid_mismatches"] == 0 and compare_images.matches(order),
          "overlapping sprites in another order match")

    group_b = rect(rect(canvas(200, 100, BLACK), 20, 20, 50, 50, WHITE),
                   66, 30, 10, 10, BLUE)
    group_a = rect(canvas(200, 100, BLACK), 20, 20, 50, 50, WHITE)
    group = result(group_a, group_b)
    check(group["worst_object_within"] >=
          compare_images.MIN_OBJECT_WITHIN_FRACTION and
          group["solid_mismatches"] > compare_images.MAX_SOLID_MISMATCHES and
          not compare_images.matches(group),
          "a sprite missing from an overlapping group fails")

    inner_b = rect(rect(canvas(200, 100, BLACK), 20, 20, 50, 50, WHITE),
                   40, 40, 5, 5, BLUE)
    inner_a = rect(rect(canvas(200, 100, BLACK), 20, 20, 50, 50, WHITE),
                   40, 40, 5, 5, MAGENTA)
    inner = result(inner_a, inner_b)
    check(inner["fallback_pixels"] == 25 and
          inner["solid_mismatches"] == 0 and
          not compare_images.matches(inner),
          "the fallback magenta inside a group fails")
    check(compare_images.matches(result(inner_a, inner_a)),
          "magenta the expected image has too is not the fallback's")


def main():
    failures = []

    def check(condition, what):
        if not condition:
            failures.append(what)

    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(scratch)
        base = image(10, 10, 100)
        a_path = root / "a.png"
        compare_images.write_png(a_path, *base)
        check(compare_images.read_png(a_path) == base, "round trip")

        def matches(other):
            return compare_images.matches(compare_images.compare(base, other))

        check(matches(base), "identical images match")
        shifted = (10, 10, bytes(
            [108, 108, 108, 100] * 100))
        result = compare_images.compare(base, shifted)
        check(result["within"] == 100, "a shift of 8 is within")
        check(not compare_images.matches(result),
              "a shift of 8 on three channels breaks the mean")
        check(not matches(image(5, 10, 100)), "sizes must match")

        # A drawn column of 10 white pixels (x = 9) on the grey background.
        column = bytearray(base[2])
        for y in range(10):
            column[(y * 10 + 9) * 4:(y * 10 + 10) * 4] = b"\xff" * 4
        column = (10, 10, bytes(column))

        def matches_column(other):
            return compare_images.matches(
                compare_images.compare(column, other))

        one = compare_images.compare(column, with_pixels(column, 1, 255))
        check(one["drawn"] == 11 and one["drawn_within"] == 10,
              "one far-off pixel: 10 of 11 drawn pixels within")
        check(matches_column(with_pixels(column, 1, 255)),
              "one far-off pixel matches")
        check(not matches_column(with_pixels(column, 3, 255)),
              "three far-off pixels (97% within) do not")

        # 100 x 100 grey with two 5 x 5 white squares (0.5% drawn): a
        # blank image passes the 98% and mean conditions but not the
        # drawn pixels.
        squares = bytearray([100] * (100 * 100 * 4))
        for left in (10, 60):
            for y in range(40, 45):
                for x in range(left, left + 5):
                    squares[(y * 100 + x) * 4:(y * 100 + x + 1) * 4] = (
                        b"\xff" * 4)
        squares = (100, 100, bytes(squares))
        blank = image(100, 100, 100)
        nothing = compare_images.compare(blank, squares)
        check(nothing["within"] / nothing["pixels"] >=
              compare_images.MIN_WITHIN_FRACTION and
              nothing["mean_abs_diff"] <= compare_images.MAX_MEAN_ABS_DIFF,
              "a blank image passes the 98% and mean conditions")
        check(nothing["drawn"] == 50 and nothing["drawn_within"] == 0,
              "none of the 50 drawn pixels is within")
        check(not compare_images.matches(nothing),
              "an image missing its sparse sprites does not match")

        sprite_cases(check)

        b_path = root / "b.png"
        compare_images.write_png(b_path, *with_pixels(base, 3, 255))
        script = HERE / "compare_images.py"
        status = subprocess.run([sys.executable, str(script), str(a_path),
                                 str(b_path)], check=False).returncode
        check(status == 1, "the command reports a mismatch")
        check((root / "b.diff.png").exists(), "the diff image is written")
        status = subprocess.run([sys.executable, str(script), str(a_path),
                                 str(a_path)], check=False).returncode
        check(status == 0, "the command reports a match")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("compare_images_test: passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
