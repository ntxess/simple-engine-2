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
| [milestone-specs.md](milestone-specs.md) | Detailed specs for M1 to M7 |
| [rules/gameplay-rules.md](rules/gameplay-rules.md) | Declared gameplay rules R1-R13 applied to the legacy reference (milestones M1b and M3a) |
| [../tests/fixtures/reference/README.md](../tests/fixtures/reference/README.md) | The legacy reference scenarios, inputs and golden projections (legacy_replay, tests/legacy, ref.sandbox) |
| [../bench/thresholds.json](../bench/thresholds.json) | se_bench gates and the measurements behind them |

## Modules and tools

| Target | Directory | What it is |
|---|---|---|
| se_core | engine/se_core | Kernel: World, Schedule, component registry, hashing, detmath, jobs, time (M2) |
| se_os | engine/se_os | The headless OS layer for threads: StartThread and RoleScope (names, roles, registry slots, QoS, CPU sets), the CPU topology and pool sizes, the unbiased clock, debugger detection, ExecutableDir; depends on se_core only (M4, ADR-029) |
| se_log | engine/se_log | The log worker: drains the per-thread log rings into the spdlog file, stderr and panel sinks; the only module that includes spdlog (M4, ADR-032) |
| se_sim | engine/se_sim | Game modules, GameStateStack, SessionLog, replays, SimDriver (M2) |
| se_render_core | engine/se_render_core | Headless renderer half: SpriteRenderer, Camera2D, BuildRenderList; depends on se_core only (M3) |
| se_crash | engine/se_crash | Out-of-process crash capture on crashpad: Start/CrashGuard, RegisterStream, DumpWithoutCrash, the terminate annotation; nothing uploaded; a stub on Linux and in msvc-asan (M4, ADR-033, ADR-034). The watchdog skeleton: role kWatchdog, the HangDetector (boot budget; hangs declared from M12), RequestDump, the shutdown deadline (M4 integration, ADR-043) |
| game_rampage | games/rampage | The legacy Sandbox's gameplay on the kernel: components, systems, scenes and prefabs (assets/), legacy scenarios, projections (M3) |
| se_platform | engine/se_platform | SDL3 on the main thread: SDL's initialisation, the event filter and Inbox, WakeMain, the window and its Win32 subclass (Modal, session end, power), se::fs on SDL's filesystem API (M5) |
| se_render | engine/se_render | The SDL_GPU device (D3D12 on Windows, Vulkan on Linux) and the frame transaction, clear-only until M7 (M5); `se::present` (present.h): the SDL presentation patch's frame-latency wait, present statistics, GPU timestamps, native handles, device status and present mode, all unsupported in the unpatched build (M6a, ADR-050 to ADR-053) |
| se_runtime | engine/se_runtime | The App on SDL's main callbacks: boot steps, the frame, the lifecycle and two-phase quit state machines, BasicInputService, the noexcept callbacks (`SE_DEFINE_SDL_APP`) (M5); the main thread's RoleScope, crash capture and its dump streams, the log worker and the watchdog in boot order (integration.h; ADR-042) |
| rampage | games/rampage/app | The rampage executable: the sandbox scene at 120 Hz in a window, clear-only until M7 (M5); crashpad_handler is copied next to it |
| se_replay | tools/se_replay | Headless runs of a game module: per-tick hashes, replays, projections |
| refcmp | tools/refcmp/refcmp.py | Compares two `.seproj` projections within the M3 tolerances (Python 3, stdlib only) |
| read_dump | tools/minidump/read_dump.py | Decodes a crash dump: threads, exception, annotations and the first-party streams (breadcrumbs, log rings, InputFrame ring), and lists every record that does not fit its index (a stale or torn InputFrame slot, log sequence numbers out of order) under "problems", apart from the one a running writer was overwriting (ADR-046); the crash tests' oracle (Python 3, stdlib only) |
| se_crash_probe | tests/crash | Registers dump streams with known contents, then crashes or requests dumps; the `crash.*` tests (label crash) check its dumps with read_dump. `crash.rampage_*` (labels desktop and crash) check the M4 exit dumps of the rampage executable |
| se_present_tests | tests/present | The present API on both GPU drivers (`present.d3d12.*`, `present.vulkan.*`, chosen with SE_GPU_DRIVER): caps, GPU timing, handles and status (label gpu), and the frame-latency waits, statistics, resize and present-mode changes (label desktop) |
| SDL patch | cmake/patches | sdl3-3.4.20-se-presentation.patch: `include/SDL3/SDL_gpu_se.h` and its D3D12 and Vulkan backends, applied with `SE_SDL_PATCH` (docs/deps.md has the rebase notes) |
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

