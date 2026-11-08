"""Checks that glaze stays confined to the component-registry units.

glaze costs seconds per translation unit to compile (ADR-010), so engine
code reaches it only through se_core/reflect_glaze.h, and only translation
units named *_components.cc include that header:

  - no #include of a glaze header except in reflect_glaze.h;
  - no #include of se_core/reflect_glaze.h except in *_components.cc.

Test and tool modules (kind "test") are exempt, like the banned-API scan.

usage:
  check_glaze.py <build>/se_checks.json
  check_glaze.py --planted <dir>   succeeds only if a violation is found
"""

import argparse
import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".cc", ".hpp", ".cpp", ".inl"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
REFLECT_HEADER = "se_core/reflect_glaze.h"


def scan_file(path):
    found = []
    text = path.read_text(encoding="utf-8", errors="replace")
    for match in INCLUDE_RE.finditer(text):
        target = match.group(1)
        line = text.count("\n", 0, match.start()) + 1
        if target.startswith("glaze/") and path.name != "reflect_glaze.h":
            found.append(f"{path}:{line}: glaze included outside "
                         f"{REFLECT_HEADER}: <{target}>")
        if target == REFLECT_HEADER and not path.name.endswith(
                "_components.cc"):
            found.append(f"{path}:{line}: {REFLECT_HEADER} included by a "
                         f"file not named *_components.cc")
    return found


def scan(root, include_planted=False):
    found = []
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        if "planted" in path.parts and not include_planted:
            continue
        found += scan_file(path)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    args = parser.parse_args()

    if args.planted:
        found = scan(args.planted, include_planted=True)
        for line in found:
            print("planted (expected):", line)
        if len(found) < 2:
            print("FAIL: the planted violations were not both detected")
            return 1
        return 0

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    violations = []
    for module in config["modules"]:
        if module.get("kind") == "test":
            continue
        violations += scan(module["dir"])
    for line in violations:
        print(line)
    print(f"check_glaze: {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
