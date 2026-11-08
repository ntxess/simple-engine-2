"""Compares two semantic projections (.seproj or .seproj.gz).

The legacy reference (core/util/SemanticProjection.cpp) and the engine
(games/rampage, WriteProjection) write the same text format: per tick a line
"t <tick> [<hash>]", then one line per entity in spawn order,

    <serial> <x> <y> <rotation> <HP> [<stat>=<value> ...]

with %.9g numbers and "-" for an absent value. For every tick this requires
(docs/milestone-specs.md, M3b):

  - the same ticks, and in each the same set of live spawn serials;
  - every stat value, HP included, equal as text (exact float equality);
  - |dx| and |dy| <= the position tolerance (1e-3 px);
  - the rotation within the rotation tolerance (1e-3 degrees), modulo 360.

It reports the first difference (the tick, the entity and both lines), then
a summary of the largest deviations. Exit status: 0 equal within the
tolerances, 1 different, 2 bad input.

usage:
  refcmp.py ACTUAL EXPECTED [--pos-tol PX] [--rot-tol DEG] [--max-report N]
"""

import argparse
import gzip
import math
import sys


class Projection:
    def __init__(self, path):
        self.path = path
        self.ticks = {}  # tick -> {serial: (line, fields)}
        self.order = []


def read_projection(path):
    opener = gzip.open if path.endswith(".gz") else open
    projection = Projection(path)
    current = None
    with opener(path, "rt", encoding="utf-8", newline="") as f:
        for number, raw in enumerate(f, 1):
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split(" ")
            if fields[0] == "t":
                if len(fields) not in (2, 3):
                    raise ValueError(f"{path}:{number}: malformed tick line")
                tick = int(fields[1])
                if tick in projection.ticks:
                    raise ValueError(f"{path}:{number}: tick {tick} repeated")
                current = {}
                projection.ticks[tick] = current
                projection.order.append(tick)
                continue
            if current is None:
                raise ValueError(f"{path}:{number}: entity before a tick")
            if len(fields) < 5:
                raise ValueError(f"{path}:{number}: malformed entity line")
            serial = int(fields[0])
            if serial in current:
                raise ValueError(f"{path}:{number}: serial {serial} repeated")
            current[serial] = (line, fields)
    return projection


def number(text):
    return None if text == "-" else float(text)


def stats_of(fields):
    """HP and the named stats, as text."""
    stats = {"HP": fields[4]}
    for item in fields[5:]:
        name, sep, value = item.partition("=")
        if not sep:
            raise ValueError(f"malformed stat '{item}'")
        stats[name] = value
    return stats


def rotation_delta(a, b):
    d = math.fmod(abs(a - b), 360.0)
    return min(d, 360.0 - d)


class Deviation:
    def __init__(self, name):
        self.name = name
        self.value = 0.0
        self.where = None

    def add(self, value, tick, serial):
        if value > self.value:
            self.value = value
            self.where = (tick, serial)

    def __str__(self):
        if self.where is None:
            return f"  max {self.name}: 0"
        return (f"  max {self.name}: {self.value:.6g} "
                f"(tick {self.where[0]}, entity {self.where[1]})")


def compare(actual, expected, pos_tol, rot_tol, max_report):
    problems = []
    dx = Deviation("|dx| px")
    dy = Deviation("|dy| px")
    drot = Deviation("|drot| deg")
    compared = 0

    def report(message):
        problems.append(message)

    if actual.order != expected.order:
        missing = sorted(set(expected.order) - set(actual.order))
        extra = sorted(set(actual.order) - set(expected.order))
        report(f"tick sets differ: missing {missing[:5]}, extra {extra[:5]}")

    for tick in expected.order:
        if tick not in actual.ticks:
            continue
        a_tick = actual.ticks[tick]
        e_tick = expected.ticks[tick]
        if set(a_tick) != set(e_tick):
            gone = sorted(set(e_tick) - set(a_tick))
            new = sorted(set(a_tick) - set(e_tick))
            report(f"tick {tick}: live entities differ: expected but missing "
                   f"{gone}, unexpected {new}")
        for serial in sorted(set(a_tick) & set(e_tick)):
            a_line, a = a_tick[serial]
            e_line, e = e_tick[serial]
            compared += 1
            reasons = []
            ax, ay, ar = number(a[1]), number(a[2]), number(a[3])
            ex, ey, er = number(e[1]), number(e[2]), number(e[3])
            for label, av, ev, dev in (("x", ax, ex, dx), ("y", ay, ey, dy)):
                if (av is None) != (ev is None):
                    reasons.append(f"{label} present in one only")
                elif av is not None:
                    d = abs(av - ev)
                    if math.isnan(d):
                        if not (math.isnan(av) and math.isnan(ev)):
                            reasons.append(f"{label} NaN in one only")
                    else:
                        dev.add(d, tick, serial)
                        if d > pos_tol:
                            reasons.append(f"|d{label}| = {d:.6g}")
            if (ar is None) != (er is None):
                reasons.append("rotation present in one only")
            elif ar is not None:
                d = rotation_delta(ar, er)
                if math.isnan(d):
                    if not (math.isnan(ar) and math.isnan(er)):
                        reasons.append("rotation NaN in one only")
                else:
                    drot.add(d, tick, serial)
                    if d > rot_tol:
                        reasons.append(f"|drotation| = {d:.6g} deg")
            a_stats = stats_of(a)
            e_stats = stats_of(e)
            if a_stats != e_stats:
                names = sorted(set(a_stats) | set(e_stats))
                diff = [n for n in names if a_stats.get(n) != e_stats.get(n)]
                reasons.append("stats differ: " + ", ".join(
                    f"{n} {a_stats.get(n, 'absent')} vs "
                    f"{e_stats.get(n, 'absent')}" for n in diff))
            if reasons:
                report(f"tick {tick}, entity {serial}: {'; '.join(reasons)}\n"
                       f"    actual:   {a_line}\n"
                       f"    expected: {e_line}")
    return problems, (dx, dy, drot), compared


def main():
    parser = argparse.ArgumentParser(
        description="Compare two .seproj projections within tolerances.")
    parser.add_argument("actual")
    parser.add_argument("expected")
    parser.add_argument("--pos-tol", type=float, default=1e-3)
    parser.add_argument("--rot-tol", type=float, default=1e-3)
    parser.add_argument("--max-report", type=int, default=10,
                        help="differences listed after the first")
    args = parser.parse_args()
    try:
        actual = read_projection(args.actual)
        expected = read_projection(args.expected)
    except (OSError, ValueError) as e:
        print(f"refcmp: {e}", file=sys.stderr)
        return 2
    try:
        problems, deviations, compared = compare(
            actual, expected, args.pos_tol, args.rot_tol, args.max_report)
    except ValueError as e:
        print(f"refcmp: {e}", file=sys.stderr)
        return 2

    print(f"refcmp: {args.actual} against {args.expected}")
    print(f"  {len(expected.order)} ticks, {compared} entity lines compared")
    if problems:
        print(f"FIRST DIFFERENCE: {problems[0]}")
        for problem in problems[1:1 + args.max_report]:
            print(f"then: {problem}")
        if len(problems) > 1 + args.max_report:
            print(f"... {len(problems) - 1 - args.max_report} more")
    print("largest deviations:")
    for deviation in deviations:
        print(deviation)
    if problems:
        print(f"refcmp: {len(problems)} differences")
        return 1
    print("refcmp: equal within the tolerances")
    return 0


if __name__ == "__main__":
    sys.exit(main())
