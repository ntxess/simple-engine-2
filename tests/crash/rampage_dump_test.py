"""The M4 exit dumps through the real stack (docs/milestone-specs.md, M4
"Integration (M4a + M4b + M5)" and "Exit (M4)").

Runs the rampage executable with a test hook and checks the dump crashpad's
handler wrote, decoded with tools/minidump/read_dump.py --json:

  crash   rampage --crash-test after-ticks:240: an access violation on main
          right after tick 240; the process ends with the exception code
  dump    rampage --dump-test after-ticks:240: after tick 240 main asks the
          watchdog for a dump, holds the ticks until the watchdog has
          written it, then quits through the two-phase quit (exit 0)
  fault   rampage --inject-throw iterate:200: the SDL_AppIterate fault
          boundary catches a standard-library throw, requests a dump on
          main and runs the quit path (exit 1)

Each dump must show, from the real rings:
  - main's breadcrumb (ThreadRegistry slot 0, role main, live) at tick 240
    (fault: at the last tick that ran), and the log worker's and the
    watchdog's slots;
  - main's log ring (stream 0x53450100) with the line logged just before
    the dump, at that tick, and nothing ticked after it;
  - the InputFrame ring holding ticks 1 up to that tick, no torn slot;
  - the exception record: an access violation on main, or crashpad's
    DumpWithoutCrash code on the watchdog thread (dump) or on main (fault)
    with the reason annotation.

usage:
  rampage_dump_test.py --rampage EXE --read-dump PY --work DIR --scenario
      crash|dump|fault
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

EXCEPTION_ACCESS_VIOLATION = 0xC0000005
SIMULATED = 0x0517A7ED  # crashpad's DumpWithoutCrash
TICK = 240
ROLE_MAIN = 1
ROLE_LOG_WORKER = 5
ROLE_WATCHDOG = 6
LEVEL_WARN = 3
LEVEL_ERROR = 4
STREAM_BREADCRUMBS = 0x53450001
STREAM_INPUT = 0x53450002
STREAM_MAIN_LOG = 0x53450100
RUN_TIMEOUT_S = 60
DUMP_WAIT_S = 20

SCENARIOS = {
    "crash": {
        "args": ["--crash-test", f"after-ticks:{TICK}"],
        "exit": EXCEPTION_ACCESS_VIOLATION,
        "line": f"crash test: an access violation on main after tick {TICK}",
        "level": LEVEL_WARN,
        "code": EXCEPTION_ACCESS_VIOLATION,
        "thread_role": ROLE_MAIN,
        "reason": None,
        "tick": TICK,
        "last": True,
    },
    "dump": {
        "args": ["--dump-test", f"after-ticks:{TICK}"],
        "exit": 0,
        "line": f"dump test: the watchdog writes a dump after tick {TICK}",
        "level": LEVEL_WARN,
        "code": SIMULATED,
        "thread_role": ROLE_WATCHDOG,
        "reason": f"dump test after tick {TICK}",
        "tick": TICK,
        "last": False,
    },
    "fault": {
        "args": ["--inject-throw", "iterate:200"],
        "exit": 1,
        "line": "fault in SDL_AppIterate: ",
        "level": LEVEL_ERROR,
        "code": SIMULATED,
        "thread_role": ROLE_MAIN,
        "reason": "SDL_AppIterate",
        "tick": None,
        "last": True,
    },
}


class Failures:
    def __init__(self):
        self.items = []

    def check(self, condition, message):
        if not condition:
            self.items.append(message)
        return condition

    def equal(self, actual, expected, what):
        return self.check(actual == expected,
                          f"{what}: got {actual!r}, expected {expected!r}")


def run_rampage(rampage, args):
    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    started = time.monotonic()
    result = subprocess.run([str(rampage), *args], capture_output=True,
                            text=True, errors="replace", env=env,
                            timeout=RUN_TIMEOUT_S, check=False)
    print(f"rampage {' '.join(args)}: exit "
          f"{result.returncode & 0xFFFFFFFF:#x} after "
          f"{time.monotonic() - started:.2f} s")
    output = (result.stdout or "") + (result.stderr or "")
    print("--- rampage output\n" + output.rstrip())
    return result, output


def find_dumps(database):
    """Waits until the handler's dump is complete (its size stable)."""
    deadline = time.monotonic() + DUMP_WAIT_S
    previous = None
    while time.monotonic() < deadline:
        dumps = sorted(database.rglob("*.dmp")) if database.exists() else []
        sizes = [p.stat().st_size for p in dumps]
        if dumps and sizes == previous and all(sizes):
            return dumps
        previous = sizes
        time.sleep(0.2)
    return sorted(database.rglob("*.dmp")) if database.exists() else []


