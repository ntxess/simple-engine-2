# Implementation plan (approved 2026-11-08)

## Context
The user approved implementing the published **Multithreaded Engine Design** (https://claude.ai/artifact/4NwGj9Td5wNJbT7B4nNEE7) on a new branch. That design sits on top of the audit's **cost-free end state**: new CMake targets grown beside today's SFML tree, using SDL3 + SDL_GPU, glaze, spdlog, Catch2, Tracy and miniaudio, with layering enforced by the link graph.

Decisions taken while planning:
- **Threading: "Both, compared."** Build the recommended design:
  - one owner thread, the SDL main thread, running input, 120 Hz ticks and rendering;
  - fork-join workers, shipped at W = 0;
  - helper threads: a background I/O lane, a log worker, a watchdog, an input sampler and a vblank sampler.

  Also build build step 13's dedicated 120 Hz sim-thread driver (SimDriver + SnapshotChannel), unconditionally, behind `--sim-driver=inline|threaded`. Inline stays the default, and tests compare the two drivers' per-tick hashes.
- **Dependencies:** CMake FetchContent with pinned `URL` + `URL_HASH SHA256`. Downloads go to a shared `.deps/` archive cache, so after the first fetch every preset configures offline.
- **Legacy tree:** stabilise it in place as a deterministic behaviour reference, built behind `SE_BUILD_LEGACY`. The new kernel's gameplay rules are compared against it using recorded inputs.

Source specs:
- The design is in `mt-result.json` (`.result.final`, plus `.result.designs.dedicated` for step 13).
- The end state is in `costfree-result.json` (`.result.synth`).
- Both files sit in this session's scratchpad. M0 copies them into the repo as `docs/design/`, so later sessions don't depend on the scratchpad.

## Branch, commits, scope
- `git switch -c redesign/engine-v2 fix-gitignore` (the base keeps the .gitignore fix the vendored files need). Leave the untracked `CppProperties.json` out.
- Commit locally at every green state, with at least one commit per milestone exit, and tag `m0`, `legacy-ref-v0`, `legacy-ref-v1`, `m2`, …
- Never push. Offering the SDL patch upstream would be public posting, so it needs a separate go-ahead.
- The program has 20 milestones (M0–M19). This session works through them in order, as far as it gets, and stops at milestone boundaries.
- Gates that need hardware this machine doesn't have are recorded as pending in `docs/gates.md`, with the exact command to run. They never block a milestone.

## Layout and targets (new code beside legacy `core/` and `app/`)

```
CMakeLists.txt (rewritten; legacy blocks → cmake/Legacy.cmake)   CMakePresets.json   project.json
cmake/  BuildTypes Warnings Dependencies SeModule Checks Legacy ApplyPatch ImGui .cmake, patches/
engine/ se_core se_sim se_render_core se_assets se_audio se_platform se_render se_crash se_runtime se_imgui se_editor
games/rampage/ (game_rampage lib + rampage exe)   apps/editor/ (simple_editor exe)
tests/ (support kernel sim rampage render platform death compile_fail golden fixtures legacy)
bench/ (se_bench)   tools/ (se_replay checks refgen refcmp minidump presentmon)
docs/  design/ decisions.md rules/gameplay-rules.md ledger.md gates.md deps.md
```

- Headers live at `<target>/include/<target>/x.h` and sources at `<target>/src/x.cc`, following Google style (see below). File names follow the design's apiSketch (`thread_role.h`, `time.h`, `world.h`, `jobs.h`, `background_lane.h`, `frame_pacer.h`, `sim_driver.cc`). Project headers are included with quotes as `"se_core/world.h"`.
- **One deviation from the end state:** the headless halves are split out into `se_sim` and `se_render_core`. That keeps `se_tests`, `se_bench` and the WSL TSan build free of SDL.
- `cmake/Checks.cmake` enforces the link graph at configure time:
  - se_core may use only std, EnTT, glaze and spdlog;
  - only se_imgui and se_editor may reach ImGui;
  - rampage never links ImGui;
  - legacy and se_* targets never meet.
- `tools/checks/*.py` adds banned-include and banned-API scans: clocks, `random_device` and the cmath transcendentals in sim code, and later stored `ImTextureID` / `SDL_GPUTexture*` in the editor. Each rule has a planted violation that CTest expects to fail.
- **Presets** (Ninja):
  - Windows: `msvc-debug`, `msvc-dev` (the default: `/O2`, asserts on, `_ITERATOR_DEBUG_LEVEL=0` everywhere), `msvc-release`, `msvc-asan`, `clangcl-dev` (`-Werror=thread-safety`), `msvc-unpatched-dev`;
  - legacy: `legacy-msvc-debug` and `legacy-msvc-release`;
  - WSL: `linux-clang-debug`, `linux-clang-asan`, `linux-clang-tsan`;
  - `deps-smoke`, which only fetches and builds the dependencies.
- MSVC presets use VS 18 (`vcvars64.bat -vcvars_ver=14.51`), because glaze needs MSVC 14.50 or later, so VS 2022 is out.
- `scripts/ci.ps1` runs the workflow presets locally; there is no hosted CI because nothing is pushed.

## Code style and linting (enforced from M0, on every commit)
The Google C++ Style Guide applies to all new C++: `engine/`, `games/`, `apps/`, `tests/`, `bench/` and `tools/`. Legacy `core/` and `app/` keep their surrounding style, so the rule commits stay reviewable diffs against the reference (your decision).

**Style rules in force**
- `.h` / `.cc` files, `*_test.cc` for tests, `#define` header guards in the form `SE_CORE_WORLD_H_` (not `#pragma once`), Google include order, and an 80-column limit.
- Naming:
  - types and functions: `PascalCase` (accessors may be `snake_case`);
  - variables: `snake_case`;
  - class data members: trailing `_`;
  - constants and enumerators: `kName`;
  - macros: `SE_` + caps;
  - namespaces: lowercase (`se`, `se::jobs`).
- The design's apiSketch identifiers are translated, recorded as an ADR: `parallelFor`→`ParallelFor`, `tickInline`→`TickInline`, `deriveGuid`→`DeriveGuid`, `m_desc`→`desc_`, `Cadence::Locked`→`Cadence::kLocked`.
- No `<filesystem>`, `<ratio>` or `<cfenv>`. Path and directory work goes through a thin `se::fs` interface implemented in se_platform with SDL3's filesystem API. No RTTI (`dynamic_cast` or `typeid`) in engine code. `auto` only where it aids readability. Output parameters follow the guide's current rules.

**Exceptions: the one recorded deviation (your decision).**
- Engine code never throws; errors are returned as `std::expected<T, se::Error>`.
- `try`/`catch (...)` appears only at these documented fault boundaries, to contain throws from std, EnTT or glaze such as `bad_alloc`:
  - the SDL callbacks;
  - Play tick and render isolation (→ Play.Faulted);
  - ForkJoinPool chunks;
  - the FrameRecorder transaction.
- A lint check rejects `throw` in engine sources, and any `catch` outside files tagged `// se-fault-boundary`.

**Tooling**
- `.clang-format`: `BasedOnStyle: Google`.
- `CPPLINT.cfg`: Google's `cpplint`, with `root=` set per module so header guards match.
- `.clang-tidy`:
  - check groups: `google-*`, `bugprone-*`, `performance-*`, `readability-*` (identifier naming configured to Google's conventions), selected `modernize-*` and `cppcoreguidelines-*`, `misc-include-cleaner`;
  - `WarningsAsErrors: '*'`;
  - it runs over the `clangcl-dev` compile database.
- clang-format and clang-tidy come from the installed LLVM 21, with the same versions in WSL.

**Enforcement**
- CTest label `lint` runs:
  - `clang-format --dry-run --Werror`;
  - `cpplint`;
  - `clang-tidy`;
  - the banned-API and include checks.
- `scripts/ci.ps1` fails on any lint error. I run it before every commit, alongside `SE_WARNINGS_AS_ERRORS=ON` builds.
- `scripts/install-hooks.ps1` adds an optional pre-commit hook. It is provided, not installed.
- A planted style violation per tool proves the gate fails.

## Downloads this plan approves
Each archive is fetched once into `.deps/` (gitignored), and its SHA256 is recorded in `cmake/Dependencies.cmake` and `docs/deps.md`. Anything not on this list is asked about first.

| Dependency | Pin | Source | Size | Milestone |
|---|---|---|---|---|
| SDL3 | release-3.4.20 | github.com/libsdl-org/SDL release asset SDL3-3.4.20.tar.gz | 15.7 MB | M5 |
| Catch2 | v3.16.1 | catchorg/Catch2 tag archive | ~1 MB | M0 |
| spdlog | v1.17.0 | gabime/spdlog tag archive | ~0.5 MB | M2 |
| glaze | v9.1.0 | stephenberry/glaze tag archive | ~1–2 MB | M2 |
| Tracy | v0.14.1 (or v0.15.0, to match your GUI) | wolfpld/tracy tag archive | ~5–10 MB | M4 |
| sentry-native (contains crashpad) | 0.17.6 | getsentry/sentry-native release asset sentry-native.zip | 9.1 MB | M4 |
| miniaudio | 0.11.26 | mackron/miniaudio tag archive | ~2 MB | M8 |
| LZ4 | v1.10.0 | lz4/lz4 release asset | ~0.5 MB | M9 |
| Dear ImGui | v1.93.0-docking | ocornut/imgui tag archive | ~2 MB | M10 |
| stb | pinned commit | nothings/stb commit archive | ~2 MB | M7 |

- `cpplint` (latest, from PyPI via `python -m pip install --user cpplint`; ~0.1 MB) is used in M0 for linting.
- EnTT stays as the vendored `dep/entt-3.16.0`, which the design was verified against; v4.0 breaks compatibility. It is built with `ENTT_USE_ATOMIC` in every translation unit.
- Shaders: DXIL comes from the Windows SDK's `dxc.exe`, which is already installed. SPIR-V comes from WSL (`apt install clang lld cmake ninja-build glslang-tools`, a few hundred MB from the Ubuntu repositories). The same WSL packages are needed for the TSan presets.
- Legacy builds still fetch SFML's freetype, ogg, flac and vorbis unpinned, as they do today.

## Milestones
These follow the design's 13 build steps (MT) and the end state's order (CF a–g). Exit criteria are the design's own wording wherever it gives one.

| # | Milestone | Exit criteria (abridged) |
|---|---|---|
| M0 | Branch and build foundation: presets, Warnings (`se_warnings`, `se_determinism` = `/fp:precise`, `-ffp-contract=off`), Dependencies, SeModule, Checks, Legacy.cmake, ApplyPatch.cmake. **Lint gate:** `.clang-format`, `.clang-tidy`, `CPPLINT.cfg`, CTest `lint` label, exception-policy check. Catch2 smoke test. Delete the stale GoogleTest `tests/test.cpp`. Write `docs/design/`, `docs/decisions.md` (ADRs, including the style translation and the exceptions deviation), `deps.md`, `gates.md`, `ledger.md`, `docs/style.md` | ci-windows green, including `lint`, on msvc-debug/dev/release/asan and clangcl-dev; WSL linux-clang debug and TSan green; every pin hashed; a second preset configures offline; planted violations fail, one per check and per lint tool; legacy-msvc-debug builds `editor_app` |
| M1a | Legacy stabilisation (`core/`, `app/`): one paced main loop replaces the 4 threads (`core/Engine.cpp:116-243`); LegacySimTime ticks replace steady_clock (EventSystem.cpp:95,100,135,144; UpdateEntity*); effects by value (StatusModEvent.hpp:26); seeded RNG (GameOfLifeSim.cpp:29, Editor.cpp:939); input sampled per tick and recorded as `.seinput` (Sandbox.cpp:111); boundary clamp moved out of render (Sandbox.cpp:155); imgui_stdlib and dt in seconds (EditorComponentVisitor.cpp:39, Editor.cpp:136); remove the unused EventBus threads; SemanticProjection hash; headless `legacy_replay` exe | identical projection hashes across runs and across `--fps` 30/60/144/160; tag `legacy-ref-v0` |
| M1b | Declared rules R1–R10, one commit each with re-baselined goldens: TEMPTIMED always reverts; per-pair cooldown decided once per tick; brute-force overlaps; unrotated AABB; seconds throughout; zero-length path guard; OVERTIME timing; sorted contacts; system order; deferred destroys. Rules written to `docs/rules/gameplay-rules.md` | 3 scenarios × 3600 ticks of golden projections; tag `legacy-ref-v1` |
| M2 | **Headless kernel (MT 1).** Detailed below | golden hashes identical under 30/60/144/160 Hz clocks including a GameOver transition and logged edits; stepper scenarios (≤1 hitch per spike at N=50k, no 3-tick frames at 59.94 Hz Locked, HalfRate at 144 Hz N=50k); FMA3 on/off hashes match; ENTT_USE_ATOMIC set, storages pre-created, structural asserts in place; serialization round trips |
| M3 | Headless gameplay port (game_rampage): components (Team, Stats, EffectSource, ActiveEffects, PairCooldowns, Collider roles, KinematicBody, intents, Path); systems Input→Intent→Movement→Collision (spatial hash, sorted contacts)→Effects→GameRules→Cleanup; sandbox scene and prefab JSON. se_render_core RenderList and interpolation; refgen/refcmp; se_bench skeleton | broadphase matches brute force; effect, path and interpolation tests; legacy comparison (`ref.sandbox.*`) exact on liveness and stats, ≤1e-3 px on positions, over 3600 ticks; RenderList of 10k sprites ≤ 1 ms in Release |
| M4 | Thread roles and crash capture (MT 2): RoleScope (name, QoS, CPU set), SPSC ring, breadcrumbs, per-thread log rings + LogWorker (spdlog sinks used synchronously), se_crash (crashpad via sentry, no upload), Tracy | CI fails on each planted violation; crash and watchdog-requested dumps, written out of process, contain breadcrumbs, log rings and the InputFrame ring |
| M5 | Platform loop on SDL main callbacks (MT 3): noexcept callbacks, NULL-safe AppQuit, event filter + Inbox, wakeMain, window subclass (Modal, session end, power), two-phase quit skeleton, Idle and Suspended, clear-only FrameRecorder | a worker-pushed LOW_MEMORY reaches main without draining on the worker; ticks continue during a drag; a NULL acquire never suspends; AppInit failure at every step is clean under ASan |
| M6 | SDL presentation patch (frame-latency waitable, present statistics, timestamps, native handles, device-removed query, present_wait) + FramePacer (Paced, Standard, VRR, HalfRate, adaptive latency) + VblankSampler (MT 4) | under 1% missed vblanks when Paced; an injected late frame drains within 2 frames; two monitors pace to the window's monitor; the unpatched preset falls back to Standard. VRR, photodiode and PresentMon are pending gates |
| M7 | Renderer2D on main (MT 5, CF b): GpuResources (resolve per frame, retire queue, upload queue, residency on fence), full FrameRecorder transaction, HLSL shaders, viewport targets; Sandbox on screen; golden images vs legacy | 10k-sprite RenderList ≤ 1 ms; throw injection at every recording step for 100 frames on D3D12 and Vulkan under ASan; a texture reloaded while minimized is correct after restore |
| M8 | Input placement and audio (MT 6): InputService with timestamped edges and the buffer window, ActionMap from config.json:30-46, 1 kHz InputSampler, raw keyboard; miniaudio voices with refcounted PCM and starts at tick anchor + D | presses 8 ms apart land on different ticks; replays reproduce; a frame's two tick sounds start 8.33 ms apart; ASan clean on stop and reload |
| M9 | Background lane + AssetRegistry (MT 7, CF d): Urgent and Background classes, strands, revisions, Durable jobs, ~2 ms steps; Handle\<T\>, AssetScope; LZ4 atlas pages | quit joins within one step; Durable saves survive a throttled disk; no out-of-order overwrite |
| M10 | Editor shell on the single loop (MT 8, CF c): thread_local GImGui, EditDocument/commands/undo, panels (ported waypoint canvas and drawers), Play/Pause/Step/Stop with SessionLog and asset freeze, Play.Faulted, recovery files, dialog mailboxes | Stop leaves an identical document hash; exceptions keep the editor alive and inspectable; an editor session replays headless; foreign-thread dialog callbacks are safe |
| M11 | Save/load, prefabs, hot reload, real component-removal command (CF d) | undo∘redo = identity; prefab overrides; editor and runtime load identical Worlds |
| M12 | Watchdog (MT 9) | an injected hang gives an out-of-process dump within 6 s that replays to the hanging tick; teardown < 2 s |
| M13 | rampage runtime: GameStates Title, Gameplay, Pause and GameOver; no ImGui (CF e) | plays the Sandbox from files; quit durability tests pass |
| M14 | ForkJoinPool threaded, still disabled (MT 10) | TSan property tests (no lost wake); identical hashes for W = 0/1/3/7 across 8 chaos seeds |
| M15 | se_bench on the real stack (MT 11) | thresholds frozen for the 7700X; other machines pending |
| M16 | Enable fork sites only where se_bench records a miss, e.g. Game of Life row bands (MT 12) | faster than inline at the median; overhead ≤ 10%; tails no worse; identical hashes |
| M17 | **Dedicated sim driver (MT 13, unconditional).** M17a headless (can start after M4): SimThread with PacedClock, SnapshotChannel, SnapshotRenderSource. M17b integrated: `--sim-driver` switch, editor Play mirror | inline and threaded hashes equal on every golden replay for W = 0/1/3; SnapshotChannel property tests; TSan clean; inline stays the default |
| M18 | Linux parity on Vulkan (CF g) | Linux goldens; real Wayland and X11 machines are pending gates |
| M19 | Rampage primitives as consumers land: tilemaps, parallax, Aseprite import, text, UI, colorblind shader (CF f) | per feature |

Where the design's 25 required changes land:
- #1–2 cadence and cap: M2, wired in M5–M6
- #3 spike sources off the tick: M3, M9, M10
- #4–5 SDL patch and pacing modes: M6
- #6 Modal detection: M5
- #7 two-phase quit: M5, M9, M10
- #8 and #15 noexcept callbacks, event filter and inbox: M5
- #9 NULL acquire, Suspended and DeviceLost: M5–M7
- #10 frame transaction: M5 and M7
- #11 editor protection: M10 and M12
- #12–13 retire queue and upload buffer: M7
- #14 and #18 audio voices, gamepads and edge placement: M8
- #16 dialog mailbox: M10
- #17 background lane: M9
- #19 tick-boundary state changes and SessionLog: M2
- #20 deterministic trig and FMA3 off: M2
- #21 crash and hang dumps: M4 and M12
- #22 Dev preset: M0
- #23 W = 0 and the enabling rules: M2, M14, M16
- #24 CI-checked seams: M0–M4 and M10
- #25 re-measure on the real stack: M15 and gates.md

## M2 headless kernel: contracts to build (engine/se_core, engine/se_sim)
Names below are written as in the design. In code they take Google style: `.h` / `.cc` files, `PascalCase` functions and trailing-underscore members.

**Thread roles and safety annotations**
- `thread_role.hpp` declares two capabilities. `g_main` is main-thread only. `g_simOwner` is World ownership: main holds it in inline mode, and the sim thread holds it under M17's threaded driver. This is decided now so that M17 isn't an annotation sweep.
- `registerMainThread()` exists because se_core can't call `SDL_IsMainThread`; se_platform cross-checks it later.
- `thread_safety.hpp` defines the `SE_REQUIRES` / `SE_CAPABILITY` macros.
- `SE_ASSERT` never calls DebugBreak.

**Time and pacing**
- `time.hpp` is a pure FixedStepper. Its accumulator is an integer in ns × rate, so there are exactly 120 ticks per simulated second.
- It encodes the design's formulas literally:
  - Locked cadence when the period is within 0.5% of k·dt;
  - `k_hi` = floor((T − R − g − m)/S), using p99.5 costs;
  - Normal: `k_max` = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1));
  - Overloaded: clamp(k_95, editorPlay ? 0 : 1, k_rt);
  - m = max(1.5 ms, timer p99 + 0.5 ms);
  - debt older than 0.25 s becomes counted dilation;
  - planNext runs before the sleep;
  - per-tick window ends.