The rampage executable runs the sandbox scene in a window (clear-only
until M7; the title shows the tick and the lifecycle state). `--frames N`
quits after N presented frames; the runtime's options are listed in
engine/se_runtime/include/se_runtime/app_config.h:

```bat
out\build\msvc-dev\bin\rampage --frames 600
```

Every run of an engine executable writes its log through the log worker to
`<pref>/logs/<app>.log` (rotated at 5 MB, 3 kept) and to stderr, and keeps
its crash database in `<pref>/crashdb`; `<pref>` is SDL's pref path
(`%APPDATA%\simple-engine\rampage\` for rampage) or `--pref-dir DIR`.
`--profile` starts Tracy (builds with `SE_TRACY`: msvc-dev, msvc-release,
clangcl-dev), which listens on 127.0.0.1:8086 for the profiler GUI.

Crash dumps (Windows; the crashpad handler writes them into the database's
reports/ directory, out of process; nothing is uploaded) decode with:

```bat
python tools\minidump\read_dump.py out\build\msvc-dev\crash_out\abort\db\reports\<id>.dmp
python tools\minidump\read_dump.py DUMP --json
python tools\minidump\read_dump.py DUMP --records 20
```

The text output lists the streams, the threads with their names, the
exception record (code and thread), the annotations (`se.app`,
`se.dump_reason` for a requested dump, `se.terminate` after
std::terminate) and the first-party streams: the breadcrumb table (one row
per ThreadRegistry slot: state, name, OS thread id, role, phase, system,
chunk and tick), one log ring per slot (0x53450100 + slot; the shared
library ring is slot 64), each record as the log worker would print it, and
the InputFrame ring (0x53450002) with each slot's seqlock stamp (2n + 2 for
a complete frame n, odd while it was being written) and its frame.
`--records N` shows the newest N records of each ring; `--json` gives
everything for scripts (tests/crash/rampage_dump_test.py shows how). In a
test build, `rampage --crash-test after-ticks:240` and
`rampage --dump-test after-ticks:240` write the M4 exit dumps.

`scripts/ci.ps1` runs the commit gate (msvc-dev, msvc-asan, clangcl-dev,
including the `lint` and `checks` test labels; `-Presets msvc-unpatched-dev`
builds and tests without the SDL presentation patch); `scripts/ci.ps1 -Milestone`
adds Debug, Release, the WSL Linux sanitizer presets and the desktop
tests. `scripts/ci.ps1 -Desktop` runs only the desktop tests (label
`desktop`: real windows on this machine's desktop, opened without taking
focus) on msvc-dev and msvc-asan. The legacy tree
builds with the `legacy-msvc-debug` and `legacy-msvc-release` presets.

Presets set every `SE_*` option a build depends on explicitly. Deleting a
cache variable from a preset does not reset it in an existing build tree,
so a value the preset used to set stays in force there: until `base` set
`SE_BUILD_PLATFORM`, the WSL clone kept the pre-M5 `OFF` and silently
skipped the platform and runtime tests. To start a tree afresh, configure
with `cmake --preset <name> --fresh`.
