"""platform.shutdown_stress (docs/milestone-specs.md M13a; the design's
"Tests And Tooling": shutdown stress): 100 headless rampage runs
(--headless --audio none --fixed-frame-dt 8.3333) from seeded frames 1 to
600, each quitting one of three ways: by --frames, by an SDL_EVENT_QUIT
pushed with SDL_PushEvent, or by SDL_EVENT_TERMINATING (the --push-event
test hook). Every run must exit 0, log "teardown took" under 2 s and,
with --expect-no-threads, report that no engine thread was left once the
teardown had run ("engine threads left at exit: 0").

usage:
  shutdown_stress_test.py --rampage EXE --work DIR [--runs N] [--seed S]
"""

import argparse
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import time

TEARDOWN_LIMIT_MS = 2000.0
RUN_TIMEOUT_S = 120
KINDS = ("frames", "quit", "terminating")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--runs", type=int, default=100)
    parser.add_argument("--seed", type=int, default=13)
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    (work / "pref").mkdir(parents=True)
    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    draw = random.Random(args.seed)
    failures = []
    longest = 0.0
    started = time.monotonic()
    for run in range(args.runs):
        frame = draw.randint(1, 600)
        kind = KINDS[run % len(KINDS)]
        command = [args.rampage, "--headless", "--audio", "none",
                   "--fixed-frame-dt", "8.3333", "--expect-no-threads",
                   "--pref-dir", str(work / "pref")]
        if kind == "frames":
            command += ["--frames", str(frame)]
        else:
            # --frames far behind the pushed event, so a lost event is a
            # failure and not a hang.
            command += ["--frames", "5000", "--push-event",
                        f"{frame}:{kind}"]
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    errors="replace", env=env,
                                    timeout=RUN_TIMEOUT_S, check=False)
        except subprocess.TimeoutExpired:
            failures.append(f"run {run} ({kind} at frame {frame}) hung")
            continue
        output = (result.stdout or "") + (result.stderr or "")
        problems = []
        if result.returncode != 0:
            problems.append(f"exit {result.returncode}")
        teardown = re.search(r"teardown took ([0-9.]+) ms", output)
        if teardown is None:
            problems.append("no 'teardown took' line")
        else:
            longest = max(longest, float(teardown.group(1)))
            if float(teardown.group(1)) >= TEARDOWN_LIMIT_MS:
                problems.append(f"teardown {teardown.group(1)} ms")
        if "engine threads left at exit: 0" not in output:
            problems.append("engine threads were left at exit")
        if kind != "frames" and f"pushed event '{kind}' at frame {frame}" \
                not in output:
            problems.append(f"the {kind} event was not pushed")
        if problems:
            failures.append(f"run {run} ({kind} at frame {frame}): "
                            + ", ".join(problems))
            print(output[-4000:])
    print(f"shutdown_stress: {args.runs} runs in "
          f"{time.monotonic() - started:.1f} s, longest teardown "
          f"{longest:.1f} ms")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("shutdown_stress: passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
