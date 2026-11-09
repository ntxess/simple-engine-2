# Code style and linting

All new C++ (engine/, games/, apps/, tests/, bench/, tools/) follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
The legacy tree (core/, app/) keeps its own style (ADR-005).

## Rules in force

- Files: `.h` and `.cc`; tests are `*_test.cc`. Public headers live at
  `<module>/include/<module>/<name>.h` and are included with quotes as
  `"se_core/world.h"`.
- Header guards are `#define` guards named from the path below `include/`:
  `SE_CORE_WORLD_H_`. Each module's `include/CPPLINT.cfg` sets `root=.` so
  cpplint derives that name. No `#pragma once`.
- Line length 80. Formatting is whatever clang-format (BasedOnStyle: Google)
  produces; run `python tools/lint/run_lint.py clang-format --fix`.
- Names:
  - types, functions and methods: `PascalCase` (`std`-style `begin`, `end`,
    `size`, `empty`, `data`, `swap` are allowed for container types);
  - variables and parameters: `snake_case`;
  - class data members: trailing underscore (`owner_`); struct members plain;
  - constants with static storage, constexpr values and enumerators: `kName`;
  - macros: `SE_` plus upper case;
  - namespaces: lower case (`se`, `se::jobs`).
- Every file starts with `// Copyright 2026 The simple-engine-2 Authors.`
- Banned: `<filesystem>`, `<ratio>`, `<cfenv>`, `<fenv.h>`, RTTI
  (`dynamic_cast`, `typeid`), `using namespace`, non-explicit single-argument
  constructors, `long`/`short` instead of fixed-width integers.
- `auto` only where the type is obvious or unutterable.
- Accessors are PascalCase like other methods (`Owner()`, `SetOwner()`),
  because clang-tidy's identifier rule allows no snake_case methods besides
  the container names above (ADR-017).
- Catch2 tests use `REQUIRE(x == literal)` where a comparison has a literal
  operand: cpplint's `readability/check` rule is written for glog's `CHECK`
  and suggests `CHECK_EQ`, which Catch2 does not have.

## Exceptions (ADR-006)

Engine code never throws; return `std::expected<T, se::Error>`. A `catch`
may appear only in a file marked `// se-fault-boundary`, and only to contain
third-party throws at the documented boundaries. Test code is exempt.

## Simulation code

Modules declared `DETERMINISTIC` (se_core, se_sim, game modules) may not read
wall clocks, use unseeded randomness or call CRT transcendentals; use
`se::dm::Sin` and friends. Nor may they declare mutable `static` variables
(function-local, member or file scope): such state is shared by every World
and thread, and a non-trivial one is destroyed at exit. Constants are
`static const`, `constexpr` or `constinit`; a never-destroyed singleton is
`static T* const kName = new T`. A single line can opt out with
`// se-allow(<rule>): <reason>` (rules: throw, rtti, header, clock, random,
math, static, catch).

## Threads, debugging and platform headers (M4)

Engine threads are named roles started with `se::StartThread` (se_os); no
other engine module creates a thread (`std::thread`, `std::jthread`,
`CreateThread`, `_beginthreadex`, `pthread_create`). Engine code never calls
`DebugBreak`, `__debugbreak` or `__builtin_trap`, and never writes a dump in
process (`MiniDumpWriteDump`, `<dbghelp.h>`): a fault aborts and crashpad
writes the dump out of process. Nothing, tests included, calls
`timeBeginPeriod`/`timeEndPeriod`. Only se_log includes spdlog, and only
se_os, se_crash, se_platform and se_render include `<windows.h>`.
tools/checks/check_banned_api.py and check_includes.py enforce these, each
with a planted fixture (`checks.planted.*`).

## Tools

| Tool | Config | Run |
|---|---|---|
| clang-format (LLVM 21) | `.clang-format` | `python tools/lint/run_lint.py clang-format [--fix]` |
| cpplint 2.x (`python -m pip install --user cpplint`) | `CPPLINT.cfg` | `python tools/lint/run_lint.py cpplint` |
| clang-tidy (LLVM 21) | `.clang-tidy` | `python tools/lint/run_lint.py clang-tidy --build-dir out/build/msvc-dev` |
| include and API scans | cmake/SeModule.cmake | `ctest --preset msvc-dev -L checks` |

All of them run as CTest tests with the `lint` and `checks` labels, so every
commit-gate preset enforces them. Each linter and checker has a planted
fixture (tools/lint/planted, tools/checks/planted) proving its gate can fail.
`scripts/install-hooks.ps1` installs an optional pre-commit hook.
