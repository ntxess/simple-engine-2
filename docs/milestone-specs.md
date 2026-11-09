# Milestone specs: M1 to M5

Detailed specs for the first milestones after M0. The milestone table and the
exit criteria are in docs/implementation-plan.md; the design is in
docs/design/. Code in the new tree follows docs/style.md (Google style,
exceptions only at fault boundaries); names below are written in that style.

## M1 Legacy reference (in place, core/ and app/; legacy style kept)

### M1a: stabilise, then tag `legacy-ref-v0`

| File | Change |
|---|---|
| core/Engine.hpp/.cpp | Delete the four threads, `m_mutex`, `startThreads` and the thread functions (Engine.cpp:116-243). New `run()`: a `pollEvent` loop instead of the blocking `waitEvent` (:58); an integer accumulator (µs × rate) clamped to 0.25 s, at most 8 steps. Each step: `simTime.tick++`; `frame = replay ? reader.next() : sampler.sampleTick(tick)`; `recorder.record(frame)`; `ctx.input = &frame`; `scene->processInput()`; `scene->update()`; `sceneManager.processChange()` (tick boundary); `projection.write(tick, reg)`. Then clear, render, display. `EngineOptions{headless, replayPath, recordPath, projectionPath, maxTicks, fpsOverride}`. Headless runs ticks back to back with a hidden window (sf::Texture needs a GL context). |
| core/ApplicationContext.hpp | Add `LegacySimTime simTime`, `const LegacyInputFrame* input`, `std::uint64_t seed`, `double realFrameSeconds`. Remove `EventBus` (its two TaskScheduler threads have no callers; grep `eventBus` first). |
| core/util/LegacySimTime.hpp (new) | `{tick, rate = 120, dt = 1.0/120}` and `msToTicks(ms, rate) = ceil(ms*rate/1000)` |
| core/component/StatusModEvent.hpp/.cpp | `Effects effect;` by value (replaces the raw pointer at :26); `startTick`, `lastApplyTick`, `elapsedTicks`; no chrono |
| core/system/EventSystem.hpp/.cpp | Constructor takes `const LegacySimTime&` and the watchdog in ticks; :95, :100, :135, :144 become tick arithmetic. Keep the comparison operators literally; semantic fixes come in M1b. |
| core/component/UpdateEntityPolling/Event.* | `isReady(std::uint64_t nowTick)`; `startTick`, `cooldownTicks` |
| core/system/CollisionSystem.cpp | Pass the tick to `isReady` (:88); copy the effect by value (:104) |
| app/scene/Sandbox.cpp | `processInput` reads `ctx.input->held` instead of `isKeyPressed` (:111-114). The boundary clamp moves from `render()` (:155) into `update()` (render count must not move entities). The player gets a `SpawnIndex`. |
| app/scene/GameOfLifeSim.cpp:29-30, app/editor/Editor.cpp:939-941 | `std::mt19937 rng(ctx.seed ^ stream)`, seed from config |
| app/editor/Editor.cpp:136 | `ImGui::SFML::Update(window, sf::seconds(ctx.realFrameSeconds))` |
| app/editor/EditorComponentVisitor.cpp:39-41 | `ImGui::InputText(label, &effect.targetStat)` from misc/cpp/imgui_stdlib.h (already compiled into ImGui-SFML) |
| core/util/LegacyInput.hpp/.cpp (new) | `LegacyAction` {Up, Left, Down, Right, Jump, Exit, Enter, Back, A1-A6}; `LegacyInputFrame{tick, held, pressed, released}`; `LegacyInputSampler` (edges from events, held sampled once per tick); `.seinput` text format `tick held pressed released` in hex, readable by both trees |
| core/component/SpawnIndex.hpp, core/util/SemanticProjection.hpp/.cpp (new) | One line per entity per tick, ordered by SpawnIndex: x, y, rotation, HP and sorted stats, printed with `%.9g`; FNV-1a 64 projection hash per tick. Stored as `.seproj` (text; gzip optional). |
| app/headless/LegacyReplayMain.cpp, app/headless/ScenarioLoader.cpp (new); app/CMakeLists.txt | Exclude `headless/` from `editor_app`'s glob. Add the `legacy_replay` executable (Sandbox root; `--scenario --inputs --ticks --projection-out --fps`, and `--live --record` for a visible recording run). |
| app/main.cpp | argv: `--record`, `--seed` |
| config/config.json | Add `sim-tick-rate: 120` and `seed`. Keys flatten to leaf names (JsonDataStoreSerializer.cpp:124), so they must be unique. |
| tests/legacy/CMakeLists.txt (+ cmake/Legacy.cmake) | CTest `legacy.selfdet.*`: two runs, and runs at `--fps` 30/60/144/160, give identical projection hashes; a live recording replays headless identically. Label `legacy`. |

Exit: same inputs give identical projection hashes across runs and across
`--fps` 30/60/144/160. Tag `legacy-ref-v0`.

### M1b: declared rules, then tag `legacy-ref-v1`

One commit per rule. Each writes its text into docs/rules/gameplay-rules.md,
changes the legacy code and re-baselines the golden projections.

- R1 TEMPTIMED always reverts, including on the watchdog path (EventSystem.cpp:138-139).
- R2 The cooldown is per (source, receiver) pair and decided once per tick; `readyOnStart` is honoured (UpdateEntityPolling.cpp:8-11, 18-24; CollisionSystem.cpp:88).
- R3 Every overlap is found: brute force in SpawnIndex order replaces `QuadTree::queryRange` (QuadTree.cpp:12, 124).
- R4 The collider is the unrotated texture-size AABB at the sprite position (CollisionSystem.cpp:73).
- R5 Seconds throughout: Speed 0.5 px/ms becomes 500 px/s; Movement's 1 px per poll becomes an explicit 120 px/s.
- R6 Zero-length path segments are guarded (WayPointSystem.cpp:42-43).
- R7 OVERTIME's first application comes exactly one interval after contact (`>=` in ticks), applied floor(duration/interval) times (:98).
- R8 Contacts are sorted by (source, receiver) SpawnIndex before effects apply.
- R9 System order is Input → WayPoint → Boundary → Collision → Event → GameRules → Cleanup (Sandbox.cpp:90-92). Boundary is the clamp of every sprite to the scene bounds that M1a moved out of `render()` (Sandbox.cpp:155); it is part of movement, so it runs after WayPoint and before Collision and contacts use the clamped positions. The goldens depend on this order and M3 reproduces it (amended after the M1 review).
- R10 HP ≤ 0 destroys are deferred to the end of the tick (Sandbox.cpp:126-136).

Exit: three scenarios × 3600 ticks with golden projections generated at
`legacy-ref-v1` (tests/fixtures/reference/); ledger entries for the
characterization items and the stress generator's distributions
(Editor.cpp:941, 948, 950).

## M2 Headless kernel (engine/se_core, engine/se_sim; design build step 1)

### se_core

