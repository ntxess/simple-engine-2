#!/usr/bin/env python3
"""
Cross-platform helper for configuring/building/cleaning this CMake project.

Examples:
  # Configure + build Debug (default)
  python scripts/project.py

  # Configure + build "Release" (defaults to RelWithDebInfo)
  python scripts/project.py --config Release

  # Rebuild Debug
  python scripts/project.py --action rebuild --config Debug

  # Clean the build directory for a config
  python scripts/project.py --action clean --config Debug

  # Delete the build directory for a config
  python scripts/project.py --action clobber --config Debug

  # Delete all build directories created by this script
  python scripts/project.py --action clobber-all

Notes:
  - This script intentionally writes a stable compile database to: build/compile_commands.json
    so editor IntelliSense can always point at a single path.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


def _slug(value: str) -> str:
    s = value.lower()
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    return s or "default"


def _run(cmd: list[str], cwd: Optional[Path] = None) -> None:
    printable = " ".join(cmd)
    print(f"+ {printable}")
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None)


def _read_cmake_cache_value(build_dir: Path, key: str) -> Optional[str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None

    # Lines look like: KEY:TYPE=value
    prefix = f"{key}:"
    try:
        for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
            if not line.startswith(prefix):
                continue
            # Split once on '=' (value can contain '=' rarely, but we don't care).
            parts = line.split("=", 1)
            if len(parts) != 2:
                return None
            return parts[1].strip()
    except OSError:
        return None

    return None


def _is_multi_config(build_dir: Path, generator_arg: Optional[str]) -> bool:
    # Prefer definitive cache checks once configured.
    cfg_types = _read_cmake_cache_value(build_dir, "CMAKE_CONFIGURATION_TYPES")
    if cfg_types:
        return True

    gen = _read_cmake_cache_value(build_dir, "CMAKE_GENERATOR")
    if gen:
        g = gen.lower()
        return ("visual studio" in g) or ("xcode" in g) or ("multi-config" in g)

    # Fall back to heuristic on the requested generator string.
    if generator_arg:
        g = generator_arg.lower()
        return ("visual studio" in g) or ("xcode" in g) or ("multi-config" in g)

    return False


def _ensure_cmake_exists() -> None:
    if shutil.which("cmake") is None:
        raise SystemExit("cmake not found on PATH. Install CMake and try again.")


def _pick_default_generator() -> str:
    """
    Pick a reasonable default generator for the current OS.
    - Prefer Ninja when available (fast, consistent, cross-platform).
    - Fall back to Visual Studio on Windows, Makefiles elsewhere.
    """
    if shutil.which("ninja") is not None:
        return "Ninja"

    if os.name == "nt":
        return "Visual Studio 17 2022"

    return "Unix Makefiles"


def _copy_compile_commands(build_dir: Path, repo_root: Path) -> None:
    src = build_dir / "compile_commands.json"
    if not src.exists():
        print(f"note: no compile_commands.json at {src} (configure/build may not have generated it yet).")
        return

    dst_dir = repo_root / "build"
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / "compile_commands.json"
    shutil.copyfile(src, dst)
    print(f"updated {dst} (from {src})")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument(
        "--action",
        choices=["configure", "build", "rebuild", "clean", "clobber", "clobber-all"],
        default="build",
        help="Action to perform.",
    )
    parser.add_argument(
        "--config",
        choices=["Debug", "Release"],
        default="Debug",
        help="High-level config. Release maps to RelWithDebInfo by default.",
    )
    parser.add_argument(
        "--release-type",
        choices=["RelWithDebInfo", "Release"],
        default="RelWithDebInfo",
        help="Which CMake build type to use when --config Release.",
    )
    parser.add_argument(
        "--generator",
        default=None,
        help="CMake generator to use (e.g. 'Ninja', 'Unix Makefiles', 'Visual Studio 17 2022', 'Xcode'). "
        "If omitted, CMake chooses its default generator.",
    )
    parser.add_argument(
        "--arch",
        default="x64",
        help="CMake -A architecture for Visual Studio generators (Windows only). Default: x64.",
    )
    parser.add_argument(
        "--build-root",
        default=None,
        help="Build root folder (default: <repo>/build/cmake).",
    )
    parser.add_argument(
        "--no-update-compile-commands",
        action="store_true",
        help="Do not update build/compile_commands.json after configure/build.",
    )

    args = parser.parse_args(argv)

    _ensure_cmake_exists()

    repo_root = Path(__file__).resolve().parents[1]
    build_root = Path(args.build_root).resolve() if args.build_root else (repo_root / "build" / "cmake")

    cmake_build_type = "Debug" if args.config == "Debug" else args.release_type

    generator = args.generator or _pick_default_generator()

    gen_slug = _slug(generator)
    build_dir = build_root / gen_slug / cmake_build_type

    def configure() -> None:
        build_dir.mkdir(parents=True, exist_ok=True)

        cmd = [
            "cmake",
            "-S",
            str(repo_root),
            "-B",
            str(build_dir),
            "-G",
            generator,
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]

        if "visual studio" in generator.lower() and os.name == "nt":
            cmd += ["-A", args.arch]

        # For single-config generators, set CMAKE_BUILD_TYPE. For multi-config, it is ignored/meaningless.
        # We still keep separate build folders by config to avoid confusion.
        if not _is_multi_config(build_dir, generator):
            cmd += [f"-DCMAKE_BUILD_TYPE={cmake_build_type}"]

        _run(cmd)

    def ensure_configured() -> None:
        if not (build_dir / "CMakeCache.txt").exists():
            configure()

    def build(clean_first: bool = False) -> None:
        ensure_configured()
        cmd = ["cmake", "--build", str(build_dir)]
        if clean_first:
            cmd += ["--clean-first"]
        if _is_multi_config(build_dir, generator):
            cmd += ["--config", cmake_build_type]
        _run(cmd)

    def clean() -> None:
        if not build_dir.exists():
            print(f"nothing to clean: {build_dir} does not exist")
            return
        ensure_configured()
        cmd = ["cmake", "--build", str(build_dir), "--target", "clean"]
        if _is_multi_config(build_dir, generator):
            cmd += ["--config", cmake_build_type]
        _run(cmd)

    def clobber() -> None:
        if not build_dir.exists():
            print(f"nothing to delete: {build_dir} does not exist")
            return
        print(f"deleting {build_dir}")
        shutil.rmtree(build_dir)

    def clobber_all() -> None:
        if not build_root.exists():
            print(f"nothing to delete: {build_root} does not exist")
            return
        print(f"deleting {build_root}")
        shutil.rmtree(build_root)

    if args.action == "configure":
        configure()
        if not args.no_update_compile_commands:
            _copy_compile_commands(build_dir, repo_root)
        return 0

    if args.action == "build":
        build(clean_first=False)
        if not args.no_update_compile_commands:
            _copy_compile_commands(build_dir, repo_root)
        return 0

    if args.action == "rebuild":
        build(clean_first=True)
        if not args.no_update_compile_commands:
            _copy_compile_commands(build_dir, repo_root)
        return 0

    if args.action == "clean":
        clean()
        return 0

    if args.action == "clobber":
        clobber()
        return 0

    if args.action == "clobber-all":
        clobber_all()
        return 0

    raise AssertionError("unreachable")


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except subprocess.CalledProcessError as e:
        raise SystemExit(e.returncode)