def decode(read_dump, path):
    result = subprocess.run([sys.executable, str(read_dump), str(path),
                             "--json"], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        raise RuntimeError(f"read_dump.py failed on {path}: "
                           f"{result.stderr.strip()}")
    text = subprocess.run([sys.executable, str(read_dump), str(path),
                           "--records", "6"], capture_output=True, text=True,
                          check=False)
    print(f"--- read_dump.py {path.name} --records 6\n"
          f"{text.stdout.rstrip()}")
    return json.loads(result.stdout)


def stream(dump, stream_type):
    for entry in dump["se_streams"]:
        if entry["type"] == stream_type:
            return entry
    return None


def check_breadcrumbs(failures, dump, tick):
    """Returns ({role: os_thread_id} of the live threads, main's tick)."""
    table = stream(dump, STREAM_BREADCRUMBS)
    if not failures.check(table is not None, "no breadcrumb stream"):
        return {}, tick
    failures.equal((table["kind"], table["capacity"]), (1, 64),
                   "breadcrumb table header")
    live = {}
    for record in table["records"]:
        if record["state"] == "live":
            live.setdefault(record["role"], record)
    main = live.get(ROLE_MAIN)
    if failures.check(main is not None, "no live main slot"):
        failures.equal((main["slot"], main["name"]), (0, "main"),
                       "main's slot")
        crumb = main["breadcrumb"]
        if tick is None:
            tick = crumb["tick"]
            failures.check(tick > 0, "main's breadcrumb has no tick")
        failures.equal((crumb["role"], crumb["tick"]), (ROLE_MAIN, tick),
                       "main's breadcrumb (role, tick)")
    for role, name in ((ROLE_LOG_WORKER, "log-worker"),
                       (ROLE_WATCHDOG, "watchdog")):
        record = live.get(role)
        if failures.check(record is not None, f"no live {name} slot"):
            failures.equal(record["name"], name, f"slot name of role {role}")
    ids = {role: record["os_thread_id"] for role, record in live.items()}
    return ids, tick


def check_main_log(failures, dump, ids, expected, tick):
    ring = stream(dump, STREAM_MAIN_LOG)
    if not failures.check(ring is not None, "no log ring for slot 0"):
        return
    failures.equal((ring["kind"], ring["capacity"], ring["role"],
                    ring["name"], ring["os_thread_id"]),
                   (2, 4096, ROLE_MAIN, "main", ids.get(ROLE_MAIN)),
                   "main log ring header")
    records = ring["records"]
    if not failures.check(records, "main's log ring is empty"):
        return
    failures.equal(len(records), min(ring["head"], ring["capacity"]),
                   "main's log records decoded")
    line = expected["line"]
    hook = [i for i, r in enumerate(records) if r["message"].startswith(line)]
    if failures.equal(len(hook), 1, f"records '{line}' in main's ring"):
        record = records[hook[0]]
        failures.equal((record["tick"], record["level"]),
                       (tick, expected["level"]), "the hook's record")
        # Nothing ticked after it: a crash or a fault dumps right after
        # the line, and the dump test holds the ticks until the dump is
        # written.
        later = records[hook[0] + 1:]
        if expected["last"]:
            failures.equal([r["message"] for r in later], [],
                           "records after the line")
        failures.equal([r["tick"] for r in later if r["tick"] != tick], [],
                       "ticks of the records after the hook's line")
    indices = [r["index"] for r in records]
    failures.equal(indices, list(range(indices[0], indices[0] + len(records))),
                   "main's log records are consecutive")
    failures.check(any("booted 'rampage'" in r["message"] for r in records),
                   "main's log ring lacks the boot line")


def check_input_ring(failures, dump, ids, tick):
    ring = stream(dump, STREAM_INPUT)
    if not failures.check(ring is not None, "no InputFrame ring"):
        return
    failures.equal((ring["kind"], ring["capacity"], ring["head"],
                    ring["record_size"], ring["os_thread_id"]),
                   (3, 7200, tick, 968, ids.get(ROLE_MAIN)),
                   "InputFrame ring header")
    records = ring["records"]
    failures.equal([r["torn"] for r in records if r["torn"]], [],
                   "torn InputFrame slots")
    ticks = [r["frame"]["tick"] for r in records if "frame" in r]
    failures.equal(ticks, list(range(1, tick + 1)), "InputFrame ticks")
    failures.equal([r["stamp"] for r in records],
                   [2 * n + 2 for n in range(tick)], "InputFrame stamps")


def check_dump(failures, dump, scenario):
    expected = SCENARIOS[scenario]
    failures.equal(dump["problems"], [], "read_dump problems")
    ids, tick = check_breadcrumbs(failures, dump, expected["tick"])
    exception = dump["exception"]
    if failures.check(exception is not None, "no exception record"):
        failures.equal(exception["code"], expected["code"], "exception code")
        failures.equal(exception["thread_id"],
                       ids.get(expected["thread_role"]), "exception thread")
    names = {t["id"]: t["name"] for t in dump["threads"]}
    for role, name in ((ROLE_MAIN, "main"), (ROLE_LOG_WORKER, "log-worker"),
                       (ROLE_WATCHDOG, "watchdog")):
        failures.equal(names.get(ids.get(role)), name,
                       f"OS name of the {name} thread")
    failures.equal(dump["annotations"]["process"].get("se.app"), "rampage",
                   "process annotation se.app")
    failures.equal(dump["annotation_objects"].get("se.dump_reason"),
                   expected["reason"], "annotation se.dump_reason")
    if tick is None:
        return
    check_main_log(failures, dump, ids, expected, tick)
    check_input_ring(failures, dump, ids, tick)
    types = {s["type"] for s in dump["se_streams"]}
    for required in (STREAM_BREADCRUMBS, STREAM_INPUT, STREAM_MAIN_LOG,
                     0x53450100 + 64):
        failures.check(required in types,
                       f"stream {required:#x} missing from {sorted(types)}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", type=pathlib.Path, required=True)
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

    failures = Failures()
    result, output = run_rampage(
        args.rampage, [*expected["args"], "--no-activate", "--pref-dir",
                       str(pref)])
    failures.equal(result.returncode & 0xFFFFFFFF, expected["exit"],
                   "rampage exit code")
    failures.check("crash capture on" in output,
                   "rampage did not start crash capture")
    if args.scenario == "dump":
        failures.check("dump test: the watchdog finished the dump" in output,
                       "main did not see the watchdog finish the dump")
        failures.check("shutdown (success)" in output,
                       "no clean two-phase shutdown after the dump")
    # The log worker writes the file. A crash may end the process before
    # the worker drains the last records, so only the dump test's line is
    # sure to reach it (and stderr).
    log_file = pref / "logs" / "rampage.log"
    if failures.check(log_file.exists(), f"no log file {log_file}"):
        text = log_file.read_text(encoding="utf-8", errors="replace")
        failures.check("booted 'rampage'" in text,
                       "the log file lacks the boot line")
        if args.scenario == "dump":
            for wanted in (expected["line"], "shutdown (success)"):
                failures.check(wanted in text,
                               f"the log file lacks '{wanted}'")
    dumps = find_dumps(pref / "crashdb")
    if failures.equal(len(dumps), 1, "dumps written"):
        check_dump(failures, decode(args.read_dump, dumps[0]), args.scenario)
    if failures.items:
        print(f"FAIL: {args.scenario}")
        for item in failures.items:
            print(f"  {item}")
        return 1
    print(f"PASS: {args.scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
