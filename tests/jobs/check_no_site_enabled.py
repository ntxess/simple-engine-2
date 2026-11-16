#!/usr/bin/env python3
# Copyright 2026 The simple-engine-2 Authors.
"""M14 exit: no fork site is enabled in any shipped configuration.

Scans the shipped trees (engine/, games/, apps/ and config/ of the source
directory given as the first argument, or the directories given after it)
for anything that enables a ForkJoinPool site:

  - `enabled_sites` (ForkJoinPool::Config's list) anywhere outside the
    pool's own header and source;
  - a call of `EnableSite(`;
  - `enabled = true` in a C++ file that names `ForkSite`.

Tests, benches and tools enable sites explicitly and are not scanned. The
enabling rules are M16's (docs/design/mt-design.md "Fork sites and
enabling"); when a site qualifies, its ADR changes this check.

Exit status: 0 when nothing enables a site, 1 with one line per finding.
"""

import os
import re
import sys

SHIPPED_DIRS = ("engine", "games", "apps", "config")
SOURCE_SUFFIXES = (".h", ".cc", ".cpp", ".hpp", ".inl", ".json", ".toml",
                   ".ini", ".cfg")
# The pool declares and reads the list; it never fills it.
POOL_FILES = (
    os.path.join("engine", "se_core", "include", "se_core", "jobs.h"),
    os.path.join("engine", "se_core", "src", "jobs.cc"),
)
ENABLED_SITES_RE = re.compile(r"\benabled_sites\b")
ENABLE_CALL_RE = re.compile(r"\bEnableSite\s*\(")
ENABLED_TRUE_RE = re.compile(r"\benabled\s*=\s*true\b")
LINE_COMMENT_RE = re.compile(r"//.*$")


def scan_file(path, relative):
    findings = []
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        return [f"{relative}: unreadable ({error})"]
    is_pool = relative in POOL_FILES
    names_fork_site = "ForkSite" in text
    for number, raw in enumerate(text.splitlines(), start=1):
        line = LINE_COMMENT_RE.sub("", raw)
        if not is_pool and ENABLED_SITES_RE.search(line):
            findings.append(f"{relative}:{number}: enabled_sites")
        if not is_pool and ENABLE_CALL_RE.search(line):
            findings.append(f"{relative}:{number}: EnableSite()")
        if not is_pool and names_fork_site and ENABLED_TRUE_RE.search(line):
            findings.append(f"{relative}:{number}: a ForkSite enabled")
    return findings


def scan_tree(root, base):
    findings = []
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(directory, name)
            findings += scan_file(path, os.path.relpath(path, base))
    return findings


def main(argv):
    if len(argv) < 2:
        print("usage: check_no_site_enabled.py SOURCE_DIR [DIR...]",
              file=sys.stderr)
        return 2
    source_dir = argv[1]
    roots = argv[2:] or [os.path.join(source_dir, d) for d in SHIPPED_DIRS]
    findings = []
    scanned = 0
    for root in roots:
        if not os.path.isdir(root):
            continue
        scanned += 1
        findings += scan_tree(root, source_dir)
    if scanned == 0:
        print("check_no_site_enabled: nothing to scan", file=sys.stderr)
        return 2
    for finding in findings:
        print(f"fork site enabled in shipped code: {finding}")
    if findings:
        print("No fork site may be enabled in a shipped configuration "
              "(docs/milestone-specs.md M14; M16 decides).")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