- **thread_role.h**: capabilities `MainThreadRole g_main` and `WorldOwnerRole g_sim_owner` (ADR-007); `enum class ThreadRole { kLibrary, kMain, kSim, kForkWorker, kBackground, kLogWorker, kWatchdog, kInputSampler, kVblankSampler }`; `RegisterMainThread()` (SE_ACQUIRE(g_main)), `IsMainThread()`, `CurrentRole()`; `SE_ASSERT_MAIN_THREAD()`, `SE_ASSERT_NOT_IN_FORK()`; `InlineOwnerScope` (scoped capability: main takes g_sim_owner in inline mode); `void WakeMain() noexcept` declared here, defined by se_platform (tests link a null definition).
- **thread_safety.h**: `SE_CAPABILITY`, `SE_SCOPED_CAPABILITY`, `SE_REQUIRES`, `SE_ACQUIRE`, `SE_RELEASE`, `SE_EXCLUDES`, `SE_NO_THREAD_SAFETY_ANALYSIS` (no-ops off clang).
- **log.h**: `SE_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(fmt, ...)` with std::format checking and compile-time stripping; M2 uses a synchronous stderr backend, M4 swaps in rings without touching call sites.
- **types.h**: `Vec2`, `Rect`, `Rgba8`, `using Tick = std::uint64_t`.
- **guid.h**: `struct Guid { std::uint64_t hi, lo; auto operator<=>(const Guid&) const = default; }`, ToString/Parse; `struct EntityRef { Guid guid; mutable entt::entity cached; entt::entity Resolve(const World&) const; }`.
- **hash.h**: streaming `Hasher` with fixed documented constants; floats hashed by bit pattern with −0 and NaN canonicalised; `struct WorldHash { std::uint64_t sum, xr; std::uint32_t count; Tick tick; std::uint64_t Folded() const; }`; per-entity digest = mix(Guid, hashed component bytes), folded by sum and xor (ADR-011); `--det-crosscheck` dumps per-entity digests sorted by Guid.
- **rng.h**: counter-based `rng::At(seed, tick, Guid, stream, draw)` (SplitMix-style mix), `Unit()` from 24 mantissa bits, `Below(n)` (Lemire), with test vectors.
- **detmath.h/.cc**: `se::dm::Sin`, `Cos`, `SinCos`, `Atan2` for float (computed in double) and double; only + − × ÷ and std::sqrt; Cody–Waite reduction to [−π/4, π/4] with minimax polynomials; atan on [0, 1] with octant fix-up; defined out of line under se_determinism flags; `std::uint64_t SelfTestHash()`. `se::fp::ConfigureProcess()` calls `_set_FMA3_enable(0)` on MSVC x64; `SetFma3ForTest(bool)`.
- **input_frame.h**: `ActionId` (uint8), `kMaxActions = 64`, `kMaxAxes = 8`, `kMaxEdgesPerTick = 32`; `enum class EdgeSource { kRawKeyboard, kWinMessage, kGamepadSampler, kMouse, kReplay }`; `struct InputEdge { ActionId action; bool down; std::uint64_t stamp_ns; std::uint8_t order; EdgeSource source; }`; `struct InputFrame { Tick tick; std::uint64_t held; std::array<std::uint8_t, 64> pressed, released; std::array<float, 8> axes; std::uint8_t edge_count; std::array<InputEdge, 32> edges; bool platform_live; }` (trivially copyable, static_assert); `class InputSource { virtual InputFrame Latch(Tick, std::uint64_t window_end_ns, bool last_tick_of_frame) = 0; }`.
- **sim_clock.h**: `struct SimClock { Tick tick = 0; std::uint32_t rate = 120; double dt; double Time() const; void Advance(); }`, `TicksFromSeconds` (ceil, a written rule).
- **time.h** (pure; no clock reads):
  - `enum class Cadence { kLocked, kFree }`
  - `struct FrameClock { std::uint64_t frame_start_ns, prev_frame_start_ns; double period_sec; std::uint32_t elapsed_vblanks; bool vblanks_known, modal, gpu_fence_pending_at_start; double timer_p99_sec, real_delta; }`
  - `struct FrameCosts { double tick_hi, non_tick_hi, gpu_hi, tick_95, non_tick_95, gpu_95, margin; }` (gpu NaN when unknown)
  - `struct StepPlan { int steps, k_max, k_rt, locked_k; Cadence cadence; float alpha; bool overloaded; std::uint32_t debt_ticks; double dilation_sec, budget_b; std::array<std::uint64_t, 8> tick_window_end_ns; }`
  - `class FixedStepper { StepPlan Plan(const FrameClock&, const FrameCosts&, bool running, bool debugger, bool editor_play) noexcept; StepPlan PlanNext(const FrameClock&, const FrameCosts&) const noexcept; void OnPresentResult(bool missed, bool modal) noexcept; void ResetAccumulator() noexcept; }`
  - Accumulator: integer ns × tick rate (one tick = 1e9 units), so exactly 120 ticks per simulated second.
  - Formulas: T = presentation period (2T in HalfRate); m = max(1.5 ms, timer p99 + 0.5 ms). Locked when |T/dt − k| ≤ 0.005·T/dt (or the same on dt/T); else Free. k_rt: Locked = k (ADR-008), Free = ceil(T/dt). k_hi = floor((T − R_hi − g_hi − m)/S_hi); k_95 the same with p95 costs. Normal k_max = clamp(max(k_rt, k_hi), 1, min(8, k_rt + 1)); Overloaded k_max = clamp(k_95, editor_play ? 0 : 1, k_rt). Unknown g with the previous fence pending at frame start → k_max = k_rt. steps = min(due, k_max); Locked due = k × elapsed vblanks + debt; Free due = acc/dt with deltas snapped to multiples of T within max(1.0 ms, timer p99 + 0.1 ms). Debt the spare rate cannot repay within 0.25 s becomes counted dilation (accumulator re-based keeping its fraction). Overloaded entry: 2 consecutive misses with debt, or k_95 < k_rt for 1 s, never while Modal; exit after 1 s with no misses and k_95 ≥ k_rt. B = min(T, steps·S_hi + R_hi + g_hi + m) plus the miss offset. Stalls > 0.25 s (100 ms with a debugger) count as paused time with no catch-up. alpha = 1 Locked, acc/dt Free. Tick windows: Locked splits [prev_start, start) evenly; Free window_end_i = start − remainder − (steps − 1 − i)·dt.
  - `CostTracker` (p99.5 over 4 s, p95 over 2 s, fixed ring) and `HalfRateController` (above 100 Hz, with hysteresis).
- **component_registry.h**: `struct ComponentInfo { name; version; entt::id_type id; flags (kHashed, kSerialized, kInterpolated, kEditorVisible); precreate; read_json; write_json; read_beve; write_beve; clone; hash; migrate = nullptr; inspect = nullptr; }`; `class ComponentRegistry { template <class T> void Add(name, version, flags); const ComponentInfo* Find(entt::id_type) const; std::span<const ComponentInfo> All() const; void Seal(); }`. glaze is confined to `reflect_glaze.h`, included only by `*_components.cc`.
- **core_components.h/.cc**: IdComponent{Guid}, Name, Transform {pos, rot, scale, teleport_tick}, PrevTransform {pos, rot, scale, tick}, Spawned {spawn_tick, spawn_serial}, DestroyRequest.
- **world.h**: `class World { World(const ComponentRegistry&, WorldDesc); entt::entity Create(Guid) SE_REQUIRES(g_sim_owner); entt::entity Spawn(Guid parent, std::uint32_t index); static Guid DeriveGuid(Guid parent, Tick, std::uint32_t index) noexcept; void RequestDestroy(entt::entity); void DrainDestroyRequests(); void FinalizeSpawns(); entt::registry& Reg() SE_REQUIRES(g_sim_owner); const entt::registry& CReg() const noexcept; std::optional<entt::entity> Find(Guid) const; template <class... C> ParallelView<C...> Parallel(const jobs::ForkSite&) SE_REQUIRES(g_sim_owner); WorldHash Hash() const; MutationGate OpenGate(GateReason); std::thread::id owner() const; void set_owner(std::thread::id); SimClock clock; }`. The constructor pre-creates every registered storage; `Reg()` asserts the owner thread, not-in-fork, and an open MutationGate on Play Worlds; world.cc has `#ifndef ENTT_USE_ATOMIC #error`.
- **jobs.h**: `ChunkRange{chunk, begin, end}`, `ForkStats`, `ForkSite` (name, grain, min_items, cores, reads, writes, enabled = false, inline_until_ns, stats); `inline thread_local bool t_in_fork`; `class ForkJoinPool { struct Config { int workers = 0; int max_workers = -1; std::uint32_t spin_us = 20; std::uint64_t chaos_seed = 0; }; template <class F> void ParallelFor(ForkSite&, std::uint32_t count, F&&) SE_REQUIRES(g_sim_owner); }`. M2 is inline only: chunks = ceil(count/grain) run in order with t_in_fork set; nested forks run inline; a third-party exception from a chunk propagates to the caller's fault boundary with no catch in the pool. (The threaded pool in M14 catches on workers at its fault boundary and re-raises on the owner thread with std::rethrow_exception.)
- **parallel_view.h**: `ParallelView<C...>` captures storage pointers on the owner thread before the fork; `size()`, `ForChunk(ChunkRange, F)`, `Read<T>`, `Write<T>`; debug builds assert T is in the declared sets and that writes stay inside the chunk's range.
- **schedule.h**: `enum class Phase { kInput, kIntent, kMovement, kCollision, kEffects, kGameRules, kCleanup }`; `struct TickContext { const InputFrame& input; const SimClock& clock; jobs::ForkJoinPool& jobs; }`; a `System` concept (`Run(World&, const TickContext&)` plus a static name; no world state in systems); `class Schedule { template <System S> SystemId Add(Phase, S = {}); void Seal(); void Step(World&, const InputFrame&, jobs::ForkJoinPool&) SE_REQUIRES(g_sim_owner); std::span<const SystemStats> stats() const; }`. Step advances the clock, sets the breadcrumb, runs the built-in PrevTransformCopy at the head of Movement, times each system into a 1024-entry ring, and ends with built-in Cleanup (drain destroys, FinalizeSpawns, hash). breadcrumb.h: the packed 64-bit {role:4, phase:4, system:12, chunk:16, tick:28} type and setters.

