"""Drives legacy_replay for the legacy behaviour-reference tests (label "legacy").

A scenario NAME in a fixture directory DIR is the pair DIR/NAME.scenario.json and
DIR/NAME.seinput. Run from the repository root: the legacy tree reads config/
and assets/ relative to the working directory.

usage:
  legacy_test.py selfdet --exe EXE --scenario DIR/NAME --work DIR [--ticks N]
      Two unpaced runs and runs at --fps 30, 60, 144 and 160 must give the same
      projection run hash.
  legacy_test.py live-replay --exe EXE --scenario DIR/NAME --work DIR
                 [--ticks N] [--fps HZ]
      A paced run in a visible window records its keyboard input; replaying
      that recording headless must give the same projection run hash.
  legacy_test.py reference --exe EXE --scenario DIR/NAME --work DIR [--update]
      The per-tick projection hashes must equal the golden DIR/NAME.sehash,
      and the projection must equal the golden DIR/NAME.seproj.gz line for
      line. --update (or SE_UPDATE_GOLDENS=1) rewrites both goldens instead
      (a golden whose content is unchanged is left as it is).

Exit status 0 on success, 1 on a mismatch or a failed run.
"""

import argparse
import gzip
import os
import pathlib
import subprocess
import sys

SELFDET_FPS = (30, 60, 144, 160)


def run_replay(exe, args, work, label):
    """Runs legacy_replay; returns (ticks, run_hash, projection_path)."""
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
    if "projection-hash" not in values or "ticks" not in values:
        sys.exit(f"FAIL: {label}: no projection hash in output\n{result.stdout}")
    return int(values["ticks"]), values["projection-hash"], projection


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


def cmd_selfdet(args):
    base_args = scenario_args(args.scenario)
    if args.ticks:
        base_args += ["--ticks", args.ticks]
    work = pathlib.Path(args.work)
    ticks, expected, baseline = run_replay(args.exe, base_args, work, "unpaced")
    runs = [("repeat", [])] + [(f"fps{fps}", ["--fps", fps])
                               for fps in SELFDET_FPS]
    failed = False
    for label, extra in runs:
        run_ticks, digest, projection = run_replay(
            args.exe, base_args + extra, work, label)
        if run_ticks != ticks or digest != expected:
            failed = True
            print(f"FAIL: {label}: {run_ticks} ticks, hash {digest}; "
                  f"unpaced: {ticks} ticks, hash {expected}")
            print(first_difference(baseline, projection))
        else:
            print(f"ok: {label}: {run_ticks} ticks, hash {digest}")
    return 1 if failed else 0


def cmd_live_replay(args):
    work = pathlib.Path(args.work)
    scenario = ["--scenario", f"{args.scenario}.scenario.json"]
    recording = work / "live.seinput"
    work.mkdir(parents=True, exist_ok=True)
    ticks, live_hash, live = run_replay(
        args.exe, scenario + ["--live", "--fps", args.fps, "--ticks", args.ticks,
                              "--record", recording], work, "live")
    replay_ticks, replay_hash, replay = run_replay(
        args.exe, scenario + ["--inputs", recording, "--ticks", ticks],
        work, "replay")
    if replay_ticks != ticks or replay_hash != live_hash:
        print(f"FAIL: live run {ticks} ticks, hash {live_hash}; "
              f"headless replay {replay_ticks} ticks, hash {replay_hash}")
        print(first_difference(live, replay))
        return 1
    print(f"ok: live {ticks} ticks replayed headless, hash {live_hash}")
    return 0


def write_hashes(path, blocks, run_hash, name):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(f"# sehash v1 scenario={name} ticks={max(blocks)}\n")
        f.write("# per tick: <tick> <fnv1a64 of the projection block>\n")
        for tick in sorted(blocks):
            f.write(f"{tick} {blocks[tick][0]}\n")
        f.write(f"# run {len(blocks)} {run_hash}\n")


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
    ticks, run_hash, projection = run_replay(
        args.exe, scenario_args(args.scenario), work, name)
    blocks = read_blocks(projection)
    if args.update or os.environ.get("SE_UPDATE_GOLDENS") == "1":
        write_hashes(golden, blocks, run_hash, name)
        changed = write_projection_golden(golden_projection, projection)
        print(f"updated {golden} and {golden_projection.name}"
              f"{'' if changed else ' (unchanged)'}: {ticks} ticks, "
              f"run hash {run_hash}")
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
    for command in ("selfdet", "live-replay", "reference"):
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
    return cmd_reference(args)


if __name__ == "__main__":
    sys.exit(main())
