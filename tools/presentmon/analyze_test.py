"""Tests analyze.py on synthetic PresentMon captures built here.

A 60 Hz capture of rampage.exe with known latencies, two dropped presents
and one present that stayed up for three refreshes (two repeated), mixed
with another process's rows that must be ignored; limits that pass and
fail; a capture without the v1 columns. CTest runs this as
presentmon.analyze (label tools).

usage:
  analyze_test.py
"""

import contextlib
import csv
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import analyze  # noqa: E402  (imported from this directory)

COLUMNS = ["Application", "ProcessID", "SwapChainAddress", "Runtime",
           "SyncInterval", "PresentFlags", "Dropped", "TimeInSeconds",
           "MsInPresentAPI", "MsBetweenPresents", "MsUntilRenderComplete",
           "MsUntilDisplayed", "MsBetweenDisplayChange"]
PERIOD = 1000.0 / 60.0


def capture_rows():
    """100 rampage presents and 20 of another process."""
    rows = []
    for i in range(100):
        dropped = i in (40, 41)
        # Latencies 10.0, 10.1, ... 19.9 ms.
        latency = 10.0 + 0.1 * i
        change = PERIOD * 3 if i == 70 else PERIOD
        rows.append({
            "Application": "rampage.exe", "ProcessID": "4242",
            "SwapChainAddress": "0x1", "Runtime": "DXGI", "SyncInterval": "1",
            "PresentFlags": "0", "Dropped": "1" if dropped else "0",
            "TimeInSeconds": f"{i * PERIOD / 1000.0:.6f}",
            "MsInPresentAPI": "0.20", "MsBetweenPresents": f"{PERIOD:.4f}",
            "MsUntilRenderComplete": "1.0",
            "MsUntilDisplayed": "NA" if dropped else f"{latency:.4f}",
            "MsBetweenDisplayChange": "NA" if dropped else f"{change:.4f}",
        })
    for i in range(20):
        rows.append({
            "Application": "dwm.exe", "ProcessID": "1", "SwapChainAddress": "0x2",
            "Runtime": "DXGI", "SyncInterval": "0", "PresentFlags": "0",
            "Dropped": "1", "TimeInSeconds": f"{i / 100.0:.6f}",
            "MsInPresentAPI": "9.0", "MsBetweenPresents": "50.0",
            "MsUntilRenderComplete": "1.0", "MsUntilDisplayed": "99.0",
            "MsBetweenDisplayChange": "99.0",
        })
    return rows


def write_csv(path, rows, columns=COLUMNS):
    with open(path, "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns,
                                extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = analyze.main(argv)
    return status, out.getvalue(), err.getvalue()


class AnalyzeTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = str(pathlib.Path(self.dir.name) / "capture.csv")
        write_csv(self.path, capture_rows())

    def tearDown(self):
        self.dir.cleanup()

    def test_summary_of_one_process(self):
        status, out, _ = run([self.path, "--json"])
        self.assertEqual(status, 0)
        summary = json.loads(out)
        self.assertEqual(summary["presents"], 100)
        self.assertEqual(summary["dropped"], 2)
        self.assertEqual(summary["displayed"], 98)
        self.assertAlmostEqual(summary["refresh_period_ms"], PERIOD, places=3)
        # The present at i = 70 stayed up for 3 refreshes: 2 repeated.
        self.assertEqual(summary["repeated"], 2)
        self.assertEqual(summary["refreshes"], 100)
        self.assertAlmostEqual(summary["repeated_percent"], 2.0)
        latency = summary["displayed_latency_ms"]
        # 98 latencies 10.0 .. 19.9 without 14.0 and 14.1: nearest rank 49
        # is 15.0 (40 values below 14.0, then 14.2, 14.3, ...), rank 98 is
        # 19.9.
        self.assertAlmostEqual(latency["p50"], 15.0, places=3)
        self.assertAlmostEqual(latency["p99"], 19.9, places=3)
        self.assertAlmostEqual(latency["max"], 19.9, places=3)
        self.assertAlmostEqual(summary["in_present_api_ms"]["p99"], 0.2)

    def test_text_report(self):
        status, out, _ = run([self.path])
        self.assertEqual(status, 0)
        self.assertIn("presents: 100 (98 displayed, 2 dropped)", out)
        self.assertIn("repeated frames: 2 of 100 refreshes (2.00%)", out)
        self.assertIn("displayed latency: p50 15.000 ms, p99 19.900 ms", out)

    def test_every_process(self):
        status, out, _ = run([self.path, "--process", "", "--json"])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(out)["presents"], 120)

    def test_refresh_rate_given(self):
        status, out, _ = run([self.path, "--refresh-hz", "120", "--json"])
        self.assertEqual(status, 0)
        summary = json.loads(out)
        # At 120 Hz every 16.7 ms change spans two refreshes.
        self.assertEqual(summary["refreshes"], 98 * 2 + 4)
        self.assertEqual(summary["repeated"], 97 + 5)

    def test_limits(self):
        status, _, _ = run([self.path, "--max-p99-ms", "20",
                            "--max-dropped", "2",
                            "--max-repeated-percent", "2.5"])
        self.assertEqual(status, 0)
        status, _, err = run([self.path, "--max-p99-ms", "19"])
        self.assertEqual(status, 1)
        self.assertIn("displayed latency p99 19.900 ms > 19.0 ms", err)
        status, _, err = run([self.path, "--max-dropped", "1"])
        self.assertEqual(status, 1)
        self.assertIn("2 dropped > 1", err)
        status, _, err = run([self.path, "--max-repeated-percent", "1"])
        self.assertEqual(status, 1)

    def test_missing_columns(self):
        write_csv(self.path, capture_rows(),
                  columns=["Application", "MsBetweenPresents"])
        status, _, err = run([self.path])
        self.assertEqual(status, 2)
        self.assertIn("missing columns", err)
        self.assertIn("--v1_metrics", err)

    def test_no_rows_for_the_process(self):
        status, _, err = run([self.path, "--process", "other.exe"])
        self.assertEqual(status, 2)
        self.assertIn("no rows", err)

    def test_bad_number(self):
        rows = capture_rows()
        rows[3]["MsUntilDisplayed"] = "fast"
        write_csv(self.path, rows)
        status, _, err = run([self.path])
        self.assertEqual(status, 2)
        self.assertIn("line 5: MsUntilDisplayed is not a number", err)

    def test_percentile(self):
        self.assertIsNone(analyze.percentile([], 50))
        self.assertEqual(analyze.percentile([3, 1, 2], 0), 1)
        self.assertEqual(analyze.percentile([3, 1, 2], 50), 2)
        self.assertEqual(analyze.percentile([3, 1, 2], 100), 3)


if __name__ == "__main__":
    unittest.main()