- **ADR:** in Locked cadence, k_rt is the locked ratio, so it is 2 at 59.94 Hz and not ceil(2.002) = 3. A test pins this.
- `CostTracker` and `HalfRateController` live alongside the stepper.

**Determinism**
- `detmath.hpp` is first-party: `dm::sin`, `cos`, `sincos` and `atan2`, built from + − × ÷ and sqrt in a translation unit compiled with determinism flags. It has a `selfTestHash()`.
- `fp::configureProcess()` calls `_set_FMA3_enable(0)`.
- `hash.hpp`: a Guid-keyed, order-independent world hash, folding per-entity digests by sum and xor, with a `--det-crosscheck` mode to find the first differing entity.
- `rng.hpp`: a counter-based RNG keyed by (seed, tick, Guid, stream).
- `guid.hpp`: Guid and EntityRef, plus `World::deriveGuid(parent, tick, index)`.

**Input and world**
- `input_frame.hpp`: a trivially copyable InputFrame (held state, pressed and released counts, axes, up to 32 stamped edges) and the `InputSource` interface.
- `sim_clock.hpp`.
- `component_registry.hpp`: name, version, flags, JSON and BEVE (glaze, confined to the registry translation units), clone, hash, plus migrate and inspect hooks.
- `core_components.hpp`: IdComponent, Name, Transform, PrevTransform, Spawned, DestroyRequest.
- `world.hpp`:
  - World pre-creates every storage;
  - `reg()` asserts the owner thread, not-in-fork, and an open MutationGate on Play Worlds;
  - destroys are requested, then drained in Cleanup, followed by finalizeSpawns;
  - `#error` without `ENTT_USE_ATOMIC`.

