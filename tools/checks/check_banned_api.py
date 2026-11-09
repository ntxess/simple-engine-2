"""Scans engine sources for banned APIs and the exception policy.

Rules (docs/style.md and docs/decisions.md):
  every module, tests included
    - no timeBeginPeriod/timeEndPeriod: the timer resolution is never
      toggled (docs/design/mt-design.md "Platform")
  every engine module
    - no `throw`: engine code returns errors as std::expected
    - `catch` only in files marked `// se-fault-boundary`
    - no RTTI: dynamic_cast, typeid
    - no <filesystem>, <ratio>, <cfenv>, <fenv.h> (Google C++ style)
    - no DebugBreak, __debugbreak, __builtin_trap, __builtin_debugtrap or
      std::breakpoint: a fault aborts and the crash handler writes the dump
    - no in-process dump writing: MiniDumpWriteDump, <dbghelp.h> (crashpad
      writes dumps out of process)
    - no raw thread creation (std::thread, std::jthread, std::async,
      CreateThread, CreateRemoteThread, _beginthreadex, pthread_create, the
      Win32 thread pool, the standard parallel execution policies) outside
      se_os: engine threads come from se::StartThread
  deterministic (simulation) modules, additionally
    - no wall clocks: steady_clock, system_clock, high_resolution_clock,
      SDL_GetTicks
    - no unseeded randomness: random_device, rand()
    - no CRT transcendentals (sin, cos, atan2, exp, pow, ...): use se::dm
    - no mutable static variables (a `static` declaration that is not const,
      constexpr or constinit): state shared by every World and thread, and
      a non-trivial one is also destroyed at exit (Google C++ style); a
      never-destroyed singleton is `static T* const k = new T`

A line may opt out of one rule with `// se-allow(<rule>): <reason>`, except
for the rules the M4 spec states without exceptions (NO_OPT_OUT: thread,
debugbreak, dump, dump_header, timer). Test modules (kind "test") are
exempt from everything but the timer rule.

usage:
  check_banned_api.py <build>/se_checks.json
  check_banned_api.py --planted <dir> [--expect RULE]
      succeeds only if a violation is found; with RULE, only if every
      source file under <dir> violates RULE (one planted case per file)
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
    ("debugbreak", re.compile(
        r"\b(DebugBreak|__debugbreak|__builtin_trap|__builtin_debugtrap)\b"
        r"|\bstd::breakpoint\w*")),
    ("dump", re.compile(r"\bMiniDumpWriteDump\b")),
    ("dump_header", re.compile(r"#\s*include\s*[<\"](?i:dbghelp\.h)[>\"]")),
]
# Rules for every module, test code included.
ALL_RULES = [
    ("timer", re.compile(r"\btime(Begin|End)Period\b")),
]
# Raw thread creation; std::thread::id and std::this_thread are fine.
# std::async (any launch policy may start a thread), the Win32 thread pool
# and the parallel execution policies start threads too.
THREAD_RULE = ("thread", re.compile(
    r"\bstd::j?thread\b(?!\s*::)"
    r"|\bstd::async\b"
    r"|\bstd::execution::(par|par_unseq)\b"
    r"|\b(CreateThread|CreateRemoteThread\w*|_beginthreadex|_beginthread"
    r"|pthread_create|QueueUserWorkItem|CreateThreadpool\w*"
    r"|TrySubmitThreadpoolCallback)\s*\("))
# The only module that may create threads (ADR-029).
THREAD_MODULES = {"se_os"}
# Rules matched on the raw line: stripping would drop the <...> of an include.
RAW_RULES = {"header", "dump_header"}
# Rules a `// se-allow(<rule>)` comment cannot silence: the spec bans these
# without exceptions (the thread rule's only exception is the se_os module).
NO_OPT_OUT = {"thread", "debugbreak", "dump", "dump_header", "timer"}
DETERMINISM_RULES = [
    ("clock", re.compile(
        r"\b(steady_clock|system_clock|high_resolution_clock|SDL_GetTicks\w*)\b")),
    ("random", re.compile(r"\brandom_device\b|(?<![\w.>:])(std::)?rand\s*\(")),
    ("math", re.compile(
        r"(?<![\w.>])(?:std::|::)?(?:" + TRANSCENDENTALS + r")[fl]?\s*\(")),
    # `static <type> <name>` followed by = ; { or [ (a variable, not a
    # function), with no const, constexpr or constinit before the name.
    ("static", re.compile(
        r"(?:^|[;{}])\s*static\s+(?![^=;{(]*\bconst(?:expr|init)?\b)"
        r"[\w:<>,\s*&]*?[\w>*&]\s*\b[A-Za-z_]\w*\s*(?:=|;|\{|\[)")),
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


def rules_for(kind, deterministic, name):
    """The rules for a module of |kind| ("engine" or "test") named |name|."""
    if kind == "test":
        return list(ALL_RULES)
    rules = ALL_RULES + ENGINE_RULES
    if name not in THREAD_MODULES:
        rules.append(THREAD_RULE)
    if deterministic:
        rules += DETERMINISM_RULES
    return rules


def scan_file(path, rules, check_catch=True):
    found = []
    text = path.read_text(encoding="utf-8", errors="replace")
    fault_boundary = FAULT_BOUNDARY in text
    in_block = False
    for number, raw in enumerate(text.splitlines(), start=1):
        allowed = set(ALLOW_RE.findall(raw))
        # Includes are matched on the raw line: stripping would drop <...>.
        code, in_block = strip_code(raw, in_block)
        for name, pattern in rules:
            subject = raw if name in RAW_RULES else code
            opted_out = name in allowed and name not in NO_OPT_OUT
            if not opted_out and pattern.search(subject):
                found.append(f"{path}:{number}: banned ({name}): {raw.strip()}")
        if (check_catch and CATCH_RE.search(code) and not fault_boundary
                and "catch" not in allowed):
            found.append(f"{path}:{number}: catch outside a file marked "
                         f"// {FAULT_BOUNDARY}: {raw.strip()}")
    return found


def planted_files(root):
    return [path for path in sorted(pathlib.Path(root).rglob("*"))
            if path.suffix in SOURCE_SUFFIXES]


def scan(root, rules, check_catch=True, include_planted=False):
    found = []
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        if "planted" in path.parts and not include_planted:
            continue
        found += scan_file(path, rules, check_catch)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    parser.add_argument("--expect",
                        help="with --planted: the rule that must fire")
    args = parser.parse_args()

    if args.planted:
        # A planted fixture is an engine module other than se_os, with the
        # determinism rules on, so every rule applies to it.
        found = scan(args.planted, rules_for("engine", True, "planted"),
                     include_planted=True)
        missed = []
        if args.expect:
            found = [line for line in found
                     if f"banned ({args.expect})" in line]
            # Each file is one planted case of the rule: every one must fire.
            missed = [path for path in planted_files(args.planted)
                      if not any(line.startswith(f"{path}:")
                                 for line in found)]
        for line in found:
            print("planted (expected):", line)
        for path in missed:
            print(f"FAIL: {path}: the planted {args.expect} violation was "
                  f"not detected")
        if not found:
            print("FAIL: the planted violation was not detected")
            return 1
        return 1 if missed else 0

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    violations = []
    for module in config["modules"]:
        kind = module.get("kind", "engine")
        rules = rules_for(kind, module["deterministic"], module["name"])
        violations += scan(module["dir"], rules, check_catch=kind != "test")
    for line in violations:
        print(line)
    print(f"check_banned_api: {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
