"""Fault injection at one step of the frame transaction, in rampage
(docs/milestone-specs.md M7, exit: "Fault injection at every step for 100
frames").

  fault_test.py --rampage EXE --render-shot EXE --driver NAME --work DIR
                --step none|build|vertices|upload|prepare|copy_pass|
                       viewport_pass|acquire|swapchain_pass|submit|
                       device_lost

Every run uses a synthetic 120 Hz frame clock (--fixed-frame-dt), one tick
per frame once the sim runs, reloads the player texture at frame 10 (so
the upload step has work during the fault), shows its window without
focus and quits after 260 presented frames. The readback is of the first
frame after tick 130 (--readback-tick): since M9 the sim holds until the
start state's textures are resident, so a fault on the uploads (or their
submit) for 100 frames delays the ticks, and a frame number would name a
different tick in each run. --step none is the reference
run: it writes the tick-130 readback that the other steps compare with.
With --inject-frame-fault STEP:1:100 (frames 1 to 100):
  - build and vertices throw in the build phase, a fault of the game build:
    the app quits through the callbacks' fault path (exit 1) after
    "render build fault", and shuts down in order;
  - every other step makes its SDL call look failed: the app keeps running
    (exit 0), and its tick-130 readback, at least 30 frames after the
    fault stopped, matches the reference within the golden tolerance
    (render_shot --compare).
With --inject-device-lost 100 (step device_lost; the M6/M7 integration,
ADR-068) the device status reports removed once at the start of frame
100: the app enters DeviceLost, releases every GPU object, recreates the
device on main, re-uploads its textures and runs on (exit 0); its tick-130
readback matches the reference the same way.
Every run must release every GPU object ("0 alive"), never trip an
SE_ASSERT (the frame guard asserts it never cancels after an acquire) and,
under ASan, report nothing. A driver the machine lacks skips (exit 77).
Python 3, stdlib only.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

FRAMES = 260
READBACK_TICK = 130
FAULT_UNTIL = 100
BUILD_STEPS = {"build", "vertices"}
DEVICE_LOST_FRAME = 100
SKIP = 77


def run(command, timeout=100):
    print("$", " ".join(str(part) for part in command))
    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env.setdefault("SE_AUDIO", "null")
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", timeout=timeout, env=env,
                            check=False)
    output = result.stdout + result.stderr
    print(output)
    return result.returncode, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--render-shot", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--step", required=True)
    args = parser.parse_args()

    status, _ = run([args.render_shot, "--probe-driver", args.driver])
    if status == SKIP:
        print(f"SKIP: the GPU driver {args.driver} is not available")
        return SKIP

    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    reference = work / "reference.png"
    readback = work / f"{args.step}.png"
    if readback.exists():
        readback.unlink()
    command = [args.rampage, "--gpu-driver", args.driver, "--no-activate",
               "--pref-dir", str(work / "pref"), "--fixed-frame-dt", "8.3333",
               "--frames", str(FRAMES), "--reload-texture", "player@10",
               "--readback-tick", f"{READBACK_TICK}:{readback}"]
    if args.step == "device_lost":
        command += ["--inject-device-lost", str(DEVICE_LOST_FRAME)]
    elif args.step != "none":
        command += ["--inject-frame-fault", f"{args.step}:1:{FAULT_UNTIL}"]
    # The sandbox's image (M13b: rampage starts at the Title).
    command += ["--", "--start-state", "sandbox"]
    status, output = run(command)

    failures = []

    def expect(pattern, why):
        if not re.search(pattern, output):
            failures.append(f"{why} (missing: {pattern})")

    def reject(pattern, why):
        match = re.search(pattern, output)
        if match:
            failures.append(f"{why}: {match.group(0)}")

    reject(r"SE_ASSERT failed[^\n]*", "an assertion failed")
    reject(r"AddressSanitizer[^\n]*", "ASan reported an error")
    reject(r"GPU objects never released[^\n]*", "GPU objects leaked")
    expect(r"GPU objects: \d+ created, \d+ released, 0 alive",
           "the renderer did not release every GPU object")
    if args.step in BUILD_STEPS:
        if status != 1:
            failures.append(f"exit status {status}, expected 1 (the fault "
                            f"path)")
        expect(r"render build fault", "no build fault was reported")
        expect(r"shutdown \(failure\)", "no orderly shutdown after the fault")
    else:
        if status != 0:
            failures.append(f"exit status {status}, expected 0")
        expect(rf"shutdown \(success\): \d+ iterates, {FRAMES} presented",
               "the app did not keep running to the end")
        expect(rf"readback of frame \d+ \(tick {READBACK_TICK},",
               "no readback")
        if args.step == "device_lost":
            expect(rf"removed \(iterate {DEVICE_LOST_FRAME}\): DeviceLost",
                   "the injected loss did not enter DeviceLost")
            expect(r"device recovered: '[a-z0-9]+' recreated",
                   "the device was not recreated")
            expect(r"device losses recovered: 1",
                   "no recovery in the shutdown summary")
        if args.step != "none":
            match = re.search(r"renderer: (\d+) frames", output)
            if not match:
                failures.append("no renderer totals")
            if not readback.exists():
                failures.append(f"no {readback}")
            elif not reference.exists():
                failures.append(f"no reference image {reference} (the "
                                f".reference test runs first)")
            else:
                compared, _ = run([args.render_shot, "--compare",
                                   str(readback), "--with", str(reference),
                                   "--diff",
                                   str(work / f"{args.step}.diff.png")])
                if compared != 0:
                    failures.append("the frame after the fault does not "
                                    "match the reference")
        elif readback.exists():
            readback.replace(reference)
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print(f"fault_test: {args.step} on {args.driver} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
