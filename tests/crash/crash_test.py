"""Runs se_crash_probe in one scenario and checks the dumps it leaves.

The probe (tests/crash/crash_probe.cc) registers dump streams with known
contents, starts crash capture with a fresh database, then crashes or asks
for dumps. This driver waits for the handler's dumps, decodes each with
tools/minidump/read_dump.py --json and checks the exception record, the
probe's threads and their names, the annotations and every registered
stream. The expected contents below mirror the probe.

Scenarios (CTest crash.<scenario>):
  access_violation, abort, terminate, terminate_noexcept, stack_overflow
      the probe's main thread faults; one dump. terminate calls
      std::terminate inside a catch block, terminate_noexcept throws through
      a noexcept function
  dump_without_crash
      the probe's worker thread asks for two dumps and the probe exits 0;
      the second shows the log records and breadcrumb written in between
  missing_handler, bad_handler
      the handler executable does not exist, or is not an executable: Start
      fails with an Error and leaves the fault handlers as they were, the
      probe runs on and exits 0, and no dump is written
  stub
      a build without crashpad: Start fails and the probe runs on

usage:
  crash_test.py --probe EXE --read-dump PY --work DIR --scenario NAME
"""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import time

EXCEPTION_ACCESS_VIOLATION = 0xC0000005
EXCEPTION_STACK_OVERFLOW = 0xC00000FD
STATUS_FATAL_APP_EXIT = 0x40000015  # crashpad's abort() handler
SIMULATED = 0x0517A7ED  # crashpad's DumpWithoutCrash

CRASHES = {
    "access_violation": ("access-violation", EXCEPTION_ACCESS_VIOLATION),
    "abort": ("abort", STATUS_FATAL_APP_EXIT),
    "terminate": ("terminate", STATUS_FATAL_APP_EXIT),
    "terminate_noexcept": ("terminate-noexcept", STATUS_FATAL_APP_EXIT),
    "stack_overflow": ("stack-overflow", EXCEPTION_STACK_OVERFLOW),
}

ROLE_MAIN = 1
ROLE_FORK = 3
ROLE_WATCHDOG = 6
PROBE_TIMEOUT_S = 40
DUMP_WAIT_S = 20


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


def run_probe(probe, args):
    started = time.monotonic()
    result = subprocess.run([str(probe), *args], capture_output=True,
                            text=True, timeout=PROBE_TIMEOUT_S, check=False)
    elapsed = time.monotonic() - started
    print(f"probe {' '.join(args)}: exit {result.returncode & 0xFFFFFFFF:#x} "
          f"after {elapsed:.2f} s")
    print("--- probe stdout\n" + result.stdout.rstrip())
    if result.stderr.strip():
        print("--- probe stderr\n" + result.stderr.rstrip())
    return result


def thread_ids(stdout):
    ids = {}
    for name, tid in re.findall(r"^probe: thread (\w+) (\d+)$", stdout,
                                re.MULTILINE):
        ids[name] = int(tid)
    return ids


def find_dumps(database, count):
    """Waits until |count| complete dumps exist (sizes stable), or times out."""
    deadline = time.monotonic() + DUMP_WAIT_S
    previous = None
    while time.monotonic() < deadline:
        dumps = sorted(database.rglob("*.dmp")) if database.exists() else []
        sizes = [p.stat().st_size for p in dumps]
        if len(dumps) >= count and sizes == previous and all(sizes):
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
                           "--records", "4"], capture_output=True, text=True,
                          check=False)
    print(f"--- read_dump.py {path.name}\n{text.stdout.rstrip()}")
    return json.loads(result.stdout)


def stream(dump, stream_type):
    for entry in dump["se_streams"]:
        if entry["type"] == stream_type:
            return entry
    return None


def check_breadcrumbs(failures, dump, ids, main_tick):
    table = stream(dump, 0x53450001)
    if not failures.check(table is not None, "no breadcrumb stream"):
        return
    failures.equal((table["kind"], table["capacity"], table["head"],
                    table["name"]), (1, 64, 3, "threads"), "breadcrumb header")
    slots = {r["slot"]: r for r in table["records"]}
    failures.equal(sorted(slots), [0, 1, 2], "breadcrumb slots in use")
    expected = {
        0: ("live", "main", ids.get("main"), ROLE_MAIN,
            (ROLE_MAIN, 2, 5, 0, main_tick)),
        1: ("live", "worker", ids.get("worker"), ROLE_WATCHDOG,
            (ROLE_WATCHDOG, 0, 0, 0, 240)),
        2: ("exited", "fork-worker-3", 0x1234, ROLE_FORK,
            (ROLE_FORK, 4, 7, 3, 239)),
    }
    for slot, (state, name, tid, role, crumb) in expected.items():
        record = slots.get(slot)
        if record is None:
            continue
        unpacked = record["breadcrumb"]
        failures.equal(
            (record["state"], record["name"], record["os_thread_id"],
             record["role"], (unpacked["role"], unpacked["phase"],
                              unpacked["system"], unpacked["chunk"],
                              unpacked["tick"])),
            (state, name, tid, role, crumb), f"breadcrumb slot {slot}")
    if 0 in slots:
        failures.equal(slots[0]["breadcrumb"]["phase_name"], "Movement",
                       "main's phase")


