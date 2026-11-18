"""Bans SDL_GPUTexture members outside se_render (docs/milestone-specs.md M7)
and ImTextureID members (M10a).

GpuResources::Resolve returns a texture that is valid for the current frame
only: SDL's D3D12 backend frees a texture's client container when it is
released, and a reload or resize releases it (red team C2). Code outside
se_render therefore keeps a Handle<Texture> and resolves it every frame; a
class, struct or union that stores an SDL_GPUTexture is a violation, in any
form (ADR-069):
  - a pointer or reference member, or an array of them;
  - a member of any type that names one: a container, a smart pointer
    (std::unique_ptr<SDL_GPUTexture, D>), any template argument;
  - a member of an SDL struct that holds one (SDL_GPUColorTargetInfo,
    SDL_GPUTextureSamplerBinding, SDL_GPUBlitInfo and the others below);
  - a member whose type is an alias (using, typedef) of any of these,
    aliases of aliases included, wherever the alias is declared.
An ImTextureID names the same per-frame texture (the SDL_GPU backend's id
is the SDL_GPUTexture*), so ImTextureID and ImTextureRef, which carries one,
are texture types too (M10a, ADR-095): the editor's panels ask TextureIds
for an id every frame and never keep it.
Member function declarations that take or return one are fine, and so are
function-pointer members, locals and parameters. A std::function member
whose signature returns one is reported too (keep a Handle).

The scan follows braces through each file with comments and strings
stripped. First every scanned file's aliases are collected (using X = ...;
typedef ... X;) whose type names a texture type or an earlier such alias;
then a declaration that ends with ';' directly inside a class, struct or
union body, and is not a member function, an alias, a friend or a
static_assert, is reported when it names a texture type or alias outside
parentheses. Every module but se_render is scanned, test and tool modules
included.

usage:
  check_gpu_members.py <build>/se_checks.json
  check_gpu_members.py --planted <dir>
      succeeds only if every source file under <dir> has a violation
  check_gpu_members.py --allowed <dir>
      succeeds only if no source file under <dir> has one
"""

import argparse
import json
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".h", ".cc", ".hpp", ".cpp", ".inl"}
ALLOWED_MODULES = {"se_render"}
# SDL_GPUTexture and the SDL structs that hold an SDL_GPUTexture*.
TEXTURE_TYPES = (
    "SDL_GPUTexture",
    "SDL_GPUTextureLocation",
    "SDL_GPUTextureRegion",
    "SDL_GPUBlitRegion",
    "SDL_GPUBlitInfo",
    "SDL_GPUColorTargetInfo",
    "SDL_GPUDepthStencilTargetInfo",
    "SDL_GPUTextureSamplerBinding",
    "SDL_GPUStorageTextureReadWriteBinding",
    # Dear ImGui's texture ids (M10a).
    "ImTextureID",
    "ImTextureRef",
)
ACCESS_SPECIFIERS = {"public", "private", "protected"}
CLASS_HEAD_RE = re.compile(
    r"^(?:template\s*<[^{]*>\s*)?(?:typedef\s+)?(?:class|struct|union)\b")
USING_RE = re.compile(
    r"^(?:template\s*<.*>\s*)?using\s+([A-Za-z_]\w*)\s*(?:\[\[.*?\]\]\s*)?=")
TYPEDEF_RE = re.compile(r"^typedef\b(.*?)\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$")
IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")
MACRO_RE = re.compile(r"[A-Z][A-Z0-9_]*")
# A '(' after one of these, or after an all-caps macro such as
# SE_GUARDED_BY, does not make a declaration a function.
NOT_FUNCTION_NAMES = {"decltype", "alignas", "alignof", "sizeof", "noexcept",
                      "__declspec", "__attribute__", "requires"}


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


def statements(text):
    """(line, scope kind, statement) for every ';'-terminated statement of
    |text|, the scope being the kind of the innermost enclosing brace;
    whitespace in the statement is collapsed."""
    code = strip(text)
    stack = ["file"]
    head = []
    head_start = 0
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
            yield head_start, stack[-1], " ".join("".join(head).split())
            head = []
            continue
        if not head:
            if ch.isspace():
                continue
            head_start = line
        head.append(ch)


def names_pattern(names):
    return re.compile(r"\b(?:" + "|".join(sorted(map(re.escape, names))) +
                      r")\b")


def declared_alias(statement):
    """(name, aliased text) of a using or typedef alias, else None.
    Function-pointer typedefs are not texture storage and are skipped."""
    match = USING_RE.match(statement)
    if match:
        return match.group(1), statement[match.end():]
    if statement.startswith("typedef ") and "(" not in statement:
        match = TYPEDEF_RE.match(statement)
        if match:
            return match.group(2), match.group(1)
    return None


