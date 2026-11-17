"""The M12 hang dumps through the real stack (docs/milestone-specs.md M12,
"Tests" and "Exit (M12)"), for rampage and for the editor's Play.

Runs the rampage executable (unfocused, audio on the null backend) with the
--hang-test hook, which puts main in a loop inside the HangTest system at
tick 240, and checks what the watchdog did:

  hang          --hang-test after-ticks:240: main loops forever. The
                watchdog declares the hang after 5 periods of 1 s and its
                out-of-process dump must be written within 6 s of the hang's
                start (both times from the process's own log, one clock).
                The dump, decoded by tools/minidump/read_dump.py --json,
                shows main's breadcrumb at tick 240 in HangTest (phase
                Cleanup), the reason "hang 1: main made no progress for 5.x
                s at tick 240", the session header (module, scene, seed,
                tick rate, start tick, a published hash) and the InputFrame
                ring up to tick 240; se_replay --from-dump replays the
                session to tick 240 and matches the dump's hash. Then the
                test ends the process (main never returns).
  driver_stall  --hang-test after-ticks:240:for-ms:6500:in-driver
                --frames 600: main loops for 6.5 s inside a DriverWaitScope,
                as a stalled SDL_GPU wait would. The hang is labelled a GPU
                or compositor stall in the log and the dump's reason, one
                dump is written, the episode's end is logged with its length
                (Hung -> Running), and rampage then quits on its own through
                the two-phase quit with its teardown under 2 s.
  editor_play_hang
                simple_editor (--editor EXE) with --hang-test
                after-ticks:240 and an editor script (--script FILE) that
                starts Play and edits the play World twice before tick 240:
                the hang is inside the Play session's tick 240. The same
                checks as "hang", with the editor's session header (flag 1:
                an editor Play; the scene is the document's name), the
                Play-start stream (0x53450006: the session's replay file
                without frames, ADR-111) in the dump, and se_replay
                --from-dump replaying the session from those bytes, the
                SessionLog's edits included, to tick 240 (docs/milestone-
                specs.md "Integration (M10a + M10b + M12 + M14)").

usage:
  rampage_hang_test.py --rampage EXE --se-replay EXE --read-dump PY
      --work DIR --scenario hang|driver_stall
  rampage_hang_test.py --editor EXE --script FILE --se-replay EXE
      --read-dump PY --work DIR --scenario editor_play_hang
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rampage_dump_test as base  # noqa: E402  (this directory)

TICK = 240
PHASE_CLEANUP = 6
HANG_LIMIT_S = 6.0
TEARDOWN_LIMIT_MS = 2000.0
RUN_TIMEOUT_S = 60
LINE = re.compile(r"^\s*(\d+\.\d+) ([TDIWEF]) (\S+) t=(\d+) (\S+) (.*)$")

SCENARIOS = {
    "hang": {
        "args": ["--hang-test", f"after-ticks:{TICK}"],
        "reason": "hang 1: main made no progress for 5.",
        "ends": False,
    },
    "driver_stall": {
        "args": ["--hang-test", f"after-ticks:{TICK}:for-ms:6500:in-driver",
                 "--frames", "600"],
        "reason": "hang 1: GPU or compositor stall, main in an SDL_GPU or "
                  "pacer wait for 5.",
        "ends": True,
    },
    "editor_play_hang": {
        "args": ["--hang-test", f"after-ticks:{TICK}"],
        "reason": "hang 1: main made no progress for 5.",
        "ends": False,
        "editor": True,
    },
}
STREAM_PLAY_START = 0x53450006
FLAG_PLAY_START = 1


class Run:
    """rampage with its merged output read line by line as it comes."""

    def __init__(self, rampage, args):
        env = dict(os.environ)
        env["SE_NO_MESSAGE_BOX"] = "1"
        env.setdefault("SE_AUDIO", "null")
        self.lines = []
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.process = subprocess.Popen(
            [str(rampage), *args], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace", env=env)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            with self.lock:
                self.lines.append((time.monotonic(), line.rstrip("\n")))

    def find(self, text):
        with self.lock:
            for arrived, line in self.lines:
                if text in line:
                    return arrived, line
        return None

    def wait_for(self, text, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.find(text)
            if found:
                return found
            if self.process.poll() is not None and not self.reader.is_alive():
                return self.find(text)
            time.sleep(0.05)
        return None

    def finish(self, kill):
        if kill and self.process.poll() is None:
            self.process.kill()
        try:
            self.process.wait(timeout=RUN_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=10)
        with self.lock:
            output = "\n".join(line for _, line in self.lines)
        print(f"rampage: exit {self.process.returncode & 0xFFFFFFFF:#x} after "
              f"{time.monotonic() - self.started:.2f} s")
        print("--- rampage output\n" + output)
        return output


def log_time(line):
    """The log record's own timestamp (seconds, the process's clock)."""
    match = LINE.match(line)
    return float(match.group(1)) if match else None


def describe_delay(run, start, written):
    """Where a late dump's time went: the declaration (late when the
    watchdog itself was not scheduled: ADR-104 does not count time it did
    not sample) or the handler's write."""
    declared = run.find("requesting a dump")
    at = log_time(declared[1]) if declared else None
    gaps = run.find("the watchdog took no sample for")
    parts = []
    if at is not None:
        parts.append(f"declared {at - start:.3f} s after the hang started, "
                     f"written {written - at:.3f} s after that")
    parts.append(f"unwatched gap: {gaps[1].strip()!r}" if gaps
                 else "no unwatched gap logged")
    return "; ".join(parts)


