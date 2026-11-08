"""Drives legacy_replay for the legacy behaviour-reference tests (label "legacy").

A scenario NAME in a fixture directory DIR is the pair DIR/NAME.scenario.json and
DIR/NAME.seinput. Run from the repository root: the legacy tree reads config/
and assets/ relative to the working directory.

usage:
  legacy_test.py selfdet --exe EXE --scenario DIR/NAME --work DIR [--ticks N]
      Two unpaced runs and runs at --fps 10, 30, 60, 144 and 160 must give the
      same projection run hash, and each run must render the number of frames
      its pacing implies (one tick per frame unpaced; at most 8 per frame).
  legacy_test.py live-replay --exe EXE --scenario DIR/NAME --work DIR
                 [--ticks N] [--fps HZ]
      A paced run in a visible window records its keyboard input; replaying
      that recording headless must give the same projection run hash. Needs
      an interactive desktop (label "desktop", docs/gates.md).
  legacy_test.py record-replay --exe EXE --scenario DIR/NAME --work DIR
      Headless: a replay of DIR/NAME.seinput recorded with --record must hold
      exactly the frames it replayed (the sparse file expanded per tick), and
      replaying that recording must give the same projection run hash.
  legacy_test.py reference --exe EXE --scenario DIR/NAME --work DIR [--update]
      The per-tick projection hashes must equal the golden DIR/NAME.sehash,
      and the projection must equal the golden DIR/NAME.seproj.gz line for
      line. --update (or SE_UPDATE_GOLDENS=1) rewrites both goldens instead
      of checking them, with a warning (a golden whose content is unchanged
      is left as it is). scripts/ci.ps1 clears SE_UPDATE_GOLDENS.

Exit status 0 on success, 1 on a mismatch or a failed run.
"""

import argparse
import gzip
import os
import pathlib
import subprocess
import sys

# 30/60/144/160 from docs/milestone-specs.md (M1a exit); at 10 Hz, 12 ticks are
# due per frame, so every frame hits the cap of MAX_STEPS ticks and the
# accumulator the 0.25 s clamp (Engine::maxStepsPerFrame,
# Engine::maxAccumulatedMicroseconds).
SELFDET_FPS = (10, 30, 60, 144, 160)
MAX_STEPS = 8


def run_replay(exe, args, work, label):
    """Runs legacy_replay; returns (ticks, run_hash, projection_path, frames)."""
    work.mkdir(parents=True, exist_ok=True)
    projection = work / f"{label}.seproj"
    cmd = [str(pathlib.Path(exe).resolve()), *map(str, args),
           "--projection-out", str(projection)]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as error:
        sys.exit(f"FAIL: {label}: cannot run {cmd[0]}: {error}")
    if result.returncode != 0:
        sys.exit(f"FAIL: {label}: legacy_replay exited {result.returncode}\n"
                 f"  {' '.join(cmd)}\n{result.stdout}{result.stderr}")
    values = {}
    for line in result.stdout.splitlines():
        key, _, value = line.partition(" ")
        values[key] = value.strip()
    if not {"projection-hash", "ticks", "frames"} <= values.keys():
        sys.exit(f"FAIL: {label}: no projection hash in output\n{result.stdout}")
    return (int(values["ticks"]), values["projection-hash"], projection,
            int(values["frames"]))


def read_blocks(path):
    """Reads a .seproj (or .seproj.gz) into {tick: (hash, [entity lines])}."""
    blocks = {}
    current = None
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                continue
            if line.startswith("t "):
                _, tick, digest = line.split(" ")
                current = []
                blocks[int(tick)] = (digest, current)
            elif current is not None:
                current.append(line)
    return blocks


def first_difference(a_path, b_path):
    """Describes the first tick whose projection block differs."""
    a = read_blocks(a_path)
    b = read_blocks(b_path)
    for tick in sorted(set(a) | set(b)):
        if a.get(tick) != b.get(tick):
            lines = [f"first differing tick: {tick}"]
            for name, blocks in ((a_path.name, a), (b_path.name, b)):
                block = blocks.get(tick)
                if block is None:
                    lines.append(f"  {name}: (no block)")
                    continue
                lines.append(f"  {name}: hash {block[0]}")
                lines.extend(f"    {entity}" for entity in block[1][:40])
            return "\n".join(lines)
    return "projections are identical"


def scenario_args(scenario):
    return ["--scenario", f"{scenario}.scenario.json",
            "--inputs", f"{scenario}.seinput"]


def projection_rate(path):
    """The tick rate from a .seproj header ("# seproj v1 rate=120")."""
    with open(path, encoding="ascii") as f:
        header = f.readline().split()
    return int(next(field for field in header
                    if field.startswith("rate="))[len("rate="):])


