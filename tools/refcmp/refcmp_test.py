"""Tests refcmp.py against planted projections (tools/refcmp/planted).

Each planted file differs from planted/expected.seproj in one way, just
inside or just outside a tolerance, or in one entity, tick or stat text, and
refcmp must exit with the status listed in CASES and print its line. Every
planted file needs a case. CTest runs this as refcmp.planted.

usage:
  refcmp_test.py
"""

import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
PLANTED = HERE / "planted"
EXPECTED = "expected.seproj"

# (actual file, exit status, a regular expression refcmp's output matches)
CASES = [
    (EXPECTED, 0, r"^refcmp: equal within the tolerances$"),
    # Within the tolerances (|d| <= 1e-3 px or degrees).
    ("dx_inside.seproj", 0, r"^  max \|dx\| px: 0\.0009 \(tick 1, entity 0\)$"),
    ("dx_at_tolerance.seproj", 0,
     r"^  max \|dx\| px: 0\.001 \(tick 1, entity 3\)$"),
    ("rot_wrap_inside.seproj", 0,
     r"^  max \|drot\| deg: 0\.0009 \(tick 0, entity 1\)$"),
    ("rot_turn_inside.seproj", 0, r"^refcmp: equal within the tolerances$"),
    # Just outside them.
    ("dx_outside.seproj", 1,
     r"^FIRST DIFFERENCE: tick 1, entity 0: \|dx\| = 0\.0011$"),
    ("dy_outside.seproj", 1,
     r"^FIRST DIFFERENCE: tick 1, entity 2: \|dy\| = 0\.0011$"),
    ("rot_wrap_outside.seproj", 1,
     r"^FIRST DIFFERENCE: tick 0, entity 1: \|drotation\| = 0\.0011 deg$"),
    # Stats compare as text: equal numbers written differently differ.
    ("hp_text.seproj", 1,
     r"^FIRST DIFFERENCE: tick 1, entity 1: stats differ: HP 64\.0 vs 64$"),
    ("stat_text.seproj", 1,
     r"^FIRST DIFFERENCE: tick 0, entity 0: stats differ: "
     r"Charge 1e\+01 vs 10$"),
    ("hp_absent.seproj", 1,
     r"^FIRST DIFFERENCE: tick 0, entity 2: stats differ: HP 0 vs -$"),
    ("stat_absent.seproj", 1,
     r"^FIRST DIFFERENCE: tick 1, entity 2: stats differ: "
     r"Speed absent vs 500$"),
    # Liveness and ticks.
    ("entity_missing.seproj", 1,
     r"^FIRST DIFFERENCE: tick 1: live entities differ: "
     r"expected but missing \[2\], unexpected \[\]$"),
    ("entity_extra.seproj", 1,
     r"^FIRST DIFFERENCE: tick 0: live entities differ: "
     r"expected but missing \[\], unexpected \[4\]$"),
    ("tick_missing.seproj", 1,
     r"^FIRST DIFFERENCE: tick sets differ: missing \[1\], extra \[\]$"),
    ("empty.seproj", 1,
     r"^FIRST DIFFERENCE: tick sets differ: missing \[0, 1\], extra \[\]$"),
    # Bad input.
    ("malformed.seproj", 2, r"malformed\.seproj:8: malformed entity line$"),
]


def run(actual):
    result = subprocess.run(
        [sys.executable, "-I", str(HERE / "refcmp.py"), str(PLANTED / actual),
         str(PLANTED / EXPECTED)],
        capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


def main():
    failures = []
    for actual, status, pattern in CASES:
        code, output = run(actual)
        found = re.search(pattern, output, re.MULTILINE) is not None
        if code == status and found:
            print(f"ok   {actual}: exit {code}")
            continue
        failures.append(actual)
        print(f"FAIL {actual}: exit {code} (expected {status}); "
              f"{'found' if found else 'missing'}: {pattern}")
        print(output)
    planted = {path.name for path in PLANTED.glob("*.seproj")}
    uncovered = sorted(planted - {actual for actual, _, _ in CASES})
    if uncovered:
        failures += uncovered
        print(f"FAIL planted files without a case: {uncovered}")
    if failures:
        print(f"refcmp_test: {len(failures)} failure(s)")
        return 1
    print(f"refcmp_test: {len(CASES)} cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
