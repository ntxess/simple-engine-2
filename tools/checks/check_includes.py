"""Checks that each engine module includes only what its layer allows.

Reads se_checks.json (written by se_check_link_graph at configure time) and
scans every source file of each module for #include paths matching the
module's banned patterns.

usage:
  check_includes.py <build>/se_checks.json
  check_includes.py --planted <dir>   succeeds only if a violation is found
"""

import argparse
import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".cc", ".hpp", ".cpp", ".inl"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
# Mirrors SE_BAN_PLATFORM_INCLUDES in cmake/SeModule.cmake.
PLATFORM_BANS = [
    r"^SDL3?/", r"^imgui", r"^backends/imgui", r"^windows\.h$",
    r"^Windows\.h$", r"^miniaudio", r"^sentry", r"^crashpad", r"^lz4",
    r"^SFML/", r"^boost/",
]


def source_files(root):
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix in SOURCE_SUFFIXES and "planted" not in path.parts:
            yield path


def scan(root, bans, include_planted=False):
    patterns = [re.compile(b) for b in bans]
    found = []
    files = (sorted(p for p in pathlib.Path(root).rglob("*")
                    if p.suffix in SOURCE_SUFFIXES)
             if include_planted else source_files(root))
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in INCLUDE_RE.finditer(text):
            target = match.group(1)
            for pattern in patterns:
                if pattern.search(target):
                    line = text.count("\n", 0, match.start()) + 1
                    found.append(f"{path}:{line}: banned include <{target}> "
                                 f"(pattern {pattern.pattern})")
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    args = parser.parse_args()

    if args.planted:
        found = scan(args.planted, PLATFORM_BANS, include_planted=True)
        for line in found:
            print("planted (expected):", line)
        if not found:
            print("FAIL: the planted violation was not detected")
            return 1
        return 0

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    violations = []
    for module in config["modules"]:
        if module["banned_includes"]:
            violations += scan(module["dir"], module["banned_includes"])
    for line in violations:
        print(line)
    print(f"check_includes: {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
