"""rampage quits with a Durable save in flight on a throttled disk
(docs/milestone-specs.md "Integration (M8 + M9 + M17a)": QuitPending waits
for Durable saves through the lane's QuitProvider).

Runs the rampage executable (a window that is never activated, the null
audio backend) with the test hook --durable-save-test FRAME:KB:MS: at
iterate FRAME the App sets the se::fs injector to MS ms per write step,
submits a Durable save of KB KiB of JSON to <pref>/durable_save_test.json
through the background lane and requests the quit. The run must show, in
this order: the save submitted, the quit pending, the lane's quit provider
waiting for its Durable job, the save renamed into place, the quit ready;
it must exit 0, the wait must have lasted at least the throttled write
steps, and the file must be complete and parse.

usage:
  durable_quit_test.py --rampage EXE --work DIR
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

FRAME = 30
KIB = 4096
STEP_MS = 100
WRITE_STEP_BYTES = 1 << 20
TIMEOUT_S = 60


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    (work / "pref").mkdir(parents=True)
    target = work / "pref" / "durable_save_test.json"

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env.setdefault("SE_AUDIO", "null")
    command = [args.rampage, "--no-activate", "--log-level", "debug",
               "--pref-dir", str(work / "pref"), "--frames", "20000",
               "--durable-save-test", f"{FRAME}:{KIB}:{STEP_MS}"]
    print("$", " ".join(command))
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", env=env, timeout=TIMEOUT_S,
                            check=False)
    print(f"exit {result.returncode} after "
          f"{time.monotonic() - started:.2f} s")
    output = (result.stdout or "") + (result.stderr or "")
    print(output)

    failures = []
    if result.returncode != 0:
        failures.append(f"exit status {result.returncode}, expected 0")
    # The lines that must appear, in order. The lane's completion ("saved
    # ...") is drained at an iterate start, so it may come before or after
    # the provider's last answer; the file's size at the start of the
    # shutdown, before shutdown step 8 joins the lane, is what proves the
    # quit waited for the rename.
    steps = [
        ("submitted", r"durable save test: \d+ bytes to .*durable_save_test"
                      r"\.json at frame \d+, \d+ ms per write step"),
        ("pending", r"quit pending \("),
        ("waiting", r"quit waits for 1 Durable lane job"),
        ("done", r"the lane's Durable jobs are done"),
        ("ready", r"quit ready after [0-9.]+ ms"),
        ("in place", r"durable save test: \d+ bytes in place at shutdown"),
    ]
    position = 0
    for name, pattern in steps:
        match = re.compile(pattern).search(output, position)
        if match is None:
            failures.append(f"missing, or out of order: {name} ({pattern})")
            continue
        position = match.end()
    waited = re.search(r"quit ready after ([0-9.]+) ms", output)
    expected_bytes = None
    size = re.search(r"durable save test: (\d+) bytes", output)
    if size:
        expected_bytes = int(size.group(1))
    in_place = re.search(r"durable save test: (\d+) bytes in place", output)
    if (in_place and expected_bytes is not None
            and int(in_place.group(1)) != expected_bytes):
        failures.append(f"at shutdown the file had {in_place.group(1)} "
                        f"bytes, expected {expected_bytes}")
    if waited and expected_bytes:
        steps_needed = -(-expected_bytes // WRITE_STEP_BYTES)
        minimum = steps_needed * STEP_MS * 0.9
        if float(waited.group(1)) < minimum:
            failures.append(f"the quit waited {waited.group(1)} ms, less than "
                            f"{steps_needed} write steps of {STEP_MS} ms")
    if not target.exists():
        failures.append(f"{target} was not written")
    else:
        data = target.read_bytes()
        if expected_bytes is not None and len(data) != expected_bytes:
            failures.append(f"{target}: {len(data)} bytes, expected "
                            f"{expected_bytes}")
        try:
            document = json.loads(data)
            if document.get("format") != "se-durable-save-test-v1":
                failures.append(f"{target}: unexpected format")
        except ValueError as error:
            failures.append(f"{target} does not parse: {error}")
    leftovers = [p.name for p in target.parent.iterdir()
                 if p.name.startswith(target.name) and p != target]
    if leftovers:
        failures.append(f"temporary files left: {leftovers}")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("durable_quit: passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
