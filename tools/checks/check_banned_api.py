"""Scans engine sources for banned APIs and the exception policy.

Rules (docs/style.md and docs/decisions.md):
  every engine module
    - no `throw`: engine code returns errors as std::expected
    - `catch` only in files marked `// se-fault-boundary`
    - no RTTI: dynamic_cast, typeid
    - no <filesystem>, <ratio>, <cfenv>, <fenv.h> (Google C++ style)
  deterministic (simulation) modules, additionally
    - no wall clocks: steady_clock, system_clock, high_resolution_clock,
      SDL_GetTicks
    - no unseeded randomness: random_device, rand()
    - no CRT transcendentals (sin, cos, atan2, exp, pow, ...): use se::dm

A line may opt out of one rule with `// se-allow(<rule>): <reason>`.
Test modules (kind "test") are exempt.

usage:
  check_banned_api.py <build>/se_checks.json
  check_banned_api.py --planted <dir>   succeeds only if a violation is found
"""

import argparse
import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".cc", ".hpp", ".cpp", ".inl"}
TRANSCENDENTALS = ("sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|"
                   "expm1|log|log2|log10|log1p|pow|cbrt|hypot")

ENGINE_RULES = [
    ("throw", re.compile(r"\bthrow\b")),
    ("rtti", re.compile(r"\b(dynamic_cast|typeid)\b")),
    ("header", re.compile(r"#\s*include\s*<(filesystem|ratio|cfenv|fenv\.h)>")),
]
DETERMINISM_RULES = [
    ("clock", re.compile(
        r"\b(steady_clock|system_clock|high_resolution_clock|SDL_GetTicks\w*)\b")),
    ("random", re.compile(r"\brandom_device\b|(?<![\w.>:])(std::)?rand\s*\(")),
    ("math", re.compile(
        r"(?<![\w.>])(?:std::|::)?(?:" + TRANSCENDENTALS + r")[fl]?\s*\(")),
]
CATCH_RE = re.compile(r"\bcatch\s*\(")
FAULT_BOUNDARY = "se-fault-boundary"
ALLOW_RE = re.compile(r"se-allow\((\w+)\)")


def strip_code(line, in_block):
    """Removes comments and string/char literals from one line."""
    out = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i = end + 2
            in_block = False
            continue
        two = line[i:i + 2]
        if two == "//":
            break
        if two == "/*":
            in_block = True
            i += 2
            continue
        ch = line[i]
        if ch in "\"'":
            j = i + 1
            while j < len(line) and line[j] != ch:
                j += 2 if line[j] == "\\" else 1
            i = j + 1
            out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out), in_block


def scan_file(path, deterministic):
    found = []
    text = path.read_text(encoding="utf-8", errors="replace")
    fault_boundary = FAULT_BOUNDARY in text
    rules = ENGINE_RULES + (DETERMINISM_RULES if deterministic else [])
    in_block = False
    for number, raw in enumerate(text.splitlines(), start=1):
        allowed = set(ALLOW_RE.findall(raw))
        # Includes are matched on the raw line: stripping would drop <...>.
        code, in_block = strip_code(raw, in_block)
        for name, pattern in rules:
            subject = raw if name == "header" else code
            if name not in allowed and pattern.search(subject):
                found.append(f"{path}:{number}: banned ({name}): {raw.strip()}")
        if (CATCH_RE.search(code) and not fault_boundary
                and "catch" not in allowed):
            found.append(f"{path}:{number}: catch outside a file marked "
                         f"// {FAULT_BOUNDARY}: {raw.strip()}")
    return found


def scan(root, deterministic, include_planted=False):
    found = []
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        if "planted" in path.parts and not include_planted:
            continue
        found += scan_file(path, deterministic)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    args = parser.parse_args()

    if args.planted:
        found = scan(args.planted, deterministic=True, include_planted=True)
        for line in found:
            print("planted (expected):", line)
        if not found:
            print("FAIL: the planted violation was not detected")
            return 1
        return 0

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    violations = []
    for module in config["modules"]:
        if module.get("kind") == "test":
            continue
        violations += scan(module["dir"], module["deterministic"])
    for line in violations:
        print(line)
    print(f"check_banned_api: {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
