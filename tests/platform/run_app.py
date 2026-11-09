"""Runs an engine executable and checks its exit status and output.

CTest's PASS_REGULAR_EXPRESSION ignores the exit status, so the desktop
tests of the rampage executable run through this driver:

  run_app.py --expect-exit 0 --expect REGEX [--expect REGEX]...
             [--reject REGEX]... [--timeout SEC] -- COMMAND [ARGS...]

Every --expect must match the combined stdout and stderr, no --reject may,
and the process must exit with --expect-exit within --timeout seconds. The
child runs with SE_NO_MESSAGE_BOX=1. Python 3, stdlib only.
"""

import argparse
import os
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--expect-exit", type=int, required=True)
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--reject", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("no command")

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=args.timeout,
                                env=env, check=False)
    except subprocess.TimeoutExpired as error:
        print(error.stdout or "")
        print(error.stderr or "")
        print(f"FAIL: no exit within {args.timeout} s")
        return 1
    output = (result.stdout or "") + (result.stderr or "")
    print(output)
    failures = []
    if result.returncode != args.expect_exit:
        failures.append(f"exit status {result.returncode}, expected "
                        f"{args.expect_exit}")
    for pattern in args.expect:
        if not re.search(pattern, output):
            failures.append(f"missing output: {pattern}")
    for pattern in args.reject:
        match = re.search(pattern, output)
        if match:
            failures.append(f"unexpected output: {match.group(0)}")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("run_app: passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
