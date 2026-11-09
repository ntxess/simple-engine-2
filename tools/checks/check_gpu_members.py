"""Bans SDL_GPUTexture* members outside se_render (docs/milestone-specs.md M7).

GpuResources::Resolve returns a texture that is valid for the current frame
only: SDL's D3D12 backend frees a texture's client container when it is
released, and a reload or resize releases it (red team C2). Code outside
se_render therefore keeps a Handle<Texture> and resolves it every frame; a
class or struct that stores an SDL_GPUTexture* (a pointer member, or a
container or wrapper of them) is a violation. Function declarations that
take or return one are fine, and so are locals.

The scan follows braces through each file with comments and strings
stripped: a declaration that ends with ';' directly inside a class, struct
or union body and names SDL_GPUTexture* outside parentheses is reported.
Every module but se_render is scanned, test and tool modules included.

usage:
  check_gpu_members.py <build>/se_checks.json
  check_gpu_members.py --planted <dir>
      succeeds only if every source file under <dir> has a violation
"""

import argparse
import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".cc", ".hpp", ".cpp", ".inl"}
ALLOWED_MODULES = {"se_render"}
TEXTURE_RE = re.compile(r"\bSDL_GPUTexture\s*\*")
ACCESS_SPECIFIERS = {"public", "private", "protected"}
CLASS_HEAD_RE = re.compile(
    r"^(?:template\s*<[^{]*>\s*)?(?:typedef\s+)?(?:class|struct|union)\b")


def strip(text):
    """The file without comments and string or character literals; newlines
    are kept so that offsets map to lines."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(ch for ch in text[i:end] if ch == "\n"))
            i = end
            continue
        ch = text[i]
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(" ")
            i = j + 1
            continue
        if ch == "#":
            # Preprocessor lines (includes, macros) are not declarations.
            while i < n and text[i] != "\n":
                if text[i] == "\\" and text[i + 1:i + 2] == "\n":
                    out.append("\n")
                    i += 2
                    continue
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def classify(head):
    """The kind of scope a '{' after |head| opens."""
    h = " ".join(head.split())
    if h.startswith("enum"):
        return "enum"
    if h.startswith("namespace") or h.startswith("extern"):
        return "namespace"
    if CLASS_HEAD_RE.match(h) and "=" not in h:
        return "class"
    if ")" in h or h.endswith("]") or h.endswith("else") or \
            h.endswith("do") or h.endswith("try") or not h:
        return "code"
    return "init"


def outside_parentheses(statement):
    depth = 0
    out = []
    for ch in statement:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def scan_text(text):
    """Line numbers of SDL_GPUTexture* members in |text|."""
    code = strip(text)
    stack = ["file"]
    head = []
    head_start = 0
    found = []
    line = 1
    for ch in code:
        if ch == "\n":
            line += 1
        if stack[-1] == "init":
            # A brace initializer of the statement being read.
            head.append(ch)
            if ch == "{":
                stack.append("init")
            elif ch == "}":
                stack.pop()
            continue
        if ch == "{":
            kind = classify("".join(head))
            if kind == "init":
                head.append(ch)
                stack.append("init")
                continue
            stack.append(kind)
            head = []
            continue
        if ch == "}":
            if len(stack) > 1:
                stack.pop()
            head = []
            continue
        if ch == ":" and "".join(head).strip() in ACCESS_SPECIFIERS:
            head = []
            continue
        if ch == ";":
            statement = "".join(head)
            if stack[-1] == "class" and \
                    TEXTURE_RE.search(outside_parentheses(statement)):
                found.append(head_start)
            head = []
            continue
        if not head:
            if ch.isspace():
                continue
            head_start = line
        head.append(ch)
    return found


def scan_file(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    return [f"{path}:{number}: SDL_GPUTexture* stored as a member outside "
            f"se_render (keep a Handle<Texture> and resolve it every frame): "
            f"{lines[number - 1].strip() if number <= len(lines) else ''}"
            for number in scan_text(text)]


def source_files(root):
    return [path for path in sorted(pathlib.Path(root).rglob("*"))
            if path.suffix in SOURCE_SUFFIXES]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    args = parser.parse_args()

    if args.planted:
        files = source_files(args.planted)
        missed = []
        for path in files:
            found = scan_file(path)
            for line in found:
                print("planted (expected):", line)
            if not found:
                missed.append(path)
        for path in missed:
            print(f"FAIL: {path}: the planted member was not detected")
        if not files:
            print("FAIL: no planted files")
            return 1
        return 1 if missed else 0

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    violations = []
    scanned = set()
    for module in config["modules"]:
        if module["name"] in ALLOWED_MODULES:
            continue
        for path in source_files(module["dir"]):
            if "planted" in path.parts or path in scanned:
                continue
            if any(part == "se_render" for part in path.parts):
                continue
            scanned.add(path)
            violations += scan_file(path)
    for line in violations:
        print(line)
    print(f"check_gpu_members: {len(scanned)} file(s), {len(violations)} "
          f"violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