**Jobs and scheduling**
- `jobs.hpp`: ForkSite and an inline-only ForkJoinPool. It runs the same chunking as M14's threaded pool, rethrows the first exception, and runs nested forks inline.
- `parallel_view.hpp` checks declared read and write sets in debug builds.
- `schedule.hpp`:
  - phases are Input, Intent, Movement, Collision, Effects, GameRules and Cleanup;
  - systems hold no state;
  - each system is timed into a ring buffer, and breadcrumbs are set as it runs.

**se_sim**
- GameModule.
- GameStateStack: push, replace and pop with a FIFO queue, applied only at tick boundaries. It keeps the semantics of `core/manager/SceneManager.cpp:40-69`.
- SessionLog.
- Replay: ReplayHeader carries the math hash, and ReplayRecorder/ReplayReader use BEVE.
- `SimDriver::runTick`/`tickInline`, the seam that M17 reuses unchanged.
- SyntheticFrameClock.

**Tools and tests**
- `tools/se_replay`: `--fixed-frame-dt`, `--hash-out`, `--jobs`.
- `tests/kernel`: stepper, detmath, hash, rng, world, schedule, session log, game-state stack (the C6 case), serialization, replay and determinism (30/60/144/160 Hz with jitter, FMA3 on and off). Plus death tests and a compile-fail test on the clang presets.

