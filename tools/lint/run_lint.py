"""Runs one linter over the engine's new C++ code (docs/style.md).

The legacy tree (core/, app/) and vendored code (dep/) are never linted.
Planted fixtures (any directory named "planted") are skipped unless
--planted is given, in which case the linter runs only on its fixture
directory and succeeds only if it reports a violation.

usage:
  run_lint.py clang-format [--tool PATH] [--fix] [--planted]
  run_lint.py cpplint [--planted]
  run_lint.py clang-tidy --build-dir DIR [--tool PATH] [--planted] [-j N]
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
LINT_ROOTS = ["engine", "games", "apps", "tests", "bench", "tools"]
SOURCE_SUFFIXES = {".h", ".cc"}
PLANTED = REPO / "tools" / "lint" / "planted"


def lint_files():
    files = []
    for root in LINT_ROOTS:
        base = REPO / root
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix in SOURCE_SUFFIXES and "planted" not in path.parts:
                files.append(path)
    return files


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


# Windows' CreateProcess takes at most 32767 characters of command line:
# the file list is passed in batches below that (every file is still
# checked; the first failing batch's code is the result).
MAX_COMMAND_CHARS = 24000


def batches(files):
    batch, size = [], 0
    for path in map(str, files):
        if batch and size + len(path) + 3 > MAX_COMMAND_CHARS:
            yield batch
            batch, size = [], 0
        batch.append(path)
        size += len(path) + 3
    if batch:
        yield batch


def run_batched(prefix, files, **kwargs):
    code, outputs = 0, []
    for batch in batches(files):
        result = run([*prefix, *batch], **kwargs)
        if result.returncode != 0 and code == 0:
            code = result.returncode
        output = (result.stdout + result.stderr).strip()
        if output:
            outputs.append(output)
    return code, "\n".join(outputs)


def clang_format(tool, files, fix):
    if fix:
        return run_batched([tool, "-i", "--style=file"], files)
    return run_batched([tool, "--dry-run", "--Werror", "--style=file"], files)


def cpplint(files):
    return run_batched([sys.executable, "-m", "cpplint", "--quiet"], files,
                       cwd=REPO)


def clang_tidy_one(tool, build_dir, path):
    result = run([tool, "-p", str(build_dir), "--quiet", str(path)])
    text = (result.stdout + result.stderr).strip()
    # Drop the "N warnings generated." summaries; keep diagnostics.
    lines = [line for line in text.splitlines()
             if "warnings generated" not in line
             and "warning generated" not in line
             and not line.startswith("Suppressed ")
             and not line.startswith("Use -header-filter")]
    return result.returncode, "\n".join(lines)


def clang_tidy(tool, build_dir, files, jobs):
    database = json.loads((pathlib.Path(build_dir) / "compile_commands.json")
                          .read_text(encoding="utf-8"))
    compiled = {os.path.normcase(os.path.realpath(entry["file"]))
                for entry in database}
    targets = [f for f in files if f.suffix == ".cc"
               and os.path.normcase(os.path.realpath(f)) in compiled]
    if not targets:
        return 0, "clang-tidy: no compiled sources to check"
    worst = 0
    outputs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(clang_tidy_one, tool, build_dir, f): f
                   for f in targets}
        for future in concurrent.futures.as_completed(futures):
            code, text = future.result()
            worst = max(worst, code)
            if code != 0 or text:
                outputs.append(f"--- {futures[future]}\n{text}")
    outputs.append(f"clang-tidy: checked {len(targets)} file(s)")
    return worst, "\n".join(outputs)


def clang_tidy_planted(tool):
    files = sorted((PLANTED / "clang-tidy").glob("*.cc"))
    worst = 0
    outputs = []
    for path in files:
        result = run([tool, f"--config-file={REPO / '.clang-tidy'}", "--quiet",
                      str(path), "--", "-std=c++20", "-xc++"])
        worst = max(worst, result.returncode)
        outputs.append((result.stdout + result.stderr).strip())
    return worst, "\n".join(outputs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("linter", choices=["clang-format", "cpplint",
                                           "clang-tidy"])
    parser.add_argument("--tool")
    parser.add_argument("--build-dir")
    parser.add_argument("--planted", action="store_true")
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    tool = args.tool or args.linter
    if args.planted:
        if args.linter == "clang-tidy":
            code, output = clang_tidy_planted(tool)
        else:
            planted = sorted((PLANTED / args.linter).glob("*"))
            if args.linter == "clang-format":
                code, output = clang_format(tool, planted, fix=False)
            else:
                code, output = cpplint(planted)
        print(output)
        if code == 0:
            print(f"FAIL: {args.linter} did not flag the planted violation")
            return 1
        print(f"{args.linter}: planted violation detected (expected)")
        return 0

    files = lint_files()
    if args.linter == "clang-format":
        code, output = clang_format(tool, files, args.fix)
    elif args.linter == "cpplint":
        code, output = cpplint(files)
    else:
        if not args.build_dir:
            parser.error("clang-tidy needs --build-dir")
        code, output = clang_tidy(tool, args.build_dir, files, args.jobs)
    if output:
        print(output)
    print(f"{args.linter}: {len(files)} file(s), exit {code}")
    return 1 if code else 0


if __name__ == "__main__":
    sys.exit(main())