def check_main_log(failures, dump, ids, head):
    ring = stream(dump, 0x53450100)
    if not failures.check(ring is not None, "no log ring for slot 0"):
        return
    failures.equal((ring["kind"], ring["capacity"], ring["head"],
                    ring["dropped"], ring["os_thread_id"], ring["role"],
                    ring["name"]),
                   (2, 64, head, 2, ids.get("main"), ROLE_MAIN, "main"),
                   "main log ring header")
    first = head - 64
    actual = [(r["index"], r["message"], r["sequence"], r["tick"], r["line"],
               r["level"], r["file"]) for r in ring["records"]]
    expected = [(i, f"probe log {i}", 1000 + i, 200 + i // 2, 100 + i, i % 5,
                 "crash_probe.cc") for i in range(first, head)]
    failures.equal(actual, expected, "main log records")


def check_worker_log(failures, dump, ids):
    ring = stream(dump, 0x53450101)
    if not failures.check(ring is not None, "no log ring for slot 1"):
        return
    failures.equal((ring["capacity"], ring["head"], ring["os_thread_id"],
                    ring["role"], ring["name"]),
                   (16, 3, ids.get("worker"), ROLE_WATCHDOG, "worker"),
                   "worker log ring header")
    failures.equal([(r["message"], r["sequence"], r["line"])
                    for r in ring["records"]],
                   [(f"worker log {i}", 2000 + i, 300 + i) for i in range(3)],
                   "worker log records")


def check_input_ring(failures, dump, ids):
    ring = stream(dump, 0x53450002)
    if not failures.check(ring is not None, "no InputFrame ring"):
        return
    failures.equal((ring["kind"], ring["capacity"], ring["head"],
                    ring["record_size"], ring["os_thread_id"]),
                   (3, 16, 20, 968, ids.get("main")), "InputFrame ring header")
    records = ring["records"]
    if not failures.equal(len(records), 16, "InputFrame slots"):
        return
    # Index 4 (slot 4, tick 5) was being overwritten by tick 21.
    failures.equal((records[0]["slot"], records[0]["torn"],
                    records[0]["stamp"]), (4, True, 3), "the torn slot")
    valid = records[1:]
    failures.equal([r["frame"]["tick"] for r in valid if "frame" in r],
                   list(range(6, 21)), "InputFrame ticks")
    for record in valid:
        frame = record.get("frame")
        if frame is None:
            failures.check(False, f"slot {record['slot']} not decoded")
            continue
        t = frame["tick"]
        writes = 2 if record["slot"] < 4 else 1
        failures.equal(
            (record["stamp"], frame["held"], frame["axes"][0],
             frame["edge_count"], frame["pressed"], frame["platform_live"]),
            (2 * writes, f"{1 | (1 << (t % 64)):#018x}", t * 0.25, t % 3,
             {str(t % 64): 1}, True), f"frame of tick {t}")
        failures.equal([(e["action"], e["down"], e["stamp_ns"], e["order"],
                         e["source"]) for e in frame["edges"]],
                       [(t % 64, True, t * 1000 + k, k, "replay")
                        for k in range(t % 3)], f"edges of tick {t}")


def check_flight(failures, dump):
    recorder = stream(dump, 0x53450004)
    if not failures.check(recorder is not None, "no flight recorder stream"):
        return
    failures.equal((recorder["kind"], recorder["capacity"],
                    recorder["record_size"], recorder.get("decoded")),
                   (5, 4, 32, False), "flight recorder header")
    failures.equal([r["bytes"] for r in recorder["records"]],
                   [bytes(range(32 * i, 32 * i + 32)).hex() for i in range(4)],
                   "flight recorder bytes")


def check_dump(failures, dump, ids, scenario, code, thread, main_tick,
               main_head):
    failures.equal(dump["problems"], [], "read_dump problems")
    exception = dump["exception"]
    if failures.check(exception is not None, "no exception record"):
        failures.equal(exception["code"], code, "exception code")
        failures.equal(exception["thread_id"], ids.get(thread),
                       f"exception thread ({thread})")
    names = {t["id"]: t["name"] for t in dump["threads"]}
    for name in ("main", "worker"):
        tid = ids.get(name)
        if failures.check(tid in names, f"thread {name} {tid} not in the "
                          f"dump's thread list {sorted(names)}"):
            failures.equal(names[tid], f"se-probe-{name}",
                           f"name of thread {tid}")
    failures.equal(dump["annotations"]["process"].get("se.probe"),
                   "crash_probe", "process annotation se.probe")
    failures.equal(dump["annotations"]["process"].get("se.scenario"),
                   scenario, "process annotation se.scenario")
    check_breadcrumbs(failures, dump, ids, main_tick)
    check_main_log(failures, dump, ids, main_head)
    check_worker_log(failures, dump, ids)
    check_input_ring(failures, dump, ids)
    check_flight(failures, dump)
    types = sorted(s["type"] for s in dump["se_streams"])
    failures.equal(types, [0x53450001, 0x53450002, 0x53450004, 0x53450100,
                           0x53450101], "first-party stream types")


