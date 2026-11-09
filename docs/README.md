# simple-engine-2 documentation

The engine is being redesigned on the `redesign/engine-v2` branch. The new
targets grow beside the legacy SFML tree, which is kept as a deterministic
behaviour reference until every rule and editor operation is ported.

| Document | What it holds |
|---|---|
| [design/mt-design.md](design/mt-design.md) | The specification being implemented: one owner thread with fork-join workers and helper threads, the 25 required changes, API sketch, build order |
| [design/dedicated-driver.md](design/dedicated-driver.md) | The dedicated 120 Hz sim-thread driver, built behind `--sim-driver=threaded` |
| [design/costfree-end-state.md](design/costfree-end-state.md) | The end state the design builds on: modules, World, renderer, editor, assets |
| [design/platform-constraints.md](design/platform-constraints.md) | Threading rules of SDL3, Dear ImGui, EnTT, miniaudio and Windows |
| [design/prototype-measurements.md](design/prototype-measurements.md) | The measurement rig's results on the 7700X |
| [decisions.md](decisions.md) | Architecture decision records |
| [style.md](style.md) | Google C++ style, linting and the exceptions policy |
| [deps.md](deps.md) | Pinned dependencies, hashes and why each is used |
| [gates.md](gates.md) | Verification gates that need hardware or setup not on the dev machine |
| [ledger.md](ledger.md) | Completeness ledger: every legacy feature ported, re-specified or deleted |
| [implementation-plan.md](implementation-plan.md) | The approved milestone plan (M0-M19) |
| [milestone-specs.md](milestone-specs.md) | Detailed specs for M1 to M5 |
| [rules/gameplay-rules.md](rules/gameplay-rules.md) | Declared gameplay rules R1-R13 applied to the legacy reference (milestones M1b and M3a) |
| [../tests/fixtures/reference/README.md](../tests/fixtures/reference/README.md) | The legacy reference scenarios, inputs and golden projections (legacy_replay, tests/legacy, ref.sandbox) |
| [../bench/thresholds.json](../bench/thresholds.json) | se_bench gates and the measurements behind them |

## Modules and tools

| Target | Directory | What it is |
|---|---|---|
| se_core | engine/se_core | Kernel: World, Schedule, component registry, hashing, detmath, jobs, time (M2) |
| se_sim | engine/se_sim | Game modules, GameStateStack, SessionLog, replays, SimDriver (M2) |
| se_render_core | engine/se_render_core | Headless renderer half: SpriteRenderer, Camera2D, BuildRenderList; depends on se_core only (M3) |
| game_rampage | games/rampage | The legacy Sandbox's gameplay on the kernel: components, systems, scenes and prefabs (assets/), legacy scenarios, projections (M3) |
| se_platform | engine/se_platform | SDL3 on the main thread: SDL's initialisation, the event filter and Inbox, WakeMain, the window and its Win32 subclass (Modal, session end, power), se::fs on SDL's filesystem API (M5) |
| se_render | engine/se_render | The SDL_GPU device (D3D12 on Windows, Vulkan on Linux) and the frame transaction, clear-only until M7 (M5) |
| se_runtime | engine/se_runtime | The App on SDL's main callbacks: boot steps, the frame, the lifecycle and two-phase quit state machines, BasicInputService, the noexcept callbacks (`SE_DEFINE_SDL_APP`) and the M4 integration hooks (M5) |
| se_replay | tools/se_replay | Headless runs of a game module: per-tick hashes, replays, projections |
| refcmp | tools/refcmp/refcmp.py | Compares two `.seproj` projections within the M3 tolerances (Python 3, stdlib only) |
| se_bench | bench/se_bench | Benchmark suites and their Release gates (`bench.*`) |
| legacy tree | core/, app/ | The SFML reference (`SE_BUILD_LEGACY`, legacy presets) |

## Building

Windows, from the repository root (needs Visual Studio 18 with MSVC 14.51):

```bat
scripts\with-msvc.cmd cmake --workflow --preset msvc-dev
```

Headless runs of a game module, with per-tick hashes, replays and the
determinism cross-check:

```bat
out\build\msvc-dev\bin\se_replay --module kernel_test --ticks 600 --fixed-frame-dt 16.6667 --hash-out hashes.txt
```

The rampage module runs the sandbox scene (assets/scenes/sandbox.scene.json
by default, or `--scene FILE`) or a legacy scenario with its recorded input,
and writes the legacy projection format; refcmp compares it with the
reference's golden (positions within 1e-3 px, rotations within 1e-3 degrees,
liveness and stats exact). From the repository root:

```bat
out\build\msvc-dev\bin\se_replay --module rampage --legacy-scenario tests/fixtures/reference/effects.scenario.json --inputs tests/fixtures/reference/effects.seinput --ticks 3600 --projection-out effects.seproj
python tools/refcmp/refcmp.py effects.seproj tests/fixtures/reference/effects.seproj.gz
```

Add `--legacy-rules v1` and use tests/fixtures/reference/v1/ to compare with
the legacy-ref-v1 rules (R1-R10). CTest runs these as `ref.sandbox.NAME` and
`ref.sandbox_v1.NAME` (label `reference`).

Benchmarks (`--n`, `--iterations`, `--warmup`, `--json FILE`; `--list`
names the suites; `--max-p95-ms` is the gate the `bench.*` tests pass):

```bat
out\build\msvc-release\bin\se_bench --suite renderlist_10k
out\build\msvc-release\bin\se_bench --suite rampage_tick_5k --json rampage_tick_5k.json
```

The `bench.*` tests run only in the msvc-release preset, against
bench/thresholds.json; run them on an otherwise idle machine.

`scripts/ci.ps1` runs the commit gate (msvc-dev, msvc-asan, clangcl-dev,
including the `lint` and `checks` test labels); `scripts/ci.ps1 -Milestone`
adds Debug, Release, the WSL Linux sanitizer presets and the desktop
tests. `scripts/ci.ps1 -Desktop` runs only the desktop tests (label
`desktop`: real windows on this machine's desktop, opened without taking
focus) on msvc-dev and msvc-asan. The legacy tree
builds with the `legacy-msvc-debug` and `legacy-msvc-release` presets.