### se_sim

- **game_module.h**: `struct GameModule { std::string_view name; void (*register_components)(ComponentRegistry&); void (*register_systems)(Schedule&); void (*register_states)(GameStateFactory&); std::string_view start_state; ActionMapDefaults actions; }`.
- **game_state.h**: `GameState` interface (OnEnter, OnPause, OnResume, OnExit, worlds, sim_running); `class GameStateStack { Request(StackOp{kPush, kReplace, kPop}, state_id); ApplyPending(); RunningWorlds(); RenderSources(const StepPlan&); SimRunning(); WorldHash Hash() const; }`: pushdown semantics of core/manager/SceneManager.cpp:40-69 with a FIFO queue, no replace by default, changes applied only at tick boundaries.
- **session_log.h**: `enum class SessionOp { kSetComponent, kAddComponent, kRemoveComponent, kCreateEntity, kDestroyEntity, kSpawnBatch, kAssetSwap, kGameStateCommand, kPause, kResume, kStep, kStop }`; `struct SessionEntry { Tick apply_before; std::uint32_t seq; SessionOp op; Guid target; entt::id_type type; std::vector<std::byte> payload; std::uint64_t content_hash; }`; `class SessionLog { Enqueue; ApplyDue(Tick, GameStateStack&); applied(); Write; Read; }`. While paused, entries apply at once and are logged against the next tick.
- **replay.h**: `ReplayHeader` {build, platform, compiler, cpu_features, math_hash = dm::SelfTestHash(), scene_hash, seed, tick_rate}; `ReplayRecorder` {Begin(header, start_bytes), Record(InputFrame), RecordHash, RecordSession, Finish → BEVE}; `ReplayReader : InputSource`.
- **sim_driver.h**: `class SimDriver { TickResult RunTick(GameStateStack&, const InputFrame&, SessionLog&, ReplayRecorder&, AudioSink&, std::uint64_t window_end_ns); void TickInline(GameStateStack&, const StepPlan&, InputSource&, SessionLog&, ReplayRecorder&, AudioSink&) SE_REQUIRES(g_main); Tick NextTick() const; }`. RunTick = ApplyPending → ApplyDue → Record → Schedule::Step for each running World → audio drain. The M17 threaded driver reuses it unchanged. AudioSink is null in M2.
- **synthetic_clock.h**: `SyntheticFrameClock(double hz, std::uint64_t seed, double jitter_sec)` drives `--fixed-frame-dt`.

### Tools and tests

