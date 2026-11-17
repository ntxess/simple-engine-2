"""Pushed key events through the rampage executable, then its replay.

The M8 input tests that run the real App (docs/milestone-specs.md M8):

1. rampage runs at 60 Hz Locked (--fixed-frame-dt 16.6667) with --record,
   and the --push-key hook pushes SDL key events at the end of one iterate
   with explicit timestamps 2 ms and 10 ms after that frame's start, so the
   next frame places them: 8 ms apart, they must land on its two ticks.
   Their releases come the same way ten frames later.
2. se_replay runs the recorded session headless (--inputs): the recorded
   per-tick hashes must match tick by tick (se_replay fails otherwise), and
   --edges-out lists the recorded edges, whose sources must be the key
   source this platform stamps with (raw-keyboard on Windows).

usage:
  input_replay_test.py --rampage EXE --se-replay EXE --scene FILE
                       --out DIR --source NAME [--headless] [--timeout S]

Python 3, stdlib only. The children run with SE_NO_MESSAGE_BOX=1 and
SE_AUDIO=null (nothing is ever heard).
"""

import argparse
import os
import pathlib
import subprocess
import sys

# rampage's actions (games/rampage/include/game_rampage/module.h).
ACTION_DOWN = 2
ACTION_RIGHT = 3
PUSH_FRAME = 20
RELEASE_FRAME = 30


def run(command, timeout):
    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env["SE_AUDIO"] = "null"
    print("running:", " ".join(str(c) for c in command))
    result = subprocess.run([str(c) for c in command], capture_output=True,
                            text=True, errors="replace", timeout=timeout,
                            env=env, check=False)
    output = (result.stdout or "") + (result.stderr or "")
    print(output)
    return result.returncode, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--se-replay", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    replay = out / "session.sereplay"
    edges_file = out / "edges.txt"
    for stale in (replay, edges_file):
        if stale.exists():
            stale.unlink()

    # Headless iterates do not wait for frames, so the 60 of them pass in
    # less time than the lane takes to decode rampage's sounds, and the sim
    # holds until the start state's scope is resident (ADR-081, ADR-087):
    # without audio no sound is loaded and the ticks start at once.
    audio = "none" if args.headless else "null"
    command = [args.rampage, "--frames", "60", "--fixed-frame-dt", "16.6667",
               "--return-on-quit", "--no-crash-capture", "--pref-dir",
               out / "pref", "--record", replay, "--audio", audio]
    command += ["--headless"] if args.headless else ["--no-activate"]
    command += ["--push-key", f"{PUSH_FRAME}:2:d",
                "--push-key", f"{PUSH_FRAME}:10:s",
                "--push-key", f"{RELEASE_FRAME}:2:d:up",
                "--push-key", f"{RELEASE_FRAME}:10:s:up"]
    # The sandbox, which se_replay runs (M13b: rampage starts at the Title).
    command += ["--", "--start-state", "sandbox"]
    status, output = run(command, args.timeout)
    failures = []
    if status != 0:
        failures.append(f"rampage exited with {status}")
    if "replay of " not in output or not replay.exists():
        failures.append("rampage wrote no replay")
    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1

    status, output = run([args.se_replay, "--module", "rampage", "--scene",
                          args.scene, "--inputs", replay, "--edges-out",
                          edges_file], args.timeout)
    if status != 0:
        print(f"FAIL: se_replay exited with {status} (a hash mismatch prints "
              f"the tick)")
        return 1

    edges = []
    for line in edges_file.read_text().splitlines():
        tick, action, direction, stamp, order, source = line.split()
        edges.append((int(tick), int(action), direction == "down", int(stamp),
                      int(order), source))
    print(f"{len(edges)} recorded edges:")
    for edge in edges:
        print("  tick {} action {} {} stamp {} order {} source {}".format(
            edge[0], edge[1], "down" if edge[2] else "up", edge[3], edge[4],
            edge[5]))

    def find(action, down):
        found = [e for e in edges if e[1] == action and e[2] == down]
        if len(found) != 1:
            failures.append(f"expected one {'press' if down else 'release'} "
                            f"of action {action}, found {len(found)}")
            return None
        return found[0]

    for down in (True, False):
        right = find(ACTION_RIGHT, down)
        lower = find(ACTION_DOWN, down)
        if right is None or lower is None:
            continue
        what = "presses" if down else "releases"
        # 8 ms apart: on consecutive ticks, the two ticks of one frame.
        if lower[0] != right[0] + 1:
            failures.append(f"{what} 8 ms apart on ticks {right[0]} and "
                            f"{lower[0]}, expected consecutive ticks")
        if lower[3] - right[3] != 8_000_000:
            failures.append(f"{what} stamped {lower[3] - right[3]} ns apart, "
                            f"expected 8000000")
        for edge in (right, lower):
            if edge[5] != args.source:
                failures.append(f"an edge's source is {edge[5]}, expected "
                                f"{args.source}")
    if edges and any(e[5] != args.source for e in edges):
        failures.append("an edge from another source was recorded")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("ok: pushed keys 8 ms apart landed on consecutive ticks, the replay "
          "reproduced every hash and kept the edge sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
