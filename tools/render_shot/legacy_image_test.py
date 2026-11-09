"""The legacy image comparison (docs/milestone-specs.md M7).

  legacy_image_test.py --legacy-replay EXE --render-shot EXE --driver NAME
                       --scenario NAME --tick N --work DIR

Runs legacy_replay with the reference scenario NAME and its recorded input
(tests/fixtures/reference/NAME.scenario.json and .seinput) up to tick N
with --image-out, then render_shot on the same scenario, input and tick
with --compare against the legacy image: both images have the legacy
viewport's size and must match within the golden tolerance
(tests/render_gpu/support/image_compare.h). A mismatch leaves a diff PNG
in DIR. The legacy image must also equal the committed copy
tests/golden/render/legacy/<NAME>_<N>.png, on which
render_gpu."The sandbox golden matches the legacy image, sprite by sprite"
checks that every sprite missing or drawn magenta fails (ADR-069); with
SE_UPDATE_GOLDENS=1 the copy is rewritten instead. Run from the repository
root (the legacy tree reads config/ and assets/ relative to the working
directory). Python 3, stdlib only.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import compare_images  # noqa: E402  pylint: disable=wrong-import-position

GOLDEN_DIR = pathlib.Path("tests/golden/render/legacy")


def check_committed_copy(legacy_png, name):
    """The legacy image equals tests/golden/render/legacy/<name>.png, or,
    with SE_UPDATE_GOLDENS=1, replaces it. Returns an error or None."""
    golden = GOLDEN_DIR / f"{name}.png"
    if os.environ.get("SE_UPDATE_GOLDENS") == "1":
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(legacy_png, golden)
        print(f"legacy_image_test: rewrote {golden}")
        return None
    if not golden.exists():
        return (f"no committed legacy image {golden}; run with "
                f"SE_UPDATE_GOLDENS=1 and commit it")
    if golden.read_bytes() == legacy_png.read_bytes():
        return None
    # Another PNG encoder: the pixels must still be the same.
    if compare_images.read_png(golden) == compare_images.read_png(legacy_png):
        return None
    return (f"{legacy_png} differs from the committed {golden}: rerun with "
            f"SE_UPDATE_GOLDENS=1 if the legacy reference changed on "
            f"purpose, and commit it")


def run(command, **kwargs):
    print("$", " ".join(str(part) for part in command))
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", timeout=110, check=False,
                            **kwargs)
    print(result.stdout + result.stderr)
    return result.returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--legacy-replay", required=True)
    parser.add_argument("--render-shot", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--tick", type=int, required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    fixtures = pathlib.Path("tests/fixtures/reference")
    scenario = fixtures / f"{args.scenario}.scenario.json"
    inputs = fixtures / f"{args.scenario}.seinput"
    legacy_png = work / f"{args.scenario}_{args.tick}_legacy.png"
    engine_png = work / f"{args.scenario}_{args.tick}_engine.png"
    diff_png = work / f"{args.scenario}_{args.tick}_diff.png"
    for stale in (legacy_png, engine_png, diff_png):
        if stale.exists():
            stale.unlink()

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    if run([args.legacy_replay, "--scenario", scenario, "--inputs", inputs,
            "--image-out", f"{args.tick}:{legacy_png}"], env=env) != 0:
        print("FAIL: legacy_replay --image-out failed")
        return 1
    if not legacy_png.exists():
        print(f"FAIL: legacy_replay wrote no {legacy_png}")
        return 1
    stale = check_committed_copy(legacy_png, f"{args.scenario}_{args.tick}")
    if stale:
        print(f"FAIL: {stale}")
        return 1
    status = run([args.render_shot, "--legacy-scenario", scenario,
                  "--inputs", inputs, "--tick", str(args.tick),
                  "--gpu-driver", args.driver, "--out", engine_png,
                  "--compare", legacy_png, "--diff", diff_png], env=env)
    if status == 77:
        print(f"SKIP: the GPU driver {args.driver} is not available")
        return 77
    if status != 0:
        print(f"FAIL: render_shot ({args.driver}) does not match the legacy "
              f"image at tick {args.tick}")
        return 1
    print(f"legacy_image_test: {args.scenario} at tick {args.tick} matches "
          f"on {args.driver}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
