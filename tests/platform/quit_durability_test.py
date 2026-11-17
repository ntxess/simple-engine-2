"""The runtime's quit durability (docs/milestone-specs.md M13a; the design's
"Tests And Tooling": quit durability), with rampage headless and its
--record replay as the Durable save (M13 decision 4: rampage persists no
game save yet).

Scenarios:
  replay_throttled      --fs-faults write-ms=3000: the replay's one write
                        step takes 3 s. The quit stays pending until the
                        rename ("quit waits for 1 Durable lane job", then
                        "the lane's Durable jobs are done"), it waited at
                        least the throttled step, and the file is complete:
                        the size the App logged, and se_replay --inputs
                        replays it with every recorded hash matching.
  deadline_after_drain  --fs-faults write-ms=7000: a 7 s drain, longer than
                        the watchdog's 5 s shutdown deadline, which starts
                        only once the drain is over ("shutdown deadline
                        started" after "the lane's Durable jobs are done"):
                        no deadline dump, exit 0, and the teardown under
                        2 s.
  failed_durable_save   --fs-faults busy-ms=2500: the replay's rename is
                        refused past the lane's 2 s retry limit, so the
                        Durable save fails. The game build logs it and
                        quits (M13 decision 5): exit 0, no "quit
                        cancelled", no replay file and no temporary file
                        left.

usage:
  quit_durability_test.py --rampage EXE --se-replay EXE --work DIR
      --scenario NAME
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

TIMEOUT_S = 90
TEARDOWN_LIMIT_MS = 2000.0
FRAMES = 240


def run(command, env):
    print("$", " ".join(command))
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", env=env, timeout=TIMEOUT_S,
                            check=False)
    print(f"exit {result.returncode} after "
          f"{time.monotonic() - started:.2f} s")
    output = (result.stdout or "") + (result.stderr or "")
    return result.returncode, output


def in_order(output, steps, failures):
    position = 0
    for name, pattern in steps:
        match = re.compile(pattern).search(output, position)
        if match is None:
            failures.append(f"missing, or out of order: {name} ({pattern})")
            continue
        position = match.end()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--se-replay", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--scenario", required=True,
                        choices=["replay_throttled", "deadline_after_drain",
                                 "failed_durable_save"])
    args = parser.parse_args()
    work = pathlib.Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    (work / "pref").mkdir(parents=True)
    replay = work / "run.sereplay"
    faults = {"replay_throttled": "write-ms=3000",
              "deadline_after_drain": "write-ms=7000",
              "failed_durable_save": "busy-ms=2500"}[args.scenario]

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    command = [args.rampage, "--headless", "--audio", "none",
               "--fixed-frame-dt", "8.3333", "--log-level", "debug",
               "--pref-dir", str(work / "pref"), "--frames", str(FRAMES),
               "--record", str(replay), "--fs-faults", faults]
    code, output = run(command, env)
    print(output)

    failures = []
    if code != 0:
        failures.append(f"exit status {code}, expected 0")
    waited = re.search(r"quit ready after ([0-9.]+) ms", output)
    teardown = re.search(r"teardown took ([0-9.]+) ms", output)
    if teardown is None:
        failures.append("no 'teardown took' line")
    elif float(teardown.group(1)) >= TEARDOWN_LIMIT_MS:
        failures.append(f"the teardown took {teardown.group(1)} ms")
    if "shutdown has not finished" in output:
        failures.append("the watchdog's shutdown deadline fired")

    if args.scenario in ("replay_throttled", "deadline_after_drain"):
        step_ms = 3000 if args.scenario == "replay_throttled" else 7000
        in_order(output, [
            ("pending", r"quit pending \("),
            ("submitted", r"replay of \d+ ticks \(\d+ bytes\) to .* through "
                          r"the background lane"),
            ("waiting", r"quit waits for 1 Durable lane job"),
            ("done", r"the lane's Durable jobs are done"),
            ("deadline", r"shutdown deadline started"),
            ("ready", r"quit ready after [0-9.]+ ms"),
            ("written", r"replay of \d+ ticks written to .* by the "
                        r"background lane"),
        ], failures)
        if waited and float(waited.group(1)) < step_ms * 0.9:
            failures.append(f"the quit waited {waited.group(1)} ms, less "
                            f"than the {step_ms} ms write step")
        size = re.search(r"replay of \d+ ticks \((\d+) bytes\)", output)
        if not replay.exists():
            failures.append(f"{replay} was not written")
        elif size and replay.stat().st_size != int(size.group(1)):
            failures.append(f"{replay}: {replay.stat().st_size} bytes, the "
                            f"recorder wrote {size.group(1)}")
        else:
            # Every recorded hash replays: the file is the recorder's.
            code, replayed = run([args.se_replay, "--module", "rampage",
                                  "--inputs", str(replay)], env)
            print(replayed)
            if code != 0:
                failures.append(f"se_replay --inputs: exit {code}")
    else:
        in_order(output, [
            ("pending", r"quit pending \("),
            ("waiting", r"quit waits for 1 Durable lane job"),
            ("failed", r"Durable job\(s\) failed during the quit; the game's "
                       r"quit goes on"),
            ("ready", r"quit ready after [0-9.]+ ms"),
            ("lost", r"--record .*: the background lane could not write it"),
        ], failures)
        if "quit cancelled" in output:
            failures.append("the game's quit was cancelled")
        if replay.exists():
            failures.append(f"{replay} exists after a failed save")
    leftovers = [p.name for p in work.iterdir()
                 if p.name.startswith(replay.name) and p != replay]
    if leftovers:
        failures.append(f"temporary files left: {leftovers}")
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print(f"quit_durability.{args.scenario}: passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
