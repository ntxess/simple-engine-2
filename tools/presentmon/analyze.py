"""Summarises a PresentMon capture: displayed latency, dropped and repeated frames.

Reads a PresentMon CSV (the v1 metric columns: PresentMon 1.x, or 2.x with
--v1_metrics) and reports, for one process (rampage.exe by default):

  displayed latency   MsUntilDisplayed of the displayed presents, p50/p99
                      (the time from the Present call to the display)
  dropped frames      presents with Dropped = 1 (never shown)
  repeated frames     refreshes that showed the previous frame again: a
                      displayed present whose MsBetweenDisplayChange spans
                      k refresh periods repeated k - 1 refreshes
  present intervals   MsBetweenPresents and MsInPresentAPI, p50/p99

The refresh period is --refresh-hz when given, else the median
MsBetweenDisplayChange. Percentiles are nearest-rank. With --max-p99-ms,
--max-dropped or --max-repeated-percent the exit status is 1 when a limit
is exceeded (the gate in docs/gates.md); malformed input exits 2.

Python 3 standard library only. CTest runs analyze_test.py (presentmon.analyze)
on a synthetic CSV; running PresentMon itself is a docs/gates.md entry.

usage:
  analyze.py CAPTURE.csv [--process rampage.exe] [--refresh-hz HZ] [--json]
             [--max-p99-ms MS] [--max-dropped N] [--max-repeated-percent P]
"""

import argparse
import csv
import json
import math
import sys

REQUIRED = ("MsBetweenPresents", "MsUntilDisplayed", "MsBetweenDisplayChange",
            "MsInPresentAPI", "Dropped")
PROCESS_COLUMNS = ("Application", "ProcessName")


class CaptureError(Exception):
    """The CSV is not a PresentMon v1-metrics capture."""


def percentile(values, p):
    """Nearest-rank percentile p in [0, 100] of |values|; None if empty."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def median(values):
    return percentile(values, 50)


def as_float(row, name, line):
    text = (row.get(name) or "").strip()
    if text in ("", "NA", "N/A"):
        return None
    try:
        return float(text)
    except ValueError as error:
        raise CaptureError(f"line {line}: {name} is not a number: {text!r}") \
            from error


def read_rows(stream, process):
    """The rows of |process| (case-insensitive; all when None)."""
    reader = csv.DictReader(stream)
    if reader.fieldnames is None:
        raise CaptureError("empty file")
    fields = [name.strip() for name in reader.fieldnames]
    missing = [name for name in REQUIRED if name not in fields]
    if missing:
        raise CaptureError("missing columns: " + ", ".join(missing) +
                           " (PresentMon 2.x needs --v1_metrics)")
    process_column = next((c for c in PROCESS_COLUMNS if c in fields), None)
    rows = []
    for index, raw in enumerate(reader, start=2):
        row = {key.strip(): value for key, value in raw.items() if key}
        if process is not None and process_column is not None:
            name = (row.get(process_column) or "").strip()
            if name.lower() != process.lower():
                continue
        rows.append((index, row))
    return rows


def analyze(rows, refresh_hz=None):
    """The summary of |rows| ((line, row) pairs) as a dict."""
    latencies = []
    between_presents = []
    in_present = []
    display_changes = []
    dropped = 0
    displayed = 0
    for line, row in rows:
        is_dropped = (row.get("Dropped") or "").strip() in ("1", "True", "true")
        value = as_float(row, "MsBetweenPresents", line)
        if value is not None:
            between_presents.append(value)
        value = as_float(row, "MsInPresentAPI", line)
        if value is not None:
            in_present.append(value)
        if is_dropped:
            dropped += 1
            continue
        displayed += 1
        value = as_float(row, "MsUntilDisplayed", line)
        if value is not None:
            latencies.append(value)
        value = as_float(row, "MsBetweenDisplayChange", line)
        if value is not None and value > 0.0:
            display_changes.append(value)

    if refresh_hz is not None and refresh_hz > 0:
        period_ms = 1000.0 / refresh_hz
    else:
        period_ms = median(display_changes)
    repeated = 0
    refreshes = 0
    if period_ms:
        for change in display_changes:
            spans = max(1, round(change / period_ms))
            refreshes += spans
            repeated += spans - 1
    return {
        "presents": len(rows),
        "displayed": displayed,
        "dropped": dropped,
        "refresh_period_ms": period_ms,
        "refreshes": refreshes,
        "repeated": repeated,
        "repeated_percent": 100.0 * repeated / refreshes if refreshes else 0.0,
        "displayed_latency_ms": {
            "p50": percentile(latencies, 50),
            "p99": percentile(latencies, 99),
            "max": max(latencies) if latencies else None,
        },
        "between_presents_ms": {
            "p50": percentile(between_presents, 50),
            "p99": percentile(between_presents, 99),
        },
        "in_present_api_ms": {
            "p50": percentile(in_present, 50),
            "p99": percentile(in_present, 99),
        },
    }


def fmt(value):
    return "n/a" if value is None else f"{value:.3f}"


def describe(summary, process):
    latency = summary["displayed_latency_ms"]
    presents = summary["between_presents_ms"]
    api = summary["in_present_api_ms"]
    return "\n".join([
        f"process: {process or 'all'}",
        f"presents: {summary['presents']} ({summary['displayed']} displayed, "
        f"{summary['dropped']} dropped)",
        f"refresh period: {fmt(summary['refresh_period_ms'])} ms",
        f"repeated frames: {summary['repeated']} of {summary['refreshes']} "
        f"refreshes ({summary['repeated_percent']:.2f}%)",
        f"displayed latency: p50 {fmt(latency['p50'])} ms, "
        f"p99 {fmt(latency['p99'])} ms, max {fmt(latency['max'])} ms",
        f"between presents: p50 {fmt(presents['p50'])} ms, "
        f"p99 {fmt(presents['p99'])} ms",
        f"in Present: p50 {fmt(api['p50'])} ms, p99 {fmt(api['p99'])} ms",
    ])


def limits_exceeded(summary, args):
    problems = []
    p99 = summary["displayed_latency_ms"]["p99"]
    if args.max_p99_ms is not None and (p99 is None or p99 > args.max_p99_ms):
        problems.append(f"displayed latency p99 {fmt(p99)} ms > "
                        f"{args.max_p99_ms} ms")
    if args.max_dropped is not None and summary["dropped"] > args.max_dropped:
        problems.append(f"{summary['dropped']} dropped > {args.max_dropped}")
    if (args.max_repeated_percent is not None and
            summary["repeated_percent"] > args.max_repeated_percent):
        problems.append(f"{summary['repeated_percent']:.2f}% repeated > "
                        f"{args.max_repeated_percent}%")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", help="PresentMon CSV")
    parser.add_argument("--process", default="rampage.exe",
                        help="process name to keep ('' keeps every row)")
    parser.add_argument("--refresh-hz", type=float, default=None)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--max-p99-ms", type=float, default=None)
    parser.add_argument("--max-dropped", type=int, default=None)
    parser.add_argument("--max-repeated-percent", type=float, default=None)
    args = parser.parse_args(argv)
    process = args.process or None
    try:
        with open(args.capture, newline="", encoding="utf-8-sig") as stream:
            rows = read_rows(stream, process)
        summary = analyze(rows, args.refresh_hz)
    except (OSError, CaptureError) as error:
        print(f"analyze.py: {error}", file=sys.stderr)
        return 2
    if not rows:
        print(f"analyze.py: no rows for process {process!r}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print(describe(summary, process))
    problems = limits_exceeded(summary, args)
    for problem in problems:
        print("limit exceeded: " + problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