## Parallel work and execution
- **Spine:** M0 → M2 → M3 → M5 → M7 → M10 → M17b. M1b must land before M3's legacy comparison.
- **Parallel tracks:**
  - after M0: legacy M1 (`core/`, `app/`, `tests/legacy`) alongside kernel M2 (`engine/`, `tests/kernel`);
  - after M3: M4 alongside M5, M6 alongside M7, and M8, M9 and M17a together;
  - M12 any time after M5; M14 any time after M4.
- Inside a milestone, independent leaf files are written in parallel by workflow agents in separate worktrees. I merge at the green state and run the full preset matrix before each commit.

## Verification
- **Every commit:** `scripts/ci.ps1` runs these workflow presets:
  - build: msvc-dev, msvc-asan, clangcl-dev;
  - CTest labels: unit, determinism, golden, checks, **lint** (clang-format, cpplint, clang-tidy, exception policy), plus legacy where touched.
  - Nothing is committed with a lint or warning failure.
- **Every milestone:** add msvc-release and the WSL `linux-clang-tsan` / `linux-clang-asan` runs.
- **Determinism**
  - Golden per-tick hashes for each platform and compiler, under 30/60/144/160 Hz synthetic clocks.
  - FMA3 on and off.
  - From M14: W = 0/1/3/7 across chaos seeds.
  - From M17: inline against threaded.
  - From M3: comparison against legacy `legacy-ref-v2` projections (rules R11-R13 are added to the reference in M3a; see docs/milestone-specs.md).