def check_dump(failures, dump, expected, system):
    failures.equal(dump["problems"], [], "read_dump problems")
    ids, tick = base.check_breadcrumbs(failures, dump, TICK)
    table = base.stream(dump, base.STREAM_BREADCRUMBS)
    if table is not None:
        mains = [r for r in table["records"] if r["slot"] == 0]
        if failures.check(mains, "no breadcrumb for slot 0"):
            crumb = mains[0]["breadcrumb"]
            failures.equal((crumb["phase"], crumb["system"]),
                           (PHASE_CLEANUP, system),
                           "main's breadcrumb (phase, system): HangTest")
    exception = dump["exception"]
    if failures.check(exception is not None, "no exception record"):
        failures.equal(exception["code"], base.SIMULATED, "exception code")
        failures.equal(exception["thread_id"], ids.get(base.ROLE_WATCHDOG),
                       "the dump was requested on the watchdog thread")
    reason = dump["annotation_objects"].get("se.dump_reason") or ""
    failures.check(reason.startswith(expected["reason"]) and
                   reason.endswith(f"at tick {TICK}"),
                   f"se.dump_reason {reason!r}")
    editor = expected.get("editor", False)
    header = base.stream(dump, 0x53450005)
    if failures.check(header is not None, "no session header stream"):
        record = header["records"][0]
        # A game's scene is its file; an editor Play's is the document's
        # name, and its start is the Play-start stream (flag 1).
        scene = "sandbox" if editor else \
            "assets/scenes/sandbox.scene.json"
        failures.equal((record.get("begun"), record.get("torn"),
                        record.get("module"), record.get("scene"),
                        record.get("seed"), record.get("tick_rate"),
                        record.get("start_tick"), record.get("first_frame"),
                        record.get("flags")),
                       (True, False, "rampage", scene, 1, 120, 1, 0,
                        FLAG_PLAY_START if editor else 0),
                       "session header")
        last = record.get("last_tick", 0)
        failures.check(TICK - 3 <= last < TICK,
                       f"the last published tick {last} is just before "
                       f"{TICK}")
    ring = base.stream(dump, base.STREAM_INPUT)
    if failures.check(ring is not None, "no InputFrame ring"):
        failures.equal(ring["head"], TICK, "InputFrame ring head (frames 1 "
                       f"to {TICK}, the hanging tick's included)")
    # The editor's Play-start stream, and since M13a a game's session
    # stream: the session's replay file without frames (the scene in it).
    start = base.stream(dump, STREAM_PLAY_START)
    if failures.check(start is not None, "no session (Play-start) stream"):
        failures.check(start.get("head", 0) > 1000,
                       f"the session stream holds {start.get('head')} "
                       "bytes (a replay file with the scene in it)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", type=pathlib.Path)
    parser.add_argument("--editor", type=pathlib.Path)
    parser.add_argument("--script", type=pathlib.Path)
    parser.add_argument("--se-replay", type=pathlib.Path, required=True)
    parser.add_argument("--read-dump", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS),
                        required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    pref = args.work / "pref"
    expected = SCENARIOS[args.scenario]
    failures = base.Failures()

    editor = expected.get("editor", False)
    if editor:
        if args.editor is None or args.script is None:
            parser.error("editor_play_hang needs --editor and --script")
        # The editor's own options come after "--"; audio stays on the null
        # backend (Run sets SE_AUDIO too).
        run = Run(args.editor, [*expected["args"], "--no-activate",
                                "--audio", "null", "--pref-dir", str(pref),
                                "--", "--editor-script",
                                str(args.script.resolve())])
    else:
        if args.rampage is None:
            parser.error(f"{args.scenario} needs --rampage")
        run = Run(args.rampage, [*expected["args"], "--no-activate",
                                 "--pref-dir", str(pref)])
    hang = run.wait_for("hang test: main loops", 30)
    dumped = run.wait_for("dump written (hang 1", 30) if hang else None
    if expected["ends"]:
        run.wait_for("shutdown order:", 40)
    output = run.finish(kill=not expected["ends"])
    if not failures.check(hang is not None, "the hang test never started"):
        return report(args.scenario, failures)
    system_match = re.search(r"'HangTest' \(id (\d+), phase Cleanup\) at "
                             rf"tick {TICK}", hang[1])
    failures.check(system_match is not None,
                   f"the hang line names HangTest and tick {TICK}")
    system = int(system_match.group(1)) if system_match else -1
    failures.check("crash capture on" in output,
                   "the executable did not start crash capture")
    if editor:
        failures.check("editor script: FAILED" not in output,
                       "the editor script reported a failure before the hang")
        failures.check("play: started at tick 1" in output,
                       "the editor did not start Play")
    if failures.check(dumped is not None, "no hang dump was written"):
        start, written = log_time(hang[1]), log_time(dumped[1])
        if failures.check(start is not None and written is not None,
                          "log lines without timestamps"):
            took = written - start
            print(f"hang dump written {took:.3f} s after the hang started "
                  f"(arrived {dumped[0] - hang[0]:.3f} s apart)")
            failures.check(5.0 <= took <= HANG_LIMIT_S,
                           f"the hang dump came {took:.3f} s after the hang "
                           f"started (5 to {HANG_LIMIT_S} s): "
                           f"{describe_delay(run, start, written)}")
    declared = [line for line in output.splitlines()
                if " E watchdog " in line and "requesting a dump" in line]
    failures.equal(len(declared), 1, "hangs declared")
    if declared:
        failures.check(expected["reason"] in declared[0],
                       f"the hang's log line: {declared[0]!r}")
    if expected["ends"]:
        failures.equal(run.process.returncode, 0, "rampage exit code")
        ended = re.search(r"hang 1 ended: main made progress again after "
                          r"([0-9.]+) s \(Hung -> Running\)", output)
        if failures.check(ended is not None, "the episode's end was not "
                          "logged"):
            length = float(ended.group(1))
            failures.check(6.0 <= length <= 7.5,
                           f"episode length {length} s for a 6.5 s stall")
        failures.check("shutdown (success)" in output,
                       "no clean two-phase shutdown after the hang")
        teardown = re.search(r"teardown took ([0-9.]+) ms", output)
        if failures.check(teardown is not None, "no teardown line"):
            failures.check(float(teardown.group(1)) < TEARDOWN_LIMIT_MS,
                           f"teardown took {teardown.group(1)} ms")
    dumps = base.find_dumps(pref / "crashdb")
    if failures.equal(len(dumps), 1, "dumps written"):
        dump = base.decode(args.read_dump, dumps[0])
        check_dump(failures, dump, expected, system)
        if not expected["ends"]:
            command = [str(args.se_replay), "--from-dump", str(dumps[0]),
                       "--hash-out", str(args.work / "hashes.txt")]
            if editor:
                command += ["--start-scene",
                            str(args.work / "play-start.scene.json")]
            replay = subprocess.run(
                command, capture_output=True, text=True,
                timeout=RUN_TIMEOUT_S, check=False)
            print("--- se_replay --from-dump\n" + replay.stdout +
                  replay.stderr)
            failures.equal(replay.returncode, 0, "se_replay exit code")
            failures.check(
                f"reached tick {TICK} (main's breadcrumb), inside which "
                "main stopped" in replay.stdout,
                f"se_replay did not reach tick {TICK}")
            failures.check(
                re.search(r"tick \d+'s hash [0-9a-f]{16} matches the dump",
                          replay.stdout) is not None,
                "se_replay did not match the dump's hash")
            hashes = (args.work / "hashes.txt").read_text().splitlines() \
                if (args.work / "hashes.txt").exists() else []
            failures.equal(len(hashes), TICK - 1,
                           "per-tick hashes of the ticks before the hang")
            if editor:
                failures.check("an editor Play session" in replay.stdout,
                               "se_replay did not start from the dump's "
                               "Play-start stream")
                failures.check(
                    (args.work / "play-start.scene.json").exists(),
                    "se_replay did not write the Play-start bytes")
    return report(args.scenario, failures)


def report(scenario, failures):
    if failures.items:
        print(f"FAIL: {scenario}")
        for item in failures.items:
            print(f"  {item}")
        return 1
    print(f"PASS: {scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