def run_crash(args, failures, scenario):
    kind, code = CRASHES[scenario]
    database = args.work / "db"
    result = run_probe(args.probe, ["--database", str(database),
                                    f"--crash={kind}"])
    ids = thread_ids(result.stdout)
    failures.check({"main", "worker"} <= set(ids),
                   f"probe did not report its threads: {ids}")
    failures.check("probe: crash capture running" in result.stdout,
                   "crash capture did not start")
    # crashpad's handler ends the process with the exception code.
    failures.equal(result.returncode & 0xFFFFFFFF, code, "probe exit code")
    dumps = find_dumps(database, 1)
    if not failures.equal(len(dumps), 1, "dumps written"):
        return
    dump = decode(args.read_dump, dumps[0])
    check_dump(failures, dump, ids, kind, code, "main", 240, 80)
    objects = dump["annotation_objects"]
    failures.check("se.dump_reason" not in objects,
                   f"a crash dump has a dump reason: {objects}")
    if scenario == "terminate":
        # std::terminate from a catch block: the exception is active.
        failures.equal(objects.get("se.terminate"),
                       "std::terminate with exception: probe terminate",
                       "annotation se.terminate")
    elif scenario == "terminate_noexcept":
        # A throw through noexcept: MSVC calls std::terminate with the
        # exception active, clang-cl before it is handled.
        failures.check(objects.get("se.terminate") in (
            "std::terminate with exception: probe noexcept",
            "std::terminate without an active exception"),
            f"annotation se.terminate: {objects.get('se.terminate')!r}")
    else:
        failures.check("se.terminate" not in objects,
                       f"se.terminate set without std::terminate: {objects}")


def run_dump_without_crash(args, failures):
    database = args.work / "db"
    result = run_probe(args.probe, ["--database", str(database),
                                    "--dump-without-crash"])
    ids = thread_ids(result.stdout)
    failures.equal(result.returncode, 0, "probe exit code")
    for line in ("probe: dump 1 written", "probe: dump 2 written",
                 "probe: guard released"):
        failures.check(line in result.stdout, f"missing '{line}'")
    dumps = find_dumps(database, 2)
    if not failures.equal(len(dumps), 2, "dumps written"):
        return
    decoded = [decode(args.read_dump, path) for path in dumps]
    by_reason = {d["annotation_objects"].get("se.dump_reason"): d
                 for d in decoded}
    failures.equal(sorted(by_reason, key=str),
                   ["probe dump 1", "probe dump 2"], "dump reasons")
    for reason, tick, head in (("probe dump 1", 240, 80),
                               ("probe dump 2", 241, 90)):
        if reason in by_reason:
            check_dump(failures, by_reason[reason], ids,
                       "dump-without-crash", SIMULATED, "worker", tick, head)


def run_start_failure(args, failures, handler, message):
    database = args.work / "db"
    probe_args = ["--database", str(database), "--expect-start-failure",
                  "--crash=abort"]
    if handler is not None:
        probe_args += ["--handler", str(handler)]
    result = run_probe(args.probe, probe_args)
    failures.equal(result.returncode, 0, "probe exit code")
    failures.check("probe: crash capture unavailable: " in result.stdout,
                   "Start did not report an Error")
    failures.check(message in result.stdout,
                   f"the Error does not say '{message}'")
    failures.check("probe: fault handlers unchanged" in result.stdout,
                   "a failed Start changed the fault handlers")
    failures.check("probe: running without crash capture" in result.stdout,
                   "the probe did not run on without crash capture")
    dumps = sorted(database.rglob("*.dmp")) if database.exists() else []
    failures.equal(dumps, [], "dumps written")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--read-dump", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--scenario", required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)

    failures = Failures()
    if args.scenario in CRASHES:
        run_crash(args, failures, args.scenario)
    elif args.scenario == "dump_without_crash":
        run_dump_without_crash(args, failures)
    elif args.scenario == "missing_handler":
        handler = args.work / "missing" / "crashpad_handler.exe"
        run_start_failure(args, failures, handler,
                          f"crash handler not found: {handler}")
    elif args.scenario == "bad_handler":
        # A file that exists but is no executable: the launch fails after
        # crashpad has installed its handlers, which Start must put back.
        handler = args.work / "bad" / "crashpad_handler.exe"
        handler.parent.mkdir(parents=True)
        handler.write_text("not an executable\n", encoding="ascii")
        run_start_failure(args, failures, handler,
                          f"crash handler did not start: {handler}")
    elif args.scenario == "stub":
        run_start_failure(args, failures, None,
                          "crash capture is not built into this binary")
    else:
        print(f"unknown scenario {args.scenario}")
        return 2
    if failures.items:
        print(f"FAIL: {args.scenario}")
        for item in failures.items:
            print(f"  {item}")
        return 1
    print(f"PASS: {args.scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