- tools/se_replay: `--module kernel_test|rampage --scene --inputs --ticks --fixed-frame-dt <ms> --hash-out --projection-out --det-crosscheck K --jobs W`.
- tests/support: `kernel_test_module.cc` (circular movers using dm::Sin/Cos; a spawner using DeriveGuid every K ticks; a GameOver Replace requested from a rule at a fixed tick; logged SetComponent edits at ticks 100 and 250 from a fixture), `virtual_display.h` (cost-model frame simulator with vblanks, misses and presents), `golden.h` (`CheckGolden(name, span)` against tests/golden/<SE_GOLDEN_PLATFORM>/, `SE_UPDATE_GOLDENS=1` rewrites).
- tests/kernel:
  - fixed_stepper_test.cc: cadence at 30, 60, 59.94, 120, 144, 160, 165 Hz; k_rt = 2 at 59.94; N=50k at 60 Hz (S 4.2 ms, R 1.4 ms) with a 25 ms spike gives ≤ 1 hitch per spike while the old 8-step cap reproduces the lock-in; no 3-tick frames at 59.94 Hz Locked over 60 s at N=30k; N=50k at 144 Hz settles in HalfRate; stalls, debugger allowance, dilation, Overloaded entry/exit, editor floor of 0, PlanNext before sleep.
  - detmath_test.cc (error bounds against the CRT, bit-exact goldens, self-test hash), hash_test.cc, rng_test.cc, guid_test.cc (golden DeriveGuid values), world_test.cc, parallel_view_test.cc (chunk boundaries depend only on count and grain), schedule_test.cc, session_log_test.cc, game_state_stack_test.cc (the C6 case: a GameOver requested in a frame's first tick does not let the second tick run in the old World), serialization_test.cc (JSON and BEVE round trips for every registered component), replay_test.cc, determinism_test.cc (per-tick hash sequences identical under 30/60/144/160 Hz synthetic clocks with jitter, including the GameOver and logged edits; golden per platform and compiler; the suite run with FMA3 forced on and off).
  - tests/death: World access in a fork; structural calls through ParallelView; undeclared component access; a Play-World mutation with the gate closed.
  - tests/compile_fail: a chunk lambda calling `world.Create()` (clang-cl preset).

Exit (design build step 1): golden hashes identical under 30/60/144/160 Hz
clocks including a GameOver transition and logged edits; the three stepper
scenarios; FMA3 on/off hashes match; ENTT_USE_ATOMIC defined, storages
pre-created, structural asserts in place; serialization round trips. All
green on msvc-dev, msvc-asan and clangcl-dev, lint included.

## M3 Headless gameplay port (games/rampage, engine/se_render_core, bench/; design build step 1, CF a)

M3 ports the legacy Sandbox's gameplay onto the M2 kernel and proves it
against the legacy reference. It has three parts. M3a changes the reference
(rules R11 to R13) and tags `legacy-ref-v2`. M3b (gameplay) and M3c
(RenderList and se_bench) are new code and are written in parallel with M3a.
The comparison runs once all three are merged. Nothing in M3 opens a window or
links SDL3, ImGui or miniaudio.

### M3a: three more declared rules, then tag `legacy-ref-v2`

The M1 review left three characterization items in docs/ledger.md that are
legacy bugs rather than behaviour worth keeping. They are fixed in the
reference first, one commit per rule, exactly as for R1 to R10:
- each commit writes its rule text into docs/rules/gameplay-rules.md;
- each re-baselines the goldens of tests/fixtures/reference/ (the `.sehash`
  and `.seproj.gz` files; Debug and Release must be identical).

Floating-point expressions are evaluated in `float`, in the order written, so
the engine (M3b) can produce the same bits.

- **R11 The clamp keeps the collider inside the scene bounds**
  (core/system/BoundarySystem.cpp).
  - Inputs: the collider's half extents `hw = texture width / 2.f` and
    `hh = texture height / 2.f` (R4: unrotated, centred on the position), and
    the bounds `W`, `H` (the configured resolution, as floats).
  - `x_new = W < 2.f * hw ? W * 0.5f : std::clamp(x, hw, W - hw)`, and the
    same for y.
  - Both axes are computed from the position before the clamp and written
    once.

  Before: the clamp used the size of the rotated global bounds against the
  centred position. It let half a sprite out on the left and top but kept a
  whole sprite's margin on the right and bottom. Each axis's `setPosition`
  also reused the pre-clamp position, so a corner correction on one axis was
  undone by the other, and sprites could leave the arena.
- **R12 Path steps follow the polyline** (core/system/WayPointSystem.cpp).

  State: the current segment `i` (from point `i` to the next) and the
  distance `u` travelled along it (float). The entity starts at its first
  point, with `u = 0` on segment 0.

  Each tick, if the entity has a Speed stat (without one it does not move, as
  before), set `rem = speed * dt` with `dt = 1.f / 120.f`, then loop:
  1. Skip segments of length 0 (R6).
  2. If no segment is left (the end of a non-repeating path), place the
     entity exactly on the last point and stop moving for good.
  3. If `u + rem < len`, set `u = u + rem` and leave the loop.
  4. Otherwise set `rem = (u + rem) - len` and `u = 0.f`, and continue with
     the next segment.

  - Position: `a + (b - a) * (u / len)` per axis, where `a` and `b` are the
    segment's points and `len = std::sqrt(dx * dx + dy * dy)` is precomputed
    per segment.
  - Heading: the segment's `atan2(dy, dx) * (180.f / pi) + 90.f` degrees.
  - An entity is "moved" (UpdateEntityEvent, the dirty flag of R2 and R8) on
    every tick its position changes.

  Before:
  - the tick's whole step followed the current segment even past its end;
  - the next segment began only once the travelled distance passed the next
    waypoint's running total, so corners were overshot by up to one step;
  - the position was integrated from per-tick increments, so it drifted from
    the path.
- **R13 Repeating paths close the loop** (WayPointSystem.cpp:27-34). A
  repeating path with two or more distinct points continues from its last
  point back to its first along a closing segment, then starts over at
  segment 0. The entity therefore cycles the closed polygon, and a two-point
  path goes back and forth. A path whose points all coincide never moves.
  Before: at the last waypoint the progress reset while the sprite kept its
  position, so the path replayed offset from its end point.

Update these ledger rows to "re-specified (M3a)":
- "Clamp corrections read the position from before the first correction";
- "A path step is taken in full past the waypoint";
- "Repeating paths restart progress, not position".

The editor's path drawing (app/editor) is unchanged.

Exit:
- three scenarios × 3600 ticks re-baselined;
- self-determinism tests green;
- `editor_app` runs;
- tag `legacy-ref-v2` (the orchestrator creates the tag).

### M3b: game_rampage (games/rampage; Google style; DETERMINISTIC)

`game_rampage` is a STATIC module built with se_add_module:
- FORBID SDL3::SDL3-static, se_imgui, miniaudio, sentry and lz4_static;
  platform includes are banned;
- depends on se_core, se_sim and se_render_core (for SpriteRenderer only).

The `rampage` executable comes in M13; in M3 the module runs through
se_replay.

**Components.** They are declared in game_rampage/components.h and
registered in rampage_components.cc through reflect_glaze.h. All are plain
aggregates, kHashed and kSerialized unless noted, and each gets a JSON and a
BEVE round-trip test.

- `Team { TeamId id; }`, with `enum class TeamId : std::uint8_t
  { kUnaffiliated, kFriendly, kEnemy }`.
- `Stats { std::vector<StatValue> values; }`, with
  `StatValue { StatId id; float value; }`.
  - `StatId` is `Fnv1a32(name)`, an interned id. A `StatNames` table in the
    World's ctx maps ids back to names for the projection and, later, the
    editor.
  - `values` is kept sorted by id.
  - A missing stat reads as absent, not 0, except where a rule says
    otherwise (R10).
- `EffectDef { EffectKind kind; StatId stat; float value;
  std::int64_t duration_ms; std::int64_t interval_ms; }`, with
  `enum class EffectKind { kInstant, kOverTime, kTempTimed }`. Tick counts
  derive as `TicksFromMs(ms) = ceil(ms * 120 / 1000)`, in integer arithmetic.
- `EffectSource { std::vector<EffectDef> effects; bool polled;
  std::int64_t cooldown_ms; bool ready_on_start; }`. A polled source is the
  legacy UpdateEntityPolling. Otherwise the source is event-driven and acts
  on the ticks it moved (R2).
- `PairCooldowns { std::vector<PairCooldown> pairs; }` on polled sources,
  with `PairCooldown { Guid receiver; Tick last; }`, sorted by receiver Guid.
- `ActiveEffects { std::vector<ActiveEffect> effects; }` on the receiver,
  with `ActiveEffect { EffectDef def; EntityRef source; Tick start;
  Tick next; std::uint32_t applied; std::uint64_t seq; }`.
  - Effects apply in `seq` (creation) order (R8).
  - The event watchdog (scene setting `event_watchdog_ms`) ends an effect
    early (R1, R7).
- `Collider { Vec2 half_extents; ColliderRole role; std::uint32_t layer;
  std::uint32_t mask; }`, with `enum class ColliderRole : std::uint8_t
  { kBody, kHitbox, kHurtbox, kTrigger }`. M3 uses one AABB per entity (R4);
  the shape list arrives with tiles (M19).
- `KinematicBody { Vec2 velocity; }`, `MoveIntent { Vec2 dir; }` and
  `PlayerControl { std::uint8_t player; float speed; }`, with speed 120 px/s
  (R5).
- `Path { std::vector<Vec2> points; bool loop; std::uint32_t segment;
  float u; bool done; }` (R6, R12, R13). Per-segment lengths are cached in a
  non-hashed derived component that is rebuilt on load.
- `Moved {}`, not serialized. Movement sets it on ticks the position changed,
  and Cleanup removes it. It is R2's condition for an event-driven source.
- Scene settings live in the World's ctx: `ArenaBounds { float w, h; }`,
  `EffectSettings { std::int64_t event_watchdog_ms; }`, and the seed.

**Systems.** They are stateless structs registered by the GameModule. One
Schedule is shared by se_replay and the tests, and later by the runtime and
the editor. The phases reproduce R9's order:

| Phase | Systems |
|---|---|
| Input | `PlayerInputSystem`: InputFrame held bits → MoveIntent. The actions Up, Left, Down, Right follow LegacyAction order (bit i = action i), the same mapping se_replay uses for `.seinput` |
| Intent | `IntentSystem`: MoveIntent × PlayerControl.speed → KinematicBody.velocity |
| Movement | The built-in PrevTransformCopy runs first. Then `KinematicMoveSystem` (the player: `pos += velocity * dt`, in the float order of the legacy Movement::execute), `PathSystem` (R12, R13, R6) and `BoundarySystem` (R11) |
| Collision | `CollisionSystem`, in order: <ul><li>cached world AABBs;</li><li>a spatial-hash broadphase (64 px cells; its scratch lives in ctx and is cleared and reused, so steady-state ticks do not allocate);</li><li>an AABB narrow phase that needs a positive-area overlap (R3);</li><li>self and same-team skips;</li><li>contacts sorted by (source, receiver) spawn serial (R8);</li><li>the per-pair cooldown, decided once per tick (R2);</li><li>one ActiveEffect per effect, in list order.</li></ul> |
| Effects | `EffectSystem`: <ul><li>INSTANT applies once;</li><li>OVERTIME applies floor(d/i) times, starting one interval after contact, compared with `>=` in ticks (R7);</li><li>TEMPTIMED applies once and is always reverted, at its end or by the watchdog (R1);</li><li>an effect is dropped with its receiver.</li></ul> |
| GameRules | `DeathRule`: HP ≤ 0 → DestroyRequest. For entities with Stats, a missing HP counts as 0 (R10) |
| Cleanup | The built-in drain in spawn-serial order (R10), plus removal of Moved |

The spawn serial (Spawned.spawn_serial) plays the role of the legacy
SpawnIndex: the player comes first, then the entities in file order. Systems
use se::dm for every transcendental; std::sqrt is allowed, since it is
correctly rounded.

**Content and loading.**

- Scene and prefabs:
  - `assets/scenes/sandbox.scene.json` holds the legacy Sandbox::init
    content;
  - `assets/prefabs/player.prefab.json` and
    `assets/prefabs/enemy.prefab.json` hold the player and the stress
    generator's archetype (app/editor/Editor.cpp:966-980).
- The format is `se-scene-v1`: `{format, settings {bounds, seed,
  event_watchdog_ms}, entities [{guid, name, prefab?, components
  {<registry name>: <json>}, overrides {<registry name>: <RFC 7386 merge
  patch>}}]}`.
  - Arrays are replaced whole.
  - There are no nested prefabs.
  - `LoadScene` returns `std::expected<..., Error>`. It uses only the
    ComponentRegistry's JSON hooks, so glaze stays confined.
- `LoadLegacyScenario(path)` reads `se-legacy-scenario-v1`
  (app/headless/ScenarioLoader.hpp) into the same World shape.
  - Collider sizes come from the PNG IHDR chunk (24 bytes) of the texture
    named in config/texture.json; no image decoder is used before M7.
  - Guids derive from the scenario name and the spawn index.
- `WriteProjection(const World&, std::string&)` emits a tick's block in the
  exact `.seproj` format of core/util/SemanticProjection.cpp, so the two
  trees' projections compare as text. Per entity:
  - the spawn serial;
  - x and y;
  - the rotation in degrees, normalised to [0, 360);
  - HP, then the other stats sorted by name;
  - values printed with `%.9g`, and `-` when absent.
- se_replay:
  - `--module rampage` with `--scene FILE` or `--legacy-scenario FILE`;
  - `--inputs X.seinput`;
  - `--projection-out` in the seproj format, and `--hash-out` as for the
    kernel module.

  This replaces the M2 test `replay.rampage_not_yet`.
- `tools/refcmp/refcmp.py` (Python 3, stdlib only) reads `.seproj` or
  `.seproj.gz`. For each tick it requires:
  - the same set of live spawn serials;
  - every stat value equal as text (exact float equality);
  - |Δx| and |Δy| ≤ 1e-3 px;
  - the rotation within 1e-3 degrees, modulo 360.

  It reports the first difference (the tick, the entity and both lines),
  then a summary of the largest deviations.

**Tests.** They live in tests/rampage with the label `rampage`, registered
from tests/rampage/CMakeLists.txt, which tests/CMakeLists.txt adds with one
line.

- broadphase_test: the spatial hash finds exactly the brute-force pair set
  over seeded random worlds. The worlds cover:
  - varied sizes and clusters;
  - boxes that straddle or touch cell edges;
  - boxes that touch without overlapping;
  - entities far outside the arena.
- effects_test covers:
  - R1;
  - R2: both readyOnStart values, two receivers of one source, and the
    same-team and self skips;
  - R7: the 250/1100 and 1000/5000 cases, interval 0, and a duration shorter
    than the interval;
  - the watchdog paths, and a receiver destroyed mid-effect;
  - R8 order with non-integer sums.
- path_test covers:
  - R6;
  - R12: several waypoints in one step, and the stop at the end with the
    exact end position;
  - R13: the closing segment, two points, and coincident points.
- boundary_test covers R11: corners, and boxes larger than the bounds.
- scene_test: the sandbox scene and the prefabs load; merge patches override
  values and replace arrays whole; saving and reloading the World gives the
  same hash.
- determinism: the sandbox under 30, 60, 144 and 160 Hz synthetic clocks
  gives the same per-tick hashes, checked against a per-platform golden
  `determinism_rampage`.
- `ref.sandbox.{stress,effects,patrol}` (label `reference`): se_replay runs
  the legacy scenario and inputs for 3600 ticks, and refcmp checks the result
  against tests/fixtures/reference/NAME.seproj.gz at `legacy-ref-v2`.

### M3c: se_render_core and se_bench (engine/se_render_core, bench/)

`se_render_core` is a STATIC, headless module (no SDL, no GPU, no ImGui) that
depends on se_core. Rendering never writes simulation state.

- Two components, registered by
  `RegisterRenderComponents(ComponentRegistry&)`, serialized but not hashed:
  - `SpriteRenderer { std::uint32_t texture; Rect region; Vec2 origin;
    Rgba8 tint; std::int16_t layer; float z; bool flip_x; bool flip_y; }`,
    where `texture` is an interned asset key until M9's handles;
  - `Camera2D { Vec2 center; float zoom; Vec2 virtual_size;
    bool integer_scale; bool primary; }`.
- `void BuildRenderList(const World&, const Camera2D&, float alpha,
  RenderList&)`:
  - interpolates PrevTransform → Transform by alpha: position and scale
    linearly, rotation along the shorter arc;
  - does not interpolate when `teleport_tick` is the World's current tick or
    PrevTransform is from an older tick;
  - culls each sprite's rotated bounds against the camera rect;
  - sorts by (layer, texture, z, spawn serial);
  - fills `RenderList { std::vector<SpriteQuad> quads;
    std::vector<QuadBatch> batches; RenderStats stats; }`, with
    `SpriteQuad { std::array<Vec2, 4> pos; std::array<Vec2, 4> uv;
    Rgba8 tint; }` and `QuadBatch { std::uint32_t texture;
    std::uint32_t first; std::uint32_t count; }`;
  - reuses the list's buffers across frames, so nothing is allocated once
    warm;
  - computes rotation with se::dm::SinCos.
- tests/render (label `render`): interpolation at alpha 0, 0.5 and 1;
  teleports; the shorter arc across ±180°; culling at the camera edges; sort
  stability; batch runs; flips and the origin.
- `bench/se_bench` (EXECUTABLE, SE_BUILD_BENCH):
  - command line: `se_bench --suite NAME [--n N] [--iterations K]
    [--json FILE]`;
  - reports the median, p95, p99 and max of each suite;
  - thresholds for the 7700X live in bench/thresholds.json;
  - CTest `bench.*` (label `bench`) runs only in the Release presets.

  Suites:
  - `renderlist_10k`: 10k sprites over 8 textures with random poses. This is
    the exit gate: p95 ≤ 1 ms in Release.
  - `rampage_tick_5k`, added once M3b is merged: one tick of 5k moving
    colliders on paths. Its threshold is the measured p95 with 2× headroom;
    the design's budget is 1/160 s.

### Exit (M3)

- The broadphase matches brute force, and the effect, path, boundary and
  interpolation tests pass.
- `ref.sandbox.*`, against `legacy-ref-v2`, over 3600 ticks of all three
  scenarios:
  - liveness and stats are exact;
  - positions are within 1e-3 px;
  - rotations are within 1e-3 degrees.
- RenderList of 10k sprites: p95 ≤ 1 ms in Release.
- Green on:
  - msvc-dev, msvc-asan and clangcl-dev, lint included;
  - msvc-debug and msvc-release;
  - the legacy presets;
  - WSL linux-clang-debug, -asan and -tsan, with linux-clang goldens for
    every new golden.

## M4 Thread roles, contract checks and crash capture (design build step 2)

Design sources: docs/design/mt-design.md "Threads" (log worker, watchdog,
library-owned threads), "Determinism And Debugging" (crash and hang capture,
tooling), "Failure Handling", "Platform" (DbgHelp, QoS, spdlog), the API
sketch, and build step 2. M2 already provides `thread_role.h` (g_main,
g_sim_owner, ThreadRole, RegisterMainThread, SetCurrentRole), `breadcrumb.h`
(the packed word), the `SE_LOG_*` front end (`log.h`) and the schedule's
per-system breadcrumbs. M4 builds the rest without touching log call sites.

M4 runs as two tracks, M4a (threads and logs) and M4b (crash capture), plus
the integration step that wires them to each other and to M5.

### Dump stream layout (the contract between M4a and M4b)

Every memory region registered as a dump stream starts with this header and
is followed by `capacity` records of `record_size` bytes, all in one
contiguous allocation that is never freed. Little-endian, natural alignment.
The producer's live counters are the header fields themselves, so a dump
shows the real state. M4a defines it in `se_core/dump_stream.h`; M4b's probe
and reader use the same layout (a local copy until integration).

```
struct DumpStreamHeader {      // 64 bytes
  uint32_t magic;              // 0x31304553, "SE01"
  uint16_t version;            // 1
  uint16_t kind;               // 1 breadcrumbs, 2 log ring, 3 InputFrame
                               // ring, 4 SessionLog tail, 5 flight recorder
  uint32_t record_size;
  uint32_t capacity;
  uint64_t head;               // records written so far; the newest is
                               // record (head - 1) % capacity
  uint64_t dropped;
  uint64_t os_thread_id;       // the producer, 0 when shared
  uint8_t  role;               // ThreadRole of the producer
  char     name[23];           // NUL-padded
};
struct BreadcrumbRecord {      // kind 1, one per ThreadRegistry slot, 64
                               // bytes (one cache line, 64-aligned)
  uint64_t packed;             // the thread's live Breadcrumb word
  uint64_t os_thread_id;
  uint8_t  role;
  uint8_t  state;              // 0 free, 1 live, 2 exited
  char     name[46];
};
struct LogRecord {             // kind 2, 512 bytes
  uint64_t timestamp_ns;       // steady clock
  uint64_t sequence;           // per process, for merging
  uint32_t tick;
  uint32_t line;
  uint8_t  level;
  uint8_t  role;
  uint16_t length;             // message bytes used
  char     file[36];           // basename, NUL-padded
  char     message[448];       // not NUL-terminated; kLogMaxMessage = 448
};
struct InputFrameSlot {        // kind 3
  uint64_t stamp;              // seqlock: odd while being written
  InputFrame frame;            // carries its tick
};
```

### M4a: threads, rings, the log worker, Tracy (engine/se_core, engine/se_os, engine/se_log)

**New module se_os** (STATIC; not DETERMINISTIC; may include `<windows.h>` and
POSIX headers; FORBID SDL3::SDL3-static se_imgui miniaudio sentry lz4_static;
link-graph ALLOW_ONLY se_core). The headless OS layer that threads need on
every platform, including the SDL-free WSL TSan build (ADR-029). It provides:
- `thread.h`: `ThreadSpec{ThreadRole role; std::string_view name; Qos qos;
  CpuSetRequest cpus;}`, `enum class Qos {kDefault, kHigh, kEco}`,
  `StartThread(const ThreadSpec&, F&& body) -> std::expected<Thread, Error>`
  (a `std::jthread` whose body runs inside a RoleScope; `Thread` joins in its
  destructor and exposes `RequestStop()` / `StopToken()`), and `RoleScope`
  for threads the engine did not start through StartThread (the main thread):
  - sets the OS thread name (SetThreadDescription; pthread_setname_np, 15
    characters), the role (SetCurrentRole), the breadcrumb role;
  - takes a ThreadRegistry slot (below);
  - applies QoS: on Windows SetThreadInformation(ThreadPowerThrottling):
    kHigh = throttling explicitly off, kEco = on, kDefault = no override; on
    Linux kEco lowers the nice value of the thread by 5 only if permitted
    (failure is logged once, never an error);
  - applies the CPU set (SetThreadSelectedCpuSets; pthread_setaffinity_np)
    only when one is requested;
  - names the thread for Tracy through the profile hook (below);
  - restores what it changed in its destructor.
- `cpu_topology.h`: `CpuTopology{logical, physical cores, per core:
  efficiency class, last-level-cache id, SMT sibling ids}` read with
  GetSystemCpuSetInformation (Windows) or /sys/devices/system/cpu (Linux), and
  pure functions over it: `ForkPoolSize(topology)` = physical cores - 2,
  homogeneous CPUs with 8 or more cores - 1, capped at 15, 0 below 3 physical
  cores; `PerformanceOnlyCount(topology)` = P-cores on main's LLC - 1; the CPU
  sets for main, workers and samplers (SMT siblings excluded). Tested with
  synthetic topologies for the 7700X (8C/16T), a 4-core laptop, 2P+8E and
  6P+8E: 7/2/8/12 and 7/2/1/5 as in the design's thread table.
- `unbiased_clock.h`: `UnbiasedNowNs()` on QueryUnbiasedInterruptTimePrecise
  (Windows) or CLOCK_MONOTONIC (Linux); both exclude system sleep.
- `debugger.h`: `DebuggerAttached()` (IsDebuggerPresent; TracerPid in
  /proc/self/status).
- `process.h`: `ExecutableDir()` (GetModuleFileNameW; /proc/self/exe), needed
  before SDL is initialised (the crash handler starts first).

**se_core additions** (header-only where possible; std atomics only):
- `spsc_ring.h`: `SpscRing<T>` for trivially copyable T, capacity a power of
  two fixed at construction, one producer and one consumer, no allocation
  after construction. `TryPush` fails when full and the caller counts the
  drop; `TryPop`/`PopAll`. Cache-line separated head and tail.
- `thread_registry.h`: a fixed table (64 slots) of per-thread records: role,
  name (16 chars), OS thread id, breadcrumb, log ring, state (free, live,
  exited). The table is one kind-1 dump stream, and each slot's Breadcrumb
  is the `packed` word of its BreadcrumbRecord. A slot is taken at RoleScope
  construction and released at its end; its log ring is allocated when the
  slot is first used and never freed, so a crash dump can always read it. Registration takes a
  mutex (thread start and exit only); readers (the log worker, dump code)
  read published slots lock-free. The main thread's slot is slot 0.
- `dump_stream.h`: the layout above.
- `log_ring.h`: `LogRing`, one thread's SPSC ring of `LogRecord` laid out as
  a dump stream (header.head is the producer index, header.dropped the drop
  count). The last records
  stay in the ring's memory after the worker consumes them (the crash tail).
  Capacities: 4096 records on main, 1024 on every other thread; one shared
  mutex-guarded ring for threads the engine did not start (library threads,
  SDL's log callback), never used by main.
- `log.cc`: the front end writes each record into the calling thread's ring
  (thread-local pointer set by RoleScope; the shared library ring
  otherwise) and wakes the worker if it is parked. Without a running worker
  (tests, tools, before boot finishes, after shutdown) it writes
  synchronously as today, so every existing test keeps its behaviour.
  `SE_LOG_FATAL` writes the record, flushes synchronously to stderr and the
  file sink, then aborts (the crash handler captures the rings).
- `input_frame_ring.h`: `InputFrameRing`, a kind-3 dump stream of 7200
  InputFrameSlots (60 s at 120 Hz) with a seqlock stamp per slot and the
  header's head as the published head. Written
  only by the World owner (SimDriver writes every latched frame when a ring
  is attached; nothing changes when none is); read by the dump code and by a
  test reader thread. The reader detects torn slots by the stamp.
- `profile.h`: `SE_PROFILE_ZONE(name)`, `SE_PROFILE_FRAME()`,
  `SE_PROFILE_FRAME_NAMED(name)`, `SE_PROFILE_PLOT(name, value)`,
  `SE_PROFILE_MESSAGE(text)`, `SE_PROFILE_THREAD_NAME(name)`. They compile to
  nothing unless `SE_TRACY` is on. With it on they use Tracy (TracyClient
  linked from se_core; ADR-029 adds Tracy to se_core's allowed list), and
  every one is inert until a runtime app starts the profiler (`--profile`):
  tests, tools and benches never open a socket or start a Tracy thread. Pick
  the Tracy defines that give this (for example TRACY_DELAYED_INIT and
  TRACY_MANUAL_LIFETIME with a started flag checked by every macro), keep
  TRACY_ON_DEMAND, TRACY_ONLY_LOCALHOST, TRACY_NO_BROADCAST, and set
  TRACY_DBGHELP_LOCK. Instrument: a zone per schedule system and per fork,
  FrameMarkNamed("tick") per tick in SimDriver, plots of ticks per frame,
  k_max, debt and dilation in the stepper's caller. `SE_TRACY` is on in
  msvc-dev, msvc-release and clangcl-dev and off in the debug, sanitizer and
  Linux presets.

**New module se_log** (STATIC; deps se_core, se_os, spdlog; ALLOW_ONLY those;
the only module that includes spdlog, enforced by a banned-include rule).
`LogWorker`:
- `Start(LogConfig) -> std::expected<LogWorker, Error>` configures the spdlog
  sinks before any other engine thread exists: a file sink (path from the
  config; rotating, 5 MB x 3), stderr at a configurable level, and an
  in-memory panel ring (last 2000 formatted lines, for M10's log panel, read
  by main with try_lock). The sinks are used synchronously on the worker only.
- Runs as role kLogWorker, default QoS. Wakes on a binary_semaphore released
  by the first record pushed after it parks, or every 50 ms.
- Each wake drains every registered ring and the library ring, merges the
  records by (timestamp, sequence) and writes them. A full ring drops the new
  record and counts it; the worker logs "N records dropped on <thread>" once
  per wake when the count grew. A call site (file, line) that logs more than
  50 records per second is cut to a summary line per second.
- `Stop()` drains everything, flushes, and calls spdlog::shutdown() (which on
  Windows must happen before main returns). The front end falls back to the
  synchronous path from then on.
- Line format: time, level, role, tick, file:line, message, for example
  `12.345678 I main t=1440 schedule.cc:88 loaded 17 entities`.

**Checks** (tools/checks, each with a planted violation and a CTest
`checks.planted.*`):
- raw thread creation (`std::thread`, `std::jthread`, `CreateThread`,
  `_beginthreadex`, `pthread_create`) outside se_os: engine threads come from
  StartThread;
- `DebugBreak`, `__debugbreak`, `__builtin_trap` and in-process dump writing
  (`MiniDumpWriteDump`, `dbghelp.h`) anywhere in engine code;
- `timeBeginPeriod`/`timeEndPeriod` anywhere;
- spdlog includes outside se_log; `<windows.h>` outside se_os, se_crash,
  se_platform and se_render (extend the include scan).

**Tests** (tests/os and tests/kernel additions; label unit unless stated):
SpscRing property tests (two threads, a million items, each delivered exactly
once and in order; full ring drops and counts) run under TSan; ThreadRegistry
(slots reused, rings never freed, names and roles read back through
GetThreadDescription / pthread_getname_np); RoleScope QoS and CPU-set calls
succeed or fail softly; LogWorker (records from 4 threads come out in
timestamp order, drop counts, rate limiting, Stop drains everything, the
synchronous fallback before Start and after Stop, SE_LOG_FATAL flushes before
abort as a death test); InputFrameRing (a reader thread never accepts a torn
slot, under TSan); topology tables; Tracy inert in tests (the started flag
stays false through the whole test run).

### M4b: crash capture (engine/se_crash, tools/minidump)

**New module se_crash** (STATIC; deps se_core, se_os, and the crashpad client
from the pinned sentry-native archive built with SENTRY_BACKEND=crashpad and
SENTRY_TRANSPORT=none; ALLOW_ONLY those). Nothing is ever uploaded: no DSN,
no URL, uploads disabled in the crashpad database settings. Prefer crashpad's
client API directly (CrashpadClient, CrashpadInfo,
CrashReportDatabase) over sentry_init, because registered streams need
CrashpadInfo::AddUserDataMinidumpStream, and record the choice in an ADR. The
handler executable (crashpad_handler) is copied next to every executable that
starts crash capture.
- `Start(CrashConfig{database dir, handler path, annotations}) ->
  std::expected<CrashGuard, Error>` starts the out-of-process handler. It is
  the first boot step, before logging and SDL. Failure is an Error, never
  fatal: the app runs without crash capture and logs why.
- `RegisterStream(StreamType, const void* data, std::size_t size)`: a memory
  region the handler reads at dump time, so dumps show live contents.
  StreamType values are in a first-party range (0x53450001 and up, "SE").
  The region must stay valid for the process lifetime (the thread registry's
  rings and breadcrumbs are never freed for this reason).
- `DumpWithoutCrash(std::string_view reason)`: callable from any thread; the
  handler writes the dump out of process (the watchdog's hang dumps and the
  callbacks' fault path use it). Returns after the handler has the dump.
- No DbgHelp call in process, no in-process MiniDumpWriteDump, never
  DebugBreak.
- Fatal faults (access violation, abort, std::terminate, a stack overflow on
  Windows) reach crashpad's handler. Install a terminate handler that records
  the reason in an annotation first.
- Each stream has a fixed header (magic, version, record size, count, head)
  so a reader can decode it without the build: breadcrumb table, one log ring
  per thread slot, the InputFrame ring, the SessionLog tail and the pacing
  flight recorder (the last two are registered by later milestones; define
  the stream types now).

**tools/minidump/read_dump.py** (stdlib only): parses MINIDUMP_HEADER and the
stream directory, lists threads and the exception record, and decodes the
first-party streams (breadcrumbs with role, phase, system, chunk and tick;
log records as text; InputFrame ring slots with their stamps). It is both a
diagnostic tool and the test oracle.

**Tests** (label crash; Windows, and Linux when crashpad runs in WSL; excluded
from the sanitizer presets if the sanitizer runtime and crashpad's handler
conflict, recorded in gates.md):
- a probe executable `se_crash_probe` registers streams with known contents
  (a breadcrumb table, a log ring with known records, an InputFrame ring with
  known frames), then `--crash=access-violation|abort|terminate` or
  `--dump-without-crash` (from a second thread, as the watchdog will);
- a CTest driver runs it with a fresh database directory, waits for the
  handler, finds the new dump, runs read_dump.py and checks: the exception
  record (for crashes), the probe's threads, and every registered stream's
  decoded contents;
- the handler failing to start (missing executable) gives an Error and the
  probe still runs;
- planted: a file calling MiniDumpWriteDump fails the banned-API check.

### Integration (M4a + M4b + M5)

- Register the real streams: the ThreadRegistry's breadcrumb table and every
  log ring (at slot allocation), and the InputFrame ring the App attaches to
  SimDriver.
- `Watchdog` skeleton (se_crash; role kWatchdog; started by the App in boot
  order; a binary_semaphore with a 1 s timeout on the unbiased clock; joined
  last). M4 gives it `RequestDump(reason)` (one dump per request, from the
  watchdog thread) and reads main's iterate counter and the Suspended, Modal
  and heartbeat flags into a pure `HangDetector` that M12 finishes (M4 only
  needs its interface and the boot budget of 30 s; hang declaration stays off
  until M12).
- The M4 exit tests run against the real rings: a crash test in the rampage
  executable (`--crash-test=after-ticks:240`) and a watchdog-requested dump
  (`--dump-test=after-ticks:240`), both decoded with read_dump.py, show main's
  breadcrumb at the tick, the log tail including a line logged just before,
  and the InputFrame ring up to that tick.

### Exit (M4)
- CI fails on each planted violation (one test per new rule).
- A crash test and a watchdog-requested hang dump, both written out of
  process, show breadcrumbs, log rings and the InputFrame ring (Windows; Linux
  in WSL, or a gates.md entry naming what blocks it).
- SpscRing, InputFrameRing and LogWorker tests green under TSan.
- Tracy builds with SE_TRACY on and is inert in every test.
- Every existing test passes unchanged; no log call site changed.

## M5 Platform loop on SDL main callbacks (design build step 3)

Design sources: docs/design/mt-design.md "Threads" (main), "Data Flow"
(events, the inbox), "Lifecycle" (Booting, Modal, FocusLost, Idle, Suspended,
QuitPending, ShuttingDown, Exited), "Shutdown Order", "Failure Handling",
"Platform", the API sketch (app.cpp) and build step 3; docs/design/
costfree-end-state.md "Runtime and main loop". The SDL presentation patch
and the FramePacer are M6; M5 presents with SDL's VSYNC present mode
("Standard" without the waitable) and plans ticks from SDL's display mode.

**New module se_platform** (STATIC; deps se_core, SDL3::SDL3-static; FORBID
se_imgui miniaudio sentry lz4_static; may include `<windows.h>`). Every
wrapper that calls an SDL video, event or window function asserts the main
thread; `InitPlatform` cross-checks RegisterMainThread() against
SDL_IsMainThread().
- `sdl_context.h`: SDL_Init(VIDEO | GAMEPAD) with
  SDL_HINT_AUTO_UPDATE_JOYSTICKS=0, SDL_SetLogOutputFunction routed to the
  SE_LOG front end (any thread), RAII SDL_QuitSubSystem.
- `inbox.h`: the event filter and `Inbox`. The filter runs on the pushing
  thread before SDL's watchers; off main, the six immediate-dispatch event
  types (TERMINATING, LOW_MEMORY, WILL_ENTER_BACKGROUND,
  DID_ENTER_BACKGROUND, WILL_ENTER_FOREGROUND, DID_ENTER_FOREGROUND) are
  copied into a mutex-guarded vector and dropped (return false), so SDL
  never drains its queue on that thread. Main swaps the vector at iterate
  start with try_lock (a failed try_lock leaves it for the next iterate) and
  dispatches with no lock held, never calling SDL under the inbox lock. Lock
  order: SDL's watch lock, then the inbox lock. Diverted events are counted.
- `wake.h`: the real `se::WakeMain()` (replacing the null definition outside
  tests): pushes one registered SDL user event, coalesced through an atomic
  flag so at most one is pending. It is the only event engine code pushes.
- `window.h`: `Window` (create from a WindowConfig: title, size, flags;
  `activate_when_shown` false for tests so test windows never take focus),
  its native handle, and on Windows a SetWindowSubclass procedure on main
  that sets and clears an atomic Modal flag on WM_ENTERSIZEMOVE /
  WM_EXITSIZEMOVE and WM_ENTERMENULOOP / WM_EXITMENULOOP, calls a session-end
  hook synchronously on WM_QUERYENDSESSION (and WM_ENDSESSION), and reports
  WM_POWERBROADCAST suspend and resume. The subclass is removed before the
  window is destroyed. Linux: no subclass; Modal is never set.
- `fs.h`: `se::fs` on SDL's filesystem API (no `<filesystem>`): base path
  (content root), pref path, path join and normalisation, directory listing
  (SDL_EnumerateDirectory or SDL_GlobDirectory), whole-file read and write
  through SDL_IOStream, rename (SDL_RenamePath), remove, and
  `WriteFileAtomic(path, bytes)` (unique temp name in the same directory,
  flush, rename) for M9's lane and M10's recovery files. Errors are
  std::expected with the path.

**New module se_render (M5 part: device and the clear-only frame)** (STATIC;
deps se_core, se_platform, se_render_core, SDL3). M7 grows it into
Renderer2D.
- `gpu_device.h`: SDL_CreateGPUDevice with DXIL and SPIR-V shader formats,
  debug mode in Debug and Dev, driver "direct3d12" on Windows and "vulkan" on
  Linux by default, `--gpu-driver` / SE_GPU_DRIVER to override; claims the
  window with SDL_GPU_PRESENTMODE_VSYNC; asserts main in every call.
- `frame_recorder.h`: the frame transaction, clear-only. Everything that can
  fail or throw runs before the acquire (`Prepare`); then
  SDL_WaitForGPUSwapchain and SDL_AcquireGPUSwapchainTexture (noexcept);
  recording is noexcept (one render pass that clears to a colour); the
  destructor always submits and never cancels after an acquire. A NULL
  swapchain texture skips the present and is never a state change. A test
  hook forces NULL acquires for N frames.

**New module se_runtime** (STATIC; deps se_core, se_sim, se_os, se_log,
se_crash, se_platform, se_render). The integration step adds the se_os,
se_log and se_crash parts (RoleScope for main, log worker, crash handler,
watchdog); the M5 track leaves documented hooks for them.
- `app.h`: `App::Create(const AppConfig&, GameFactory) ->
  std::expected<std::unique_ptr<App>, Error>`. Init steps in the design's
  Booting order (crash handler, logging, SDL, event filter, content root and
  pref path, window and subclass, GPU device and claim, CPU topology, helper
  threads, GameModule registration and storages); each step's RAII undoes
  it. `AppConfig` carries the command line: `--frames N` (exit after N
  presented frames), `--fixed-frame-dt`, `--gpu-driver`, `--sim-driver`
  (reserved for M17; only `inline` accepted), `--profile`, and in test
  builds `--fail-init-step K` and the throw/NULL-acquire injections.
- `sdl_app.h` and `SE_DEFINE_SDL_APP(factory)`: the four SDL callbacks for an
  executable that defines SDL_MAIN_USE_CALLBACKS, in a file marked
  `// se-fault-boundary`. All four are noexcept with a catch-all. AppInit
  sets *appstate only after a complete init; AppQuit accepts NULL (then only
  logging shuts down). A fault in a callback requests a dump (hook), shows a
  message box (suppressed with SE_NO_MESSAGE_BOX=1, which tests set) and runs
  the quit path; nothing unwinds through SDL's C frames.
- `lifecycle.h`: a pure state machine, unit-tested without SDL. States:
  Booting, Running, Modal, FocusLost, Idle, Suspended, QuitPending,
  ShuttingDown, Exited (Overloaded, HalfRate, DeviceLost, Hung and the Play
  states arrive with M6, M7, M10 and M12). Inputs: window events, the Modal
  flag, power messages, input activity, quit requests, provider status.
  Outputs: the callback rate (`SDL_HINT_MAIN_CALLBACK_RATE` "0" normally,
  "waitevent" when Idle, "10" when Suspended), whether to render, whether
  gameplay runs, whether to clear held input, frame-clock resets. Rules from
  the design: Suspended only on MINIMIZED, HIDDEN or OCCLUDED or a power
  suspend, never on a NULL acquire; leaving it resets the frame clock (no
  catch-up); Idle only when the app allows it (the editor, M10) after 1 s
  without input; FocusLost clears held input; Modal removes waits and is
  never Overloaded (M6 consumes the flag).
- `quit.h`: two-phase quit. QUIT, a window close or `App::RequestQuit()` set
  quit_pending; callbacks keep returning CONTINUE while any QuitProvider
  reports pending work (M9's Durable jobs and M10's prompt and dialogs plug
  in here; M5 has a test provider); a provider can cancel the quit;
  `ReadyToExit()` turns true only after the drain, and the shutdown deadline
  starts then (the watchdog's, wired at integration). WM_QUERYENDSESSION and
  TERMINATING call the synchronous recovery hook first.
- The frame: drain the inbox, plan ticks with the M2 FixedStepper from a
  FrameClock built from SDL's display mode and SDL_GetTicksNS, tick the
  running Worlds through SimDriver::TickInline, record the clear-only frame,
  present. A minimal `BasicInputService` builds InputFrames from held keys
  per tick (M8 replaces it with timestamped edge placement).
- Shutdown in the design's order, for the parts that exist: remove the event
  filter, SDL_WaitForGPUIdle, destroy Worlds, release the device, remove the
  subclass, destroy the window, SDL_QuitSubSystem, then the integration's
  log worker stop and watchdog join; release builds terminate the process
  after the final flush, other builds return.

**rampage executable** (games/rampage/app; links se_runtime and
game_rampage; no ImGui): runs the sandbox scene through game_rampage's
GameModule at 120 Hz with the clear-only frame (M7 draws the sprites), the
window title showing tick and state, `--frames N` for tests.

**Platform on Linux:** the Linux presets build SDL3 without window-system
development packages (dummy and offscreen video only) so that the inbox,
wake, quit and lifecycle tests run under TSan and ASan in WSL. Window, GPU and
desktop tests carry the labels desktop and gpu, already excluded there.

**Tests** (tests/platform; labels unit, or desktop for anything with a
window; desktop tests never move the real mouse or send real keystrokes, and
open their windows without taking focus):
- inbox: LOW_MEMORY and TERMINATING pushed from an engine thread while main
  iterates reach main through the inbox; an SDL event watcher installed by
  the test never runs on the pushing thread; regular events keep their order
  (Windows and Linux, TSan);
- WakeMain from another thread wakes a main thread blocked in waitevent;
  coalescing (100 wakes, at most one pending event);
- lifecycle and quit state machines: every transition above, including a
  NULL acquire on a visible window (stays Running), a cancelled quit, a
  provider that keeps the quit pending, and the deadline start;
- callbacks: AppQuit(NULL); a throw injected in AppIterate and in AppEvent
  reaches the catch-all and runs the quit path;
- desktop: ticks continue during a modal move loop with Modal set (enter it
  programmatically on main, for example WM_SYSCOMMAND SC_MOVE posted to the
  window, and end it from a helper thread with a posted key or
  WM_CANCELMODE; at least 100 ticks per second of modal time, the cap still
  applied);
- desktop: NULL acquires injected for 30 frames on a visible window never
  enter Suspended, and presentation resumes within 2 frames after;
- desktop: minimize gives Suspended at a callback rate of 10 with no frames
  rendered and gameplay paused; restore resets the frame clock (no burst of
  catch-up ticks);
- desktop: WM_QUERYENDSESSION sent on main calls the recovery hook
  synchronously; power suspend and resume enter and leave Suspended;
- desktop, msvc-asan: `--fail-init-step K` for every init step exits with
  SDL_APP_FAILURE, *appstate NULL, no thread left running, ASan clean;
- desktop: `rampage --frames 300` exits 0 with ticks advancing.
`scripts/ci.ps1 -Desktop` runs the desktop label on msvc-dev and msvc-asan;
`-Milestone` includes it. Real-input variants (SendInput drags) are a
gates.md entry.

### Exit (M5)
- A worker-pushed LOW_MEMORY reaches main without a queue drain on the
  worker.
- Ticks continue during a drag with Modal set.
- NULL acquires on a visible window never suspend.
- AppInit failure at each step is clean under ASan.
- The rampage executable runs the sandbox in a window and quits cleanly
  through the two-phase quit.
- Full matrix green, including `-Desktop` and the WSL presets.