- **Desktop tests** (label `desktop`, from M5) run on this machine, which has an RTX 3080, an AMD iGPU and two monitors: SendInput drags, NULL acquire, AppInit failure, throw injection on D3D12 and Vulkan.
- **Visual checks:** run the sandbox and the editor and screenshot them at M7 and M10.
- **Hardware-only gates** are tracked in `docs/gates.md`: laptops (4-core, 2P+8E on battery), Linux GPU stacks, a VRR panel, a photodiode, PresentMon, a gamepad, sleep/resume, and soaks on the weakest machine.

## Risks to watch
1. **glaze.** It is heavy to compile and needs MSVC 14.51 and CMake 3.31. M0 measures it; the fallback is nlohmann/json behind the same registry.
2. **SDL patch.** It is estimated at 500–900 lines across D3D12 and Vulkan.
   - FetchContent's PATCH_COMMAND would run inside this repo's work tree, so it goes through `cmake/ApplyPatch.cmake` with `GIT_CEILING_DIRECTORIES`.
   - The unpatched preset must stay green.
3. **TSan exists only in WSL.** Win32-only threads (vblank sampler, window subclass, dialogs) rely on asserts and stress tests instead.
4. **Legacy comparison fidelity.** It is tolerance-based and rests on declared rules. If the reference still diverges after M1a, that is a flip condition in the cost-free verdict: stop and reassess.
5. **Toolchain.** CMake 4.2.0 is a release candidate. ImGui 1.93's ImDrawList stroke changes affect the ported widgets. Debug builds run EnTT-heavy code 63–111× slower, so tests at N=50k are labelled `slow` and run in Dev and Release only.
6. **Dedicated-driver hash equality** holds only for replays. Live input latching differs by design.
