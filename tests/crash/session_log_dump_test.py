"""A game's dump carries its SessionLog (docs/milestone-specs.md M13a,
"Replays and dumps of multi-state sessions"): crash.runtime_dump_with_
session_log.

Runs content_probe (the content test module as a game: its states load
their texts through the lane and the content gate) headless with
--push-event 60:focus-lost, which the game turns into a logged GameState
command (push "pause" over "play"), and --dump-test after-ticks:240: the
watchdog writes a dump after tick 240 and the probe quits. The dump, read
by tools/minidump/read_dump.py, holds the session stream (0x53450006: the
session's replay file without frames, here a content bundle and the
SessionLog with the focus loss's entry); se_replay --from-dump replays the
session from it, the pause included, to tick 240 and matches the dump's
published hash.

usage:
  session_log_dump_test.py --probe EXE --se-replay EXE --read-dump PY
      --content-root DIR --work DIR
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rampage_dump_test as base  # noqa: E402  (this directory)

TICK = 240
FOCUS_FRAME = 60
STREAM_SESSION = 0x53450006
RUN_TIMEOUT_S = 90


def report(failures):
    if failures.items:
        print("FAIL: runtime_dump_with_session_log")
        for item in failures.items:
            print(f"  {item}")
        return 1
    print("PASS: runtime_dump_with_session_log")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--se-replay", type=pathlib.Path, required=True)
    parser.add_argument("--read-dump", type=pathlib.Path, required=True)
    parser.add_argument("--content-root", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    pref = args.work / "pref"
    failures = base.Failures()

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    command = [str(args.probe), "--headless", "--audio", "none",
               "--fixed-frame-dt", "8.3333", "--log-level", "debug",
               "--content-root", str(args.content_root),
               "--pref-dir", str(pref),
               "--push-event", f"{FOCUS_FRAME}:focus-lost",
               "--dump-test", f"after-ticks:{TICK}"]
    print("$", " ".join(command))
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", env=env, timeout=RUN_TIMEOUT_S,
                            check=False)
    output = (result.stdout or "") + (result.stderr or "")
    print(f"content_probe: exit {result.returncode}\n{output}")
    failures.equal(result.returncode, 0, "content_probe exit code")
    failures.check("crash capture on" in output,
                   "the probe did not start crash capture")
    focus = re.search(r"focus lost on 'play': GameState command push "
                      r"'pause' logged before tick (\d+)", output)
    failures.check(focus is not None, "no focus-loss state command logged")
    failures.check("dump test: the watchdog finished the dump" in output,
                   "main did not see the watchdog finish the dump")

    dumps = base.find_dumps(pref / "crashdb")
    if not failures.equal(len(dumps), 1, "dumps written"):
        return report(failures)
    dump = base.decode(args.read_dump, dumps[0])
    session = base.stream(dump, STREAM_SESSION)
    if failures.check(session is not None, "no session stream in the dump"):
        failures.check(session.get("head", 0) > 100,
                       f"the session stream holds {session.get('head')} "
                       "bytes")
    header = base.stream(dump, 0x53450005)
    if failures.check(header is not None, "no session header stream"):
        record = header["records"][0]
        failures.equal((record.get("module"), record.get("flags", 0)),
                       ("content_test", 0), "session header module, flags")

    hashes = args.work / "replayed.txt"
    replay = subprocess.run(
        [str(args.se_replay), "--from-dump", str(dumps[0]), "--hash-out",
         str(hashes)], capture_output=True, text=True, errors="replace",
        timeout=RUN_TIMEOUT_S, check=False)
    text = (replay.stdout or "") + (replay.stderr or "")
    print(f"se_replay --from-dump: exit {replay.returncode}\n{text}")
    failures.equal(replay.returncode, 0, "se_replay --from-dump exit code")
    failures.check(re.search(rf"reached tick {TICK} ", text) is not None,
                   f"the replay did not reach tick {TICK}")
    failures.check("matches the dump" in text,
                   "the replayed hash does not match the dump's")
    states = pathlib.Path(str(hashes) + ".states")
    if focus and failures.check(states.exists(), f"no {states}"):
        tops = {}
        for line in states.read_text().splitlines():
            tick, top = line.split()
            tops[int(tick)] = top
        paused = int(focus.group(1))
        failures.equal((tops.get(paused - 1), tops.get(paused)),
                       ("play", "pause"),
                       f"the replay's top states at ticks {paused - 1} and "
                       f"{paused}")
    return report(failures)


if __name__ == "__main__":
    sys.exit(main())