def expected_frames(ticks, fps, rate):
    """Frames a headless run renders for `ticks` ticks (core/Engine.cpp,
    runHeadless). Unpaced, one tick per frame. With the synthetic clock at
    fps Hz, rate/fps ticks are due per frame; while that is below the cap,
    tick n runs in frame ceil(n * fps / rate). From rate/fps >= 8 every frame
    runs exactly MAX_STEPS ticks and the backlog sits at the 0.25 s clamp."""
    if fps == 0:
        return ticks
    if rate >= MAX_STEPS * fps:
        return -(-ticks // MAX_STEPS)
    return -(-ticks * fps // rate)


def cmd_selfdet(args):
    base_args = scenario_args(args.scenario)
    if args.ticks:
        base_args += ["--ticks", args.ticks]
    work = pathlib.Path(args.work)
    ticks, expected, baseline, frames = run_replay(
        args.exe, base_args, work, "unpaced")
    rate = projection_rate(baseline)
    runs = [("unpaced", 0, None), ("repeat", 0, [])]
    runs += [(f"fps{fps}", fps, ["--fps", fps]) for fps in SELFDET_FPS]
    failed = False
    for label, fps, extra in runs:
        if extra is not None:
            run_ticks, digest, projection, frames = run_replay(
                args.exe, base_args + extra, work, label)
        else:
            run_ticks, digest, projection = ticks, expected, baseline
        want_frames = expected_frames(run_ticks, fps, rate)
        if run_ticks != ticks or digest != expected:
            failed = True
            print(f"FAIL: {label}: {run_ticks} ticks, hash {digest}; "
                  f"unpaced: {ticks} ticks, hash {expected}")
            print(first_difference(baseline, projection))
        elif frames != want_frames:
            failed = True
            print(f"FAIL: {label}: {frames} frames rendered for {run_ticks} "
                  f"ticks at {rate} Hz; the pacing gives {want_frames}")
        else:
            print(f"ok: {label}: {run_ticks} ticks in {frames} frames, "
                  f"hash {digest}")
    return 1 if failed else 0


def cmd_live_replay(args):
    work = pathlib.Path(args.work)
    scenario = ["--scenario", f"{args.scenario}.scenario.json"]
    recording = work / "live.seinput"
    work.mkdir(parents=True, exist_ok=True)
    ticks, live_hash, live, _ = run_replay(
        args.exe, scenario + ["--live", "--fps", args.fps, "--ticks", args.ticks,
                              "--record", recording], work, "live")
    replay_ticks, replay_hash, replay, _ = run_replay(
        args.exe, scenario + ["--inputs", recording, "--ticks", ticks],
        work, "replay")
    if replay_ticks != ticks or replay_hash != live_hash:
        print(f"FAIL: live run {ticks} ticks, hash {live_hash}; "
              f"headless replay {replay_ticks} ticks, hash {replay_hash}")
        print(first_difference(live, replay))
        return 1
    print(f"ok: live {ticks} ticks replayed headless, hash {live_hash}")
    return 0


def parse_seinput(path):
    """Reads a .seinput into a list of (tick, held, pressed, released)."""
    frames = []
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                frames.append(tuple(int(value, 16) for value in line.split()))
    return frames


def expand_seinput(frames, ticks):
    """The frames LegacyInputReader::next gives for ticks 1..ticks: a tick
    without a line repeats the previous held state with no edges, and lines
    for earlier ticks still update the held state."""
    by_tick = {frame[0]: frame for frame in frames}
    held = 0
    for frame in frames:
        if frame[0] < 1:
            held = frame[1]
    expanded = []
    for tick in range(1, ticks + 1):
        frame = by_tick.get(tick, (tick, held, 0, 0))
        held = frame[1]
        expanded.append(frame)
    return expanded


def cmd_record_replay(args):
    """Replays the scenario's inputs while recording them, checks that the
    recording holds exactly the frames the replay consumed, then replays the
    recording: the run hash must be the same."""
    work = pathlib.Path(args.work)
    scenario = ["--scenario", f"{args.scenario}.scenario.json"]
    source = parse_seinput(f"{args.scenario}.seinput")
    # The recorder must see every field set somewhere, or a swapped or dropped
    # field could go unnoticed.
    for index, field in ((1, "held"), (2, "pressed"), (3, "released")):
        if not any(frame[index] for frame in source):
            print(f"FAIL: {args.scenario}.seinput has no non-zero {field} "
                  "mask; the round trip would not cover it")
            return 1
    recording = work / "recorded.seinput"
    ticks, recorded_hash, recorded, _ = run_replay(
        args.exe, scenario_args(args.scenario) + ["--record", recording],
        work, "record")
    expected = expand_seinput(source, ticks)
    produced = parse_seinput(recording)
    if produced != expected:
        for index in range(max(len(expected), len(produced))):
            want = expected[index] if index < len(expected) else None
            got = produced[index] if index < len(produced) else None
            if want != got:
                print(f"FAIL: recorded frame {index + 1} is {got}, the "
                      f"replayed input was {want} (tick, held, pressed, "
                      f"released); {recording}")
                return 1
    replay_ticks, replay_hash, replay, _ = run_replay(
        args.exe, scenario + ["--inputs", recording], work, "replay")
    if replay_ticks != ticks or replay_hash != recorded_hash:
        print(f"FAIL: recording run {ticks} ticks, hash {recorded_hash}; "
              f"replay of the recording {replay_ticks} ticks, "
              f"hash {replay_hash}")
        print(first_difference(recorded, replay))
        return 1
    print(f"ok: {ticks} recorded frames match the replayed input; replaying "
          f"them gives hash {recorded_hash}")
    return 0


def write_hashes(path, blocks, run_hash, name):
    """Writes the .sehash golden; keeps an existing golden whose content is
    the same (line endings aside, which git may have converted)."""
    lines = [f"# sehash v1 scenario={name} ticks={max(blocks)}",
             "# per tick: <tick> <fnv1a64 of the projection block>"]
    lines += [f"{tick} {blocks[tick][0]}" for tick in sorted(blocks)]
    lines.append(f"# run {len(blocks)} {run_hash}")
    text = "\n".join(lines) + "\n"
    if path.exists():
        if path.read_bytes().replace(b"\r\n", b"\n") == text.encode("ascii"):
            return False
    path.write_bytes(text.encode("ascii"))
    return True


def read_hashes(path):
    hashes = {}
    with open(path, encoding="ascii") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            tick, digest = line.split()
            hashes[int(tick)] = digest
    return hashes


def write_projection_golden(path, projection):
    """Gzips the projection deterministically (no name, mtime 0); keeps an
    existing golden whose content is the same."""
    data = projection.read_bytes()
    if path.exists():
        with gzip.open(path, "rb") as f:
            if f.read() == data:
                return False
    path.write_bytes(gzip.compress(data, compresslevel=9, mtime=0))
    return True


def cmd_reference(args):
    work = pathlib.Path(args.work)
    name = pathlib.Path(args.scenario).name
    golden = pathlib.Path(f"{args.scenario}.sehash")
    golden_projection = pathlib.Path(f"{args.scenario}.seproj.gz")
    ticks, run_hash, projection, _ = run_replay(
        args.exe, scenario_args(args.scenario), work, name)
    blocks = read_blocks(projection)
    if args.update or os.environ.get("SE_UPDATE_GOLDENS") == "1":
        # A re-baseline replaces the check, so say so loudly. scripts/ci.ps1
        # clears SE_UPDATE_GOLDENS: the commit gate always checks.
        written = [path.name for path, changed in (
            (golden, write_hashes(golden, blocks, run_hash, name)),
            (golden_projection,
             write_projection_golden(golden_projection, projection)))
            if changed]
        trigger = "--update" if args.update else "SE_UPDATE_GOLDENS=1"
        action = ("rewrote " + " and ".join(written) if written
                  else "goldens unchanged")
        print(f"WARNING: {trigger}: {name} was not checked; {action}: "
              f"{ticks} ticks, run hash {run_hash}")
        return 0
    for path in (golden, golden_projection):
        if not path.exists():
            print(f"FAIL: no golden {path}; run with SE_UPDATE_GOLDENS=1")
            return 1
    expected = read_hashes(golden)
    for tick in sorted(set(expected) | set(blocks)):
        produced = blocks.get(tick, (None, []))
        if expected.get(tick) != produced[0]:
            print(f"FAIL: {name}: tick {tick}: hash {produced[0]}, "
                  f"golden {expected.get(tick)} ({golden})")
            print(f"  produced block ({projection}):")
            print("\n".join(f"    {line}" for line in produced[1][:40]))
            return 1
    if read_blocks(golden_projection) != blocks:
        print(f"FAIL: {name}: projection differs from {golden_projection}")
        print(first_difference(golden_projection, projection))
        return 1
    print(f"ok: {name}: {ticks} ticks match {golden.name} and "
          f"{golden_projection.name}, run hash {run_hash}")
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("selfdet", "live-replay", "record-replay", "reference"):
        p = sub.add_parser(command)
        p.add_argument("--exe", required=True)
        p.add_argument("--scenario", required=True)
        p.add_argument("--work", required=True)
        p.add_argument("--ticks", default="")
        if command == "live-replay":
            p.add_argument("--fps", default="60")
        if command == "reference":
            p.add_argument("--update", action="store_true")
    args = parser.parse_args()
    if args.command == "selfdet":
        return cmd_selfdet(args)
    if args.command == "live-replay":
        if not args.ticks:
            args.ticks = "600"
        return cmd_live_replay(args)
    if args.command == "record-replay":
        return cmd_record_replay(args)
    return cmd_reference(args)


if __name__ == "__main__":
    sys.exit(main())
