"""Tests compare_images.py on synthetic PNGs (CTest tools.compare_images).

Writes small RGBA images with compare_images.write_png, reads them back,
and checks the tolerance rule at its edges: identical images, a uniform
shift inside the per-pixel tolerance that still breaks the mean, a few
far-off pixels below and above 2%, a size mismatch, and the diff image of
a mismatch. Python 3, stdlib only.
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
        check(matches(with_pixels(base, 1, 255)), "one far-off pixel matches")
        check(not matches(with_pixels(base, 3, 255)),
              "three far-off pixels (97% within) do not")
        check(not matches(image(5, 10, 100)), "sizes must match")

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