def collect_aliases(texts):
    """Names of the aliases, in any of |texts|, of a texture type or of
    another such alias."""
    declared = []
    for text in texts:
        for _, _, statement in statements(text):
            alias = declared_alias(statement)
            if alias:
                declared.append(alias)
    found = set()
    while True:
        pattern = names_pattern(set(TEXTURE_TYPES) | found)
        added = {name for name, aliased in declared
                 if name not in found and
                 pattern.search(outside_parentheses(aliased))}
        if not added:
            return found
        found |= added


def is_function_declaration(statement):
    """A member function: an identifier (or an operator) followed by '(' at
    template-angle depth 0, before any '=' or '{' (a data member's
    initializer), and not a function pointer, a macro or a keyword such as
    SE_GUARDED_BY(...) or alignas(...)."""
    angle = 0
    i = 0
    n = len(statement)
    while i < n:
        ch = statement[i]
        if angle == 0 and statement.startswith("operator", i) and \
                (i == 0 or not statement[i - 1].isalnum()):
            return True
        if ch == "<":
            angle += 1
        elif ch == ">" and angle > 0:
            angle -= 1
        elif ch in "={" and angle == 0:
            return False  # An initializer: a data member.
        elif ch == "(" and angle == 0:
            if statement[i + 1:].lstrip()[:1] in ("*", "&", "^"):
                return False  # A pointer to a function: a data member.
            before = statement[:i].rstrip()
            words = IDENTIFIER_RE.findall(before)
            name = words[-1] if words else ""
            if name and before.endswith(name) and \
                    name not in NOT_FUNCTION_NAMES and \
                    not MACRO_RE.fullmatch(name):
                return True
            # A macro's or keyword's parentheses: skip them.
            depth = 0
            while i < n:
                if statement[i] == "(":
                    depth += 1
                elif statement[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
        i += 1
    return False


def member_violation(statement, pattern):
    if declared_alias(statement):
        return False
    if statement.split(" ", 1)[0] in ("friend", "static_assert"):
        return False
    if is_function_declaration(statement):
        return False
    return bool(pattern.search(outside_parentheses(statement)))


def scan_text(text, aliases):
    """Line numbers of texture members in |text|."""
    pattern = names_pattern(set(TEXTURE_TYPES) | aliases)
    return [line for line, scope, statement in statements(text)
            if scope == "class" and member_violation(statement, pattern)]


def scan_file(path, aliases):
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    return [f"{path}:{number}: a texture (SDL_GPUTexture or ImTextureID) "
            f"stored as a member outside se_render (keep a Handle<Texture> "
            f"and resolve it every frame): "
            f"{lines[number - 1].strip() if number <= len(lines) else ''}"
            for number in scan_text(text, aliases)]


def source_files(root):
    return [path for path in sorted(pathlib.Path(root).rglob("*"))
            if path.suffix in SOURCE_SUFFIXES]


def read_all(paths):
    return [path.read_text(encoding="utf-8", errors="replace")
            for path in paths]


def fixture_mode(root, expect_violation):
    files = source_files(root)
    aliases = collect_aliases(read_all(files))
    failed = []
    for path in files:
        found = scan_file(path, aliases)
        for line in found:
            print("planted (expected):" if expect_violation else "FOUND:",
                  line)
        if bool(found) != expect_violation:
            failed.append(path)
    for path in failed:
        if expect_violation:
            print(f"FAIL: {path}: the planted member was not detected")
        else:
            print(f"FAIL: {path}: an allowed declaration was reported")
    if not files:
        print("FAIL: no fixture files")
        return 1
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config", nargs="?")
    parser.add_argument("--planted")
    parser.add_argument("--allowed")
    args = parser.parse_args()

    if args.planted:
        return fixture_mode(args.planted, expect_violation=True)
    if args.allowed:
        return fixture_mode(args.allowed, expect_violation=False)

    config = json.loads(pathlib.Path(args.config).read_text(encoding="utf-8"))
    files = []
    seen = set()
    for module in config["modules"]:
        if module["name"] in ALLOWED_MODULES:
            continue
        for path in source_files(module["dir"]):
            if "planted" in path.parts or path in seen:
                continue
            if any(part == "se_render" for part in path.parts):
                continue
            seen.add(path)
            files.append(path)
    aliases = collect_aliases(read_all(files))
    violations = []
    for path in files:
        violations += scan_file(path, aliases)
    for line in violations:
        print(line)
    print(f"check_gpu_members: {len(files)} file(s), {len(aliases)} texture "
          f"alias(es), {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
