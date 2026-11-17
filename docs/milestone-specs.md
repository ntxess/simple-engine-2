# Milestone specs: M1 to M14 and M17

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

## M6 SDL presentation patch and FramePacer (design build step 4)

Design sources: docs/design/mt-design.md "Threads" (vblank sampler),
"Input And Latency" (Pacing modes, Computed), "Rendering" (Present, GPU
timing, Device loss), "Platform" (the waitable, DwmGetCompositionTimingInfo,
windowed VRR, Vulkan FIFO), the API sketch (frame_pacer.hpp, time.hpp), the
"Frame Timeline Text", build step 4, red team P1 and P2, and the risks
"Carrying a vendored SDL patch for years" and "Present statistics or the
vblank sampler misbehave". M6 runs as two tracks, M6a (the patch and its
engine wrapper) and M6b (the pacer and the vblank sampler), which meet in
the present API below. M7 runs beside them.

M5 found that on this machine SDL reports 60.001 Hz for the window's
display while VSYNC presents run at about 165 Hz (ADR-039's measured-period
fallback). M6b's vblank sampler is the ground truth for the window's
output; find out which of SDL's display query and the present rate is
wrong, and record it in an ADR.

### The present API (the contract between M6a and M6b)

`engine/se_render/include/se_render/present.h`, namespace `se::present`.
Every function asserts the main thread except where marked. In an
unpatched build (SE_SDL_PATCH=OFF, preset msvc-unpatched-dev) every query
reports "unsupported" and every action is a no-op that returns false; the
engine then runs Standard pacing.

```cpp
struct Caps {
  bool patched = false;                 // built with the patch
  bool frame_latency_waitable = false;  // D3D12 waitable, Vulkan present_wait
  bool present_stats = false;           // displayed-refresh statistics
  bool gpu_timestamps = false;          // per-command-buffer begin/end
  bool native_handles = false;
  bool device_status = false;           // device-removed query
  bool tearing = false;                 // IMMEDIATE with ALLOW_TEARING (VRR)
};
Caps QueryCaps(GpuDevice&);

// 1 or 2; creates the waitable on first use (D3D12: the swapchain is
// recreated with FRAME_LATENCY_WAITABLE_OBJECT, then
// SetMaximumFrameLatency). False when unsupported.
bool SetFrameLatency(GpuDevice&, int frames);
enum class WaitResult : std::uint8_t { kReady, kTimeout, kUnsupported };
// D3D12: WaitForSingleObjectEx on the waitable. Vulkan: vkWaitForPresentKHR
// for the present |frames| back. Never longer than |timeout_ns|.
WaitResult WaitFrameLatency(GpuDevice&, std::uint64_t timeout_ns);

struct PresentStats {             // DXGI_FRAME_STATISTICS semantics
  std::uint64_t present_count = 0;          // presents the swapchain issued
  std::uint64_t present_refresh_count = 0;  // refresh that showed the last
                                            // displayed present
  std::uint64_t sync_refresh_count = 0;
  std::uint64_t sync_time_ns = 0;           // that vblank, SDL_GetTicksNS
                                            // domain
  std::uint64_t last_present_count = 0;     // GetLastPresentCount
};
// False before the first displayed present, while minimized, or when
// unsupported (Vulkan without VK_GOOGLE_display_timing or present_id).
bool GetPresentStats(GpuDevice&, PresentStats&);

// GPU duration of one command buffer. Begin after the command buffer is
// acquired, End right before submit; Read after its fence signalled.
void BeginGpuTiming(GpuDevice&, SDL_GPUCommandBuffer*) noexcept;
void EndGpuTiming(GpuDevice&, SDL_GPUCommandBuffer*) noexcept;
struct GpuTime { std::uint64_t duration_ns = 0; };
std::optional<GpuTime> ReadGpuTime(GpuDevice&, SDL_GPUFence*);

enum class DeviceStatus : std::uint8_t { kOk, kRemoved, kUnknown };
DeviceStatus QueryDeviceStatus(GpuDevice&);   // kUnknown when unsupported

struct NativeHandles {   // borrowed; valid while the device lives
  void* d3d12_device = nullptr;  void* d3d12_queue = nullptr;
  void* dxgi_swapchain = nullptr;
  void* vk_instance = nullptr;   void* vk_physical_device = nullptr;
  void* vk_device = nullptr;     void* vk_queue = nullptr;
  std::uint32_t vk_queue_family = 0;  void* vk_swapchain = nullptr;
};
bool GetNativeHandles(GpuDevice&, NativeHandles&);

// IMMEDIATE (tearing) or VSYNC for the claimed window; VRR uses IMMEDIATE.
bool SetPresentMode(GpuDevice&, SDL_GPUPresentMode);
```

M6a owns present.h and present.cc. M6b never includes them: the pacer
talks to a `PresentPort` interface that M6b owns
(`se_runtime/present_port.h`: the same operations as virtual functions,
plus `WaitFrameFence(frames_back, timeout_ns)` for the N-L wait and
`RepresentLast()` for HalfRate), with a fake in `tests/support` for unit
tests and a clear-only implementation over M5's GpuDevice and
FrameRecorder meanwhile. The integration step writes the real adapter
over se::present and M7's FrameRecorder.

### M6a: the SDL presentation patch (cmake/patches, se_render)

- `cmake/patches/sdl3-3.4.20-se-presentation.patch`, a `git diff` against
  the extracted SDL3-3.4.20 tarball, applied by cmake/ApplyPatch.cmake (the
  hook in cmake/Dependencies.cmake already exists and the patch's SHA256 is
  checked). All new SDL API lives in one new public header
  `include/SDL3/SDL_gpu_se.h` with `SDL_SE`-prefixed functions, plus the
  smallest backend edits, so rebasing onto a later SDL is mechanical.
  Keep a rebase note in docs/deps.md (files touched, line counts).
  - D3D12: the frame-latency waitable (swapchain flag
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, then
    IDXGISwapChain2::SetMaximumFrameLatency and GetFrameLatencyWaitableObject),
    present statistics (GetFrameStatistics, GetLastPresentCount), timestamp
    queries (a query heap and a readback buffer per frame in flight,
    resolved at submit; GetTimestampFrequency), native handles, the
    device-removed status (GetDeviceRemovedReason), and ALLOW_TEARING with
    sync interval 0 for IMMEDIATE (SDL already does this; verify).
  - Vulkan: VK_KHR_present_id and VK_KHR_present_wait when the device has
    them (the waitable analogue and present ids), VK_GOOGLE_display_timing
    for statistics when present, vkCmdWriteTimestamp queries, native
    handles, VK_ERROR_DEVICE_LOST status. Missing extensions report
    unsupported, never fail device creation.
  - SDL's own behaviour without the new calls is unchanged.
- `se_render/present.h` and `src/present.cc` as specified above, behind
  `SE_SDL_PATCH` (compile definition SE_SDL_PATCHED=0/1).
- Linux: the patch compiles in the WSL presets (SDL's Vulkan backend builds
  with the dummy/offscreen video drivers); GPU tests stay excluded there
  (gates.md).
- Tests (label gpu, the commit gate runs them on Windows; each runs on
  D3D12 and on Vulkan through --gpu-driver / SE_GPU_DRIVER, and is skipped
  with a printed reason when the driver is not available):
  - caps on both backends; a planted check that msvc-unpatched-dev reports
    every cap false and the engine still presents;
  - a hidden or unfocused window (desktop label where a window is needed):
    SetFrameLatency(1) then 120 frames, each WaitFrameLatency returns kReady
    within 2 refreshes; latency 2 likewise;
  - present statistics advance: present_count increases by one per
    present and present_refresh_count tracks sync_refresh_count;
  - GPU timing: a command buffer that clears a 4096x4096 target reports a
    duration > 0 and below 100 ms; Read before the fence returns nullopt;
  - native handles are non-null for the active backend;
  - QueryDeviceStatus is kOk.
- ADRs 050-053.

### M6b: FramePacer and the vblank sampler (se_runtime, se_platform)

- `se_platform/vblank_sampler.h`: `VblankSampler` (Windows; a stub that
  reports unsupported elsewhere). One thread, role kVblankSampler, HighQoS,
  started with se_os StartThread. It finds the DXGI output that shows the
  window (MonitorFromWindow, then EnumAdapters1/EnumOutputs on every adapter,
  matching DXGI_OUTPUT_DESC::Monitor), blocks in IDXGIOutput::WaitForVBlank,
  and publishes each vblank's QPC stamp converted to the SDL_GetTicksNS
  domain in one 64-bit atomic, plus a count and a period estimate (median of
  the last 64 intervals). Main publishes a new monitor after
  DISPLAY_CHANGED, a window move to another display or a mode change
  (`SetMonitor`, release store; the sampler re-resolves the output on its
  next wake). It never touches World state, SDL, ImGui, the swapchain or
  engine GPU objects. Stop joins within one refresh plus 50 ms (the wait is
  bounded by a 100 ms watchdog of its own: if WaitForVBlank does not return,
  the sampler marks itself stale). Link dxgi.lib only in se_platform.
- `se_runtime/frame_pacer.h`: `FramePacer` with `Mode { kPaced, kStandard,
  kVrr }`, split into a pure `PacerPolicy` (decisions from numbers; no
  clock, no SDL; fully unit-tested) and the shell that calls SDL, the
  present API and the sampler.
  - `BeginFrame()`: waits for the fence of frame N-L (L = frame latency;
    the fence comes from the frame recorder, see Integration), reads present
    statistics and turns them into hits and misses (a present displayed
    later than its target refresh is a miss; count misses from
    present_refresh_count against present_count, never from frame starts),
    reports each result to FixedStepper::OnPresentResult(missed, modal), and
    returns the FrameClock (period from the sampler when it agrees with the
    display mode within 0.5%, else SDL's mode, else ADR-039's measured
    period).
  - `EndFrame(const StepPlan& next)`: the waitable (WaitFrameLatency), then
    in Paced a sleep with SDL_DelayNS to the window output's predicted vblank
    minus B(next), B = steps*S_hi + R_hi + g_hi + m with m =
    max(1.5 ms, timer p99 + 0.5 ms) (FrameCosts already carries these; g_hi
    is unknown without timestamps, and then k_max holds at k_rt whenever the
    previous fence had not signalled). No sleep while Modal. Never spins.
  - Mode choice: Paced is the Windows runtime default on fixed-refresh
    displays when the waitable and present statistics exist and the sampler
    is valid and agrees with the display mode; otherwise Standard (the
    waitable when present, then SDL_WaitForGPUSwapchain; no late latch).
    VRR (opt-in `--pacing vrr`, when tearing is supported): IMMEDIATE, a
    self-paced 120 Hz deadline (60 Hz when the panel range is lower), one
    tick per frame, no miss or halving logic. `--pacing paced|standard|vrr|
    auto` (default auto); the editor default (M10) is Standard with latency
    2. Linux: Standard; Paced from present_wait where supported (not
    claimed until measured; gates.md).
  - HalfRate above 100 Hz with hysteresis (enter after the miss rate or the
    planned cost exceeds the budget for 2 s; leave after 4 s of margin):
    Paced skips every second vblank by phase; Standard re-presents the last
    composite (a FrameRecorder call M7 provides; M6b uses the clear-only
    recorder until integration). `PresentInterval()` is 1 or 2.
  - Adaptive latency: `FrameLatency()` is 1, or 2 when CPU and GPU each fit
    in the period but their sum does not (hysteresis as above); a change
    calls present::SetFrameLatency.
  - Per-frame stats: waitable waits, fence waits, missed vblanks, NULL
    acquires, mode, interval, latency; logged once a second at debug level
    and shown in the window title in test builds.
- `tools/presentmon/`: `analyze.py` (stdlib only) reads a PresentMon CSV
  (MsBetweenPresents, MsUntilDisplayed, MsBetweenDisplayChange,
  MsInPresentAPI, Dropped) for the rampage process and reports displayed
  latency p50/p99, dropped and repeated frames; a unit test with a synthetic
  CSV. Running PresentMon itself is a gates.md entry (not installed, not
  approved for download).
- Tests:
  - unit (everywhere, TSan included): PacerPolicy mode choice per caps,
    sampler validity and the 0.5% rule; B and the sleep target from costs;
    miss counting from statistics sequences (on time, one late, a run of
    late presents, statistics resets after a mode change); HalfRate entry
    and exit with hysteresis; adaptive latency; VRR deadline; the sampler
    loop with a fake vblank source publishing and re-resolving across a
    monitor change, and Stop while blocked;
  - desktop (Windows; unfocused windows, no real input), after integration
    where they need M6a: see Exit.
- ADRs 054-058.

### Exit (M6)

- Paced misses fewer than 1% of vblanks by present statistics over 20 s of
  rampage at the 120 Hz Locked load on this machine's primary display
  (desktop test `pacing.paced_misses`; prints the mode, period, misses and
  the frames counted). A busy desktop can fail it: the test reports the
  foreign input it saw (GetLastInputInfo) and fails only when none arrived.
- An injected late frame (`--inject-late-frame FRAME:MS`, test builds)
  drains within 2 frames: the statistics show at most 2 consecutive misses
  after it and on-time presents after that.
- Two monitors pace to the window's monitor: the test moves its own window
  to each display (SDL_SetWindowPosition; never the cursor), and the
  sampler's output and period match that display within 0.5%; Paced mode
  stays on, or drops to Standard with the logged reason when SDL's mode
  disagrees.
- The unpatched preset (msvc-unpatched-dev) builds, passes its tests, and
  rampage runs in Standard.
- VRR runs `--pacing vrr` at a self-paced 120 Hz deadline (frame starts
  within 0.5 ms p99 of the deadline over 10 s); visible VRR behaviour,
  displayed-time latency (PresentMon) and the photodiode check are pending
  gates in docs/gates.md, with exact commands.

## M7 Renderer2D on main (design build step 5, CF b)

Design sources: docs/design/mt-design.md "Rendering" (all of it), the API
sketch (se_render: GpuResources, FrameRecorder), the "Frame Timeline Text"
handoff 2 (uploads), "Platform" (D3D12 release frees the client container,
cancel after acquire, ImGui backend hazards for later), build step 5;
docs/design/costfree-end-state.md "Rendering". M7 replaces M5's clear with
the Sandbox drawn on screen. Viewport targets exist for M10's editor; ImGui
is not linked in M7.

**Dependency:** stb (nothings/stb commit
`4e9b368bfc071e97206ea5c6adba68dd0cbf8897`, archive
https://github.com/nothings/stb/archive/4e9b368bfc071e97206ea5c6adba68dd0cbf8897.tar.gz,
SHA256 `e60706ab084e0cae35ee7282deb4a7f283e4a28be1896d097437205da01a0683`,
1.5 MB; already in `.deps/stb/`). Declare it in cmake/Dependencies.cmake and
docs/deps.md as an INTERFACE target `se::stb` (stb_image.h, and
stb_image_write.h for tests and tools only), with the implementation macro
defined in exactly one translation unit of se_render. stb is a third-party
header: exempt from lint, included with angle brackets.

**Shaders.** HLSL sources in `engine/se_render/shaders/` (sprite.vert.hlsl,
sprite.frag.hlsl, blit.vert.hlsl, blit.frag.hlsl, and any others).
- DXIL is compiled at build time on Windows with the Windows SDK's dxc.exe
  (10.0.26100.0 x64; find it through the MSVC environment or the SDK root,
  fail configure with a clear message when absent), into generated headers
  of byte arrays.
- SPIR-V comes from glslang in WSL (`glslangValidator -D -V -S <stage> -e
  main`; the Windows SDK's dxc has no SPIR-V codegen, checked). It is
  committed as generated headers under `engine/se_render/shaders/generated/`
  by `tools/shaders/build_shaders.py` (runs glslang through wsl.exe on
  Windows, directly on Linux). A test in the WSL presets (label checks)
  regenerates them and fails on any difference, so a stale SPIR-V header
  cannot pass CI. glslang output was checked to be deterministic.
- SDL_CreateGPUShader with the format the device reports; resource counts
  declared to SDL match the shader (a test reflects each blob's counts).

**se_render (M7 part)** (STATIC; deps se_core, se_platform, se_render_core,
SDL3, se::stb). Every SDL_GPU wrapper asserts SDL_IsMainThread() (SDL does
not check).
- `gpu_resources.h`: `GpuResources`.
  - `Handle<Texture>{index, generation}`; `Resolve(Handle) ->
    SDL_GPUTexture*`, valid this frame only, never stored by callers
    (a planted check, label checks, bans storing an `SDL_GPUTexture*` member
    outside se_render); a stale or unknown handle resolves to a
    built-in 1x1 magenta texture and counts.
  - The retire queue: `Retire(SDL_GPUTexture*)` (and buffers, samplers,
    pipelines) releases after this frame's submit, never before; SDL's D3D12
    backend frees the client container at release (red team C2).
  - The upload queue: `QueueUpload(Handle, Payload)` (decoded RGBA8 pixels,
    width, height); `CollectUploads()` at frame start moves uploads whose
    fence has signalled to resident (generation flip, the old texture
    retired after the next submit); `RecordUploads(cmd)` copies at most
    8 MB per frame through a transfer buffer in the upload command buffer,
    which is submitted with its own fence whether or not the frame presents
    (uploads are independent of presenting, so a minimized window still
    uploads). A skipped frame never shows a half-uploaded texture.
  - Load from config/texture.json names (ADR-026) with stb_image on main at
    boot in M7 (M9 moves decoding to the background lane); a missing file
    is an Error naming the path, and the sprite draws magenta.
  - Device loss support: `ReleaseAll()` through the retire queue and
    `RecreateAll(GpuDevice&)` re-uploading from the kept decoded payloads
    and re-resolving handles; Worlds are untouched (wired at integration).
- `renderer_2d.h`: `Renderer2D`.
  - `Frame(const RenderList&, ...)`, split into the build phase (may throw:
    vertices from the RenderList's quads into a pre-sized CPU buffer,
    camera transform, batches) and the recording phase (noexcept).
  - Sprite pipeline: one vertex/index buffer pair, grown only in the build
    phase (growth is logged, never during recording), filled through a
    transfer buffer mapped with cycle=true; one draw per batch (texture
    change); nearest sampling (the legacy SFML textures are not smooth);
    premultiplied or straight alpha to match legacy (record which in an
    ADR); the camera from Camera2D (M3's SandboxCamera/ArenaCamera).
  - The game view renders into an offscreen composite target the size of
    the window's pixels, then a blit pass draws it to the swapchain. The
    composite serves readback (goldens, tests) and HalfRate's re-present
    (`RepresentLast`: the blit again, no new RenderList).
  - `ViewportTarget`: an offscreen colour target with its own size and
    camera, resized only at frame start (old texture retired), for M10's
    editor viewports; a test renders two viewports and the composite in
    one frame.
  - Stats per frame: draws, batches, quads, upload bytes, NULL acquires,
    the build and record times; the FrameCosts R input.
- `frame_recorder.h`: the full transaction, replacing M5's clear-only one.
  1. Build phase (may throw): RenderList, vertices. Nothing GPU-side is
     acquired yet.
  2. Prepare: acquire the upload command buffer when uploads are pending,
     and the frame command buffer (failure is an Error; the frame is skipped
     and logged).
  3. Copy pass for vertices, then offscreen viewport passes and the
     composite, then the non-blocking swapchain acquire (NULL skips the
     swapchain pass, never a state change; the command buffer is still
     submitted).
  4. The swapchain pass (the blit).
  5. The guard (the destructor) ends any open pass, submits with
     SDL_SubmitGPUCommandBufferAndAcquireFence, never cancels after an
     acquire, keeps the fence for the pacer's N-L wait (`TakeFence`), and
     runs the retire queue.
  Recording is noexcept: buffers are pre-sized and only engine-provided
  draw callbacks run. The hook points the integration fills for M6 are
  marked `INTEGRATION(M6)`: BeginGpuTiming after the command buffer is
  acquired, EndGpuTiming before submit, ReadGpuTime when the fence
  signalled.
- Test hooks (test builds, AppConfig, like M5's): `--inject-frame-fault
  STEP[:EVERY]` with STEP one of build, vertices, upload, prepare,
  copy_pass, viewport_pass, acquire, swapchain_pass, submit. Build-phase
  steps throw a standard-library exception (engine code never throws);
  recording steps simulate the SDL call failing (a NULL pass, a NULL
  texture, a failed submit). `--reload-texture NAME@FRAME` re-decodes and
  queues an upload of a texture (optionally a test-only replacement file);
  `--readback FRAME:FILE` writes the composite as PNG (stb_image_write).

**rampage.** The App renders the Sandbox's RenderList (M3's builder over the
current World at alpha) instead of the clear; the window shows the textured
sandbox.

**Legacy image comparison.** legacy_replay (legacy tree, legacy style)
gains `--image-out TICK:FILE`: it renders the Sandbox scene at that tick
into an sf::RenderTexture of the legacy viewport's size and writes a PNG.
A new tool `tools/render_shot` (se_render, a GPU device without a window)
renders the same scenario and tick from the same scene and seed into a PNG.
A test (labels gpu and legacy, legacy presets plus the Windows gate where
the legacy build is present) compares them: identical size, and at least
98% of pixels within 8/255 per channel with mean absolute error at most
2/255; differences are written as a diff PNG next to the output. If SFML
cannot create a context without a visible window, record that in
gates.md and keep the comparison as a desktop test.

**Golden images.** `tests/golden/render/<driver>/` (d3d12, vulkan): PNGs of
fixed scenes (a sprite grid, rotated sprites, overlapping layers and
alpha, the sandbox at tick 600), compared with the same tolerance;
SE_UPDATE_GOLDENS=1 rewrites them. Label gpu (no window), so the Windows
commit gate runs them on both backends.

**Bench.** bench.render_frame_10k (Release only, like M3's): RenderList plus
vertex generation for 10k visible sprites, CPU side; p95 at most 1 ms. M3's
bench.renderlist_10k stays.

**Linux.** se_render and its shader headers build in the WSL presets; GPU,
golden-image and desktop tests stay excluded (gates.md, M18).

ADRs 059-065.

### Exit (M7)

- The Sandbox is on screen in rampage, textured, matching the legacy image
  within the stated tolerance (or the gates.md fallback above), and I check
  a screenshot.
- 10k-sprite RenderList plus vertices at most 1 ms p95 in Release.
- Fault injection at every step for 100 frames on D3D12 and on Vulkan
  under msvc-asan (desktop): the app keeps running or quits cleanly as the
  step's policy says, never cancels after an acquire (an assertion in the
  guard), leaks nothing (ASan), and draws correctly again after the fault
  stops.
- A texture reloaded while minimized is correct after restore: the test
  minimizes its own window (SDL_MinimizeWindow), reloads a texture to a
  test replacement, restores (SDL_RestoreWindow), reads back the composite,
  and the sprite's pixels match the replacement.
- Golden images pass on both backends; the SPIR-V freshness check passes
  in WSL; every new rule has a planted violation.

## Integration (M6a + M6b + M7)

- The PresentPort adapter over M6a's se::present and M7's frame
  transaction: the N-L fence from FrameRecorder::TakeFence, HalfRate's
  Standard re-present through Renderer2D::RepresentLast; the
  `INTEGRATION(M6)` timing hooks filled, so g_hi is measured and FrameCosts
  carries it.
- Device loss: when QueryDeviceStatus is kRemoved (checked at frame start
  and after a failed submit), the lifecycle enters DeviceLost: release
  through the retire queue, recreate the device and claim the window on
  main, GpuResources::RecreateAll, re-resolve handles; Worlds are
  untouched. A test hook `--inject-device-lost FRAME` makes the status
  report kRemoved once (desktop test: the app recovers and the readback
  after recovery matches the golden).
- The vblank sampler starts at boot step 9 with the other helpers and stops
  with them; EngineThreadsRunning counts it; its breadcrumb slot appears in
  dumps.
- The M6 desktop exit tests, then the full matrix (-Milestone -Legacy, which
  includes -Desktop, plus wsl:linux-clang-debug and msvc-unpatched-dev).
- ADRs 066 and on.

## M8 Input placement and audio (design build step 6)

Design sources: docs/design/mt-design.md "Threads" (input sampler),
"Input And Latency" (input path, timestamp sources, placing edges on
ticks), "Audio And Assets" (Audio), "Platform" (SDL_UpdateGamepads from any
thread, miniaudio rules), the API sketch (InputEdge, InputService::latch,
tickInline), build step 6, red team P10; docs/design/costfree-end-state.md
input and audio rows. M8 replaces M5's BasicInputService and the
NullAudioSink.

### Input (engine/se_runtime, engine/se_platform)

- `se_runtime/input_service.h`: `InputService` (implements M2's
  InputSource) replacing BasicInputService. SDL_AppEvent hands it every
  key, mouse button, gamepad button and axis event with its SDL timestamp;
  it keeps ordered, stamped edges (InputEdge: action, down, stamp_ns,
  order, source), held state and axes. Focus loss and the Windows modal
  loop clear held input (M5's lifecycle signals). No system polls the
  keyboard.
- `ActionMap`: keys from config/config.json "control-settings" /
  "key-bindings" (legacy names key-up, key-left, ..., key-6), resolved to SDL
  keycodes; default gamepad bindings in code (recorded in an ADR), both
  mapped to ActionIds the game declares (rampage: move up/left/down/right
  and the existing legacy actions). Unknown names are a boot Error naming
  the key.
- Timestamp sources: runtime builds set SDL_HINT_WINDOWS_RAW_KEYBOARD=1
  (keys stamped on SDL's raw-input thread, SDL_GetTicksNS domain); an
  AppConfig flag leaves it off (the editor, M10, uses Win32 message stamps).
  Every edge records its EdgeSource, and the replay keeps it.
- Placing edges on ticks, exactly as the design says: Locked cadence splits
  the wall time since the previous frame start into k equal windows; Free
  cadence ends tick i's window at frameStart - remainder - (steps-1-i)*dt;
  edges stamped after the last window go to the last tick; in a frame with
  no tick, edges wait for the next frame's first tick. Check that
  StepPlan::tick_window_end_ns (M2) is computed this way and fix it if not
  (the stepper's tests pin it). A pure `PlaceEdges` function holds the rule
  and is unit-tested on synthetic frames (30/60/144/165 Hz, Locked and
  Free, zero-tick frames).
- `se_core/input_buffer.h`: the 6-12 tick buffer window (default 8, set
  per action by the game): a press latched on tick t stays consumable until
  t + window. Pure, unit-tested; rampage's Intent system uses it for its
  discrete actions.
- `se_platform/input_sampler.h`: `InputSampler`, one thread (role
  kInputSampler, started through se_os StartThread). It calls
  SDL_UpdateGamepads at 1000 Hz on AC and 250 Hz on battery (main reads
  SDL_GetPowerInfo every 5 s and publishes the rate), with
  SDL_HINT_AUTO_UPDATE_JOYSTICKS=0 (M5 sets it), waiting with SDL_DelayNS
  and checking stop every period. It is parked (an atomic doorbell wait)
  while the app is Suspended or Idle. SDL_UpdateGamepads pushes SDL events,
  so the sampler obeys ADR-047: it starts after the first iterate and stops
  before shutdown step 4, inside the WakeEventScope window. It never
  touches World state, InputService, ImGui or the GPU.
- Tests:
  - unit (everywhere, TSan included): PlaceEdges; InputService latching
    (order, held state, focus-loss clear, per-tick edges, the last-tick
    rule, zero-tick frames); InputBuffer; the sampler's park and stop with
    a fake poll function;
  - desktop: key events pushed with SDL_PushEvent and explicit timestamps
    8 ms apart land on different ticks at 60 Hz Locked; an SDL virtual
    gamepad (SDL_AttachVirtualJoystick) whose buttons a test thread toggles
    8 ms apart lands on different ticks through the sampler, with stamps
    about 1 ms apart from the true toggle times; never real input;
  - replay: a session recorded with pushed events replays headless through
    se_replay with identical per-tick hashes, and the edge sources survive.
- `tools/input_probe`: a small window that logs each real key event's SDL
  and Win32 message stamps and reports their resolution (raw keyboard on
  and off). Running it needs a person: docs/gates.md row "Win32 key stamp
  resolution", with the command. A real gamepad is another gate row.

### Audio (new module engine/se_audio)

- se_audio (STATIC; deps se_core, miniaudio 0.11.26 from .deps, already
  declared; miniaudio is confined to se_audio, enforced by the link graph
  and an include check with a planted violation). Main owns the
  `AudioEngine` (ma_engine) and is the only caller of ma_engine and ma_sound
  functions; groups Music and Effects with volumes from config/config.json
  "sound-settings" (0-100). If device init fails, it uses miniaudio's null
  backend and logs it. A noDevice mode (frames pulled with
  ma_engine_read_pcm_frames) serves tests and offline checks.
- `Sound` data: decoded PCM (miniaudio's decoder: WAV and FLAC, no new
  dependency) held by `std::shared_ptr<const Pcm>`; voices reference it
  through an ma_audio_buffer. A voice record holds its ma_sound, the PCM
  pointer, its group and a cosmetic flag. PCM is released only after the
  voice's end id has drained and ma_sound_uninit has returned. Releasing a
  scope stops and uninitialises its voices first (ma_sound_uninit is the
  fence). Reloading a sound gives the new PCM to new voices only.
- The end-of-sound callback (miniaudio's thread) writes the voice id into
  an SPSC ring sized for the voice cap (256) and never waits; a per-frame
  `Sweep()` also uninitialises voices whose ma_sound_at_end is true.
- Scheduling: `AudioBridge` (implements SimDriver's AudioSink) drains the
  World's audio events after each tick (an engine `AudioEvents` ctx
  resource systems push to: sound key, group, cosmetic, position for pan)
  and starts each sound at max(now, tick anchor + D) with
  ma_sound_set_start_time_in_pcm_frames, mapping SDL_GetTicksNS to the
  engine's PCM clock (recalibrated each frame). The anchor is the tick's
  window end (TickInline already passes it); D = B + T/2 comes from the
  pacer (B of the frame's plan, T the display period; 13.8 ms at 60 Hz
  Paced Locked). Late gameplay sounds play at once; a per-group voice cap
  steals the oldest voice; only cosmetic sounds more than 100 ms late are
  dropped (counted).
- rampage: one or two sounds (small WAV files generated by a checked-in
  script; no downloads) emitted as audio events by rules that already
  exist in game_rampage (for example when an effect applies damage). Audio
  events never change simulation state, so the M3 hashes are unchanged.
- Tests (everywhere; noDevice mode, no sound on the owner's speakers):
  sounds from one frame's two ticks start 8.33 ms (+-1 PCM frame at 48 kHz)
  apart, by onset detection on the mixed output; late and cosmetic rules;
  voice-cap stealing; the end ring and the sweeper; stop, scope release and
  reload during playback are clean under ASan and TSan (WSL); the null
  backend fallback. A desktop test opens the real device at volume 0 for
  1 s and closes it cleanly.

ADRs 073-077.

### Exit (M8)

- Key presses and pad presses 8 ms apart land on different ticks (pushed
  events and the virtual gamepad).
- Replays reproduce, with edge sources recorded.
- Sounds from one frame's two ticks start 8.33 ms apart.
- Stop and reload during playback are clean under ASan (Windows and WSL)
  and TSan (WSL).
- Win32 stamp resolution and a real gamepad are gates.md rows with
  commands (they need a person and hardware).

## M9 Background lane and AssetRegistry (design build step 7, CF d)

Design sources: docs/design/mt-design.md "Threads" (background lane),
"Data Flow" (AssetRequest, SaveRequest, decoded payloads, uploads),
"Audio And Assets" (Assets), "Lifecycle" (QuitPending), build step 7;
docs/design/costfree-end-state.md assets rows. M9 moves M7's boot-time
decoding on main to the lane and adds hot reload and durable saves.

**New module engine/se_assets** (STATIC; deps se_core, se_os, se_platform
for se::fs, lz4_static, glaze, se::stb; FORBID SDL video and GPU headers
outside se_platform's fs).

- `background_lane.h`: `BackgroundLane`, one thread (role kBackground).
  - Requests through an SPSC ring (main to lane) and results through an
    SPSC completion ring (lane to main); main releases a binary_semaphore
    after pushing; the lane waits with a 500 ms timeout, which drives the
    hot-reload scan; a stop_callback releases it.
  - Classes: Urgent (loads someone waits on, user imports, hot reload) at
    HighQoS and normal priority, always first; Background (autosave,
    recovery, flushes, thumbnails, scans) at EcoQoS. QoS switches per step
    (se_os Qos).
  - Jobs are sequences of steps of about 2 ms or less with a stop_token
    check between steps: file reads of at most 1 MB per step, LZ4 blocks of
    about 256 KB per step, scenes in chunks. The lane owns a buffer until it
    pushes the result.
  - Durable jobs are never cancelled; quit waits for them (a QuitProvider,
    M5's two-phase quit, reports kPending until they finish).
  - Writes to one path form a strand (first in, first out). Each file
    carries a revision; the lane never renames over a newer one. Writes go
    to a unique temp file in the same directory, then rename. A rename that
    hits another process's sharing violation retries for up to 2 s, then
    fails and reports the Error (the document stays dirty).
  - Engine reads open files with FILE_SHARE_DELETE on Windows (extend
    se::fs if SDL's IOStream does not; record it in an ADR).
  - It never touches a World, SDL video, events or GPU, ImGui or the
    AssetRegistry tables. It may call WakeMain (inside the ADR-047 window).
- `asset_registry.h`: `AssetRegistry` on main: typed stores (Texture,
  Atlas, Sound, and parsed Prefab and SceneChunk payloads), `Handle<T>
  {index, generation}`, an `AssetScope` per GameState (and later per
  EditDocument) plus a persistent UI scope, refcounted per scope.
  - Loading: main submits an AssetRequest (Urgent when something waits on
    it); the lane returns a payload or an Error{code, message, path, line,
    column}; main drains the completion ring at iterate start, creates GPU
    objects through M7's GpuResources and queues uploads within its 8 MB
    budget; a texture becomes resident when its fence signals (M7).
  - A World never steps before its scope is resident: the game's sim holds
    (no ticks planned, logged) until then, so asset timing cannot change
    simulation results.
- Cooked atlas pages: `tools/asset_cook` turns the textures named in
  config/texture.json into `.seatlas` files (a small header plus
  independently LZ4-compressed blocks of about 256 KB of RGBA8), run by
  CMake at build time into the build tree; the runtime loads cooked pages
  through the lane. stb_image runs only at import (the cook tool, and the
  editor in M10). Hot reload in a dev build re-cooks a changed source on
  the lane (Urgent).
- Hot reload: the lane scans the watched files every 500 ms (size and
  modification time) and wakes main on a change; a changed asset swaps at
  frame start once resident, and the old texture goes through M7's retire
  queue. (Editor Play's freeze of gameplay assets is M10.)
- Sounds: M8's decoded PCM comes from the lane (miniaudio's decoder on the
  lane thread, data only; ma_engine stays on main).
- Saves and flushes: `SaveRequest{path, bytes, revision, durable}`;
  replay and SessionLog flushes go through the lane as Background jobs.
- Test hooks (test builds): an se::fs fault and delay injector (a
  throttled disk: N ms per write step; a sharing violation on rename for
  M ms) and `--lane-step-trace`.
- Tests (everywhere, TSan in WSL for the rings and the lane):
  - quit during a large job (a 64 MB read or a 200-block LZ4 decode) joins
    within one step (measured, at most one step plus 5 ms);
  - Durable saves survive quit on a throttled disk: the quit stays
    QuitPending until the save is renamed into place, and the file is
    complete and parses;
  - out-of-order completion never renames over a newer revision (two
    saves of one path with revisions 1 and 2, completion forced in the
    wrong order);
  - strands keep per-path order under interleaved Urgent and Background
    work; Urgent always first;
  - the sharing-violation retry succeeds within 2 s and fails cleanly after;
  - hot reload: change a texture file while rampage runs (desktop): the
    sprite shows the new pixels after the swap (readback), the old texture
    is retired after the next submit (GPU objects alive count);
  - the registry: handles, generations, scope release, a World held until
    resident.
- Bench: bench.lane_steps (Release): p99 step time of a 1 MB read, a
  256 KB LZ4 block decode and a scene chunk parse, each at most 2 ms on
  this machine (the design's estimate becomes a measured threshold).

ADRs 078-082.

### Exit (M9)

- Quit during a large job joins within one step.
- Durable saves survive quit on a throttled disk.
- Out-of-order completion never renames over a newer revision.
- Steps measure at most about 2 ms (bench.lane_steps).
- rampage loads its textures from cooked atlas pages through the lane and
  hot-reloads a changed texture.

## M17a Dedicated sim driver, headless (design build step 13, part 1)

Design sources: docs/design/dedicated-driver.md "Threads" (sim), "Data
Flow" (InputEvent, SimCommand, Snapshot slot, returned slots, Fault),
"Snapshot And Interpolation", "Synchronization", "Lifecycle" (sim states
and shutdown), "Determinism And Debugging", the API sketch;
docs/design/mt-design.md "Changes To Cost Free End State" (Escalation seam)
and build step 13. The plan builds the threaded driver unconditionally,
behind `--sim-driver=inline|threaded` (M17b wires it into the App after
M10); inline stays the default. M17a is headless: no SDL.

- `se_sim/sim_thread.h`: `SimThread`, one thread (role kSim, se_os
  StartThread) that owns every running World while it runs and calls
  SimDriver::RunTick (unchanged) on absolute deadlines.
- `se_sim/paced_clock.h`: `PacedClock`: deadlines L_k = T0 + k*dt + phi; a
  waitable timer (SDL-free: se_os) to the deadline minus max(0.3 ms,
  measured overshoot p99), then a short spin with a pause instruction;
  clock_nanosleep(TIMER_ABSTIME) on Linux. phi slews by at most 0.1 ms per
  tick, only when the display period is a whole multiple of dt (set by the
  platform). While paused or idle the thread waits on a doorbell
  (std::atomic<uint32_t> sequence, atomic::wait, rung by a stop_callback).
  A `ManualClock` drives it deterministically in tests.
- Channels (se_core SpscRing):
  - `InputEvent` ring (4096) from the platform; the sim's `InputFolder`
    builds each tick's InputFrame with M8's placement rule against the
    sim's own deadlines. For hash equality the threaded driver also runs
    replays from recorded InputFrames (live latching differs by design).
  - `SimCommand` ring (256, move-only, sequence-numbered) with the ops M17a
    needs: StartGame, Pause, Resume, Step{n}, SetTimeScale, Suspend, Quit,
    and ApplySessionEntry (M2's SessionLog entries); the snapshot header
    acks the last applied sequence.
  - `SnapshotChannel`: a pool of 8 slots and two SPSC rings of slot indices
    (published 16, returned 16), exactly the design's memory orders. A
    slot holds a header (session, generation, tick, wall anchor L_k + dt,
    latch time, acked sequence, tick cost, dropped publishes), trivially
    copyable `RenderItem`s (about 56 bytes: entity id and spawn serial,
    position, rotation, scale, texture key, region, layer, z, tint, flip
    and flag bits teleported/visible), a sparse index from entity index to
    item, cameras (position, zoom, rotation, cut flag), and a reliable
    section (audio events, session events, scope releases) accumulated
    since the last successful publish. The sim fills a slot after Cleanup
    and never waits: with no free slot it skips the render part and keeps
    the reliable deltas for the next publish.
- `se_render_core/snapshot_render_source.h`: `SnapshotRenderSource` on the
  platform side: pops every published slot, applies reliable sections in
  tick order, keeps the newest 3 and returns the rest; tau = F - D';
  picks S_a and S_b around tau with alpha from the real anchor spacing;
  starvation holds the newest pair at alpha 1 and raises D' elastically
  (decay at most 5% per frame); per-entity lerp when full id, spawn serial
  match and no teleport flag (rotation along the shorter arc), otherwise
  S_b's state; discrete state from S_a below alpha 0.5; spawn, despawn and
  teleport switch at the half-tick; a session or generation change
  hard-cuts; camera cuts honour the cut flag. It produces the same
  RenderList the inline builder makes from a World (M3's BuildRenderList)
  for matching ticks and alpha.
- `FaultBox` per thread: exception_ptr written, then an atomic flag with
  release; the platform polls it with acquire after a wake. The sim's tick
  is a fault boundary (ADR-006 list grows by one file, recorded).
- Tests (everywhere, TSan in WSL for everything threaded):
  - inline against threaded: per-tick hashes identical on every golden
    replay (the M2 kernel goldens and M3's three rampage scenarios over
    3600 ticks) at W = 0; W = 1 and 3 become checks when M14 lands (say so
    in docs/gates.md or the plan's tracking, not as a skipped test);
  - SnapshotChannel property tests: randomized producer and consumer
    schedules (thousands of runs, seeded): every consumed slot is complete,
    no slot is written while the platform holds it, reliable sections are
    never lost or reordered, dropped publishes are counted;
  - SnapshotRenderSource: RenderList equality with BuildRenderList for
    matching state; spawn, despawn, teleport, id reuse with a new spawn
    serial, generation hard cut, starvation hold and decay;
  - PacedClock with ManualClock: the deadline grid, phi slew bound,
    overshoot accounting; with the real clock (Release, label slow) the
    wake error p99 is recorded;
  - commands: Pause, Step{n} runs exactly n ticks, Resume, Quit joins.
- Bench: bench.snapshot_publish_10k (Release): build and publish one slot
  of 10k items; p95 recorded as a threshold.

ADRs 083-086.

### Exit (M17a)

- Inline and threaded per-tick hashes equal on every golden replay at W =
  0 (W = 1 and 3 after M14).
- SnapshotChannel property tests pass; TSan clean in WSL.
- The inline driver stays the default; nothing in the App changes until
  M17b.

## Integration (M8 + M9 + M17a)

- M9's lane decodes M8's sounds; a sound hot-reloaded during playback gives
  new voices the new PCM and frees the old PCM only after its voices end
  (ASan desktop test).
- rampage's textures and sounds load through the lane (cooked atlas pages),
  and its scope must be resident before the first tick.
- The input sampler and the background lane start and stop in the boot and
  shutdown order with the other helpers (event pushers inside the ADR-047
  window), appear in the ThreadRegistry and in dumps, and
  EngineThreadsRunning counts them.
- The pacer's D for audio scheduling comes from the real FramePacer.
- QuitPending waits for Durable saves through the lane's QuitProvider.
- The full matrix (-Milestone -Legacy, which includes -Desktop, plus
  wsl:linux-clang-debug and msvc-unpatched-dev).
- ADRs 087 and on.

## M10 Editor shell on the single loop (design build step 8, CF c)

Design sources: docs/design/mt-design.md "Editor" (all of it), "Lifecycle"
(Play.Edit, Play.Playing, Play.Paused, Play.Stepping, Play.Faulted and
their transitions; QuitPending), "Failure Handling", "Platform" (file
dialogs, ImGui's global context, imgui_impl_sdlgpu3 hazards, SDL_GPU
release on D3D12), the API sketch, build step 8; docs/design/
costfree-end-state.md "Editor", "Dear ImGui (docking branch)" and "Kept From
Todays Code" (the editor pieces to port). The legacy editor in app/editor
is the source of the ported pieces; it is read, not changed.

M10 runs as two tracks that meet in the panel interface below: M10a (ImGui,
documents, commands, Play, faults, recovery, quit, the editor executable)
and M10b (the panels). Asset browser, tilemap painter and animation
preview are M19 (they need its content types); save/load beyond what Play
and recovery need, prefabs and the real component-removal command are M11.

### The panel interface (the contract between M10a and M10b)

`engine/se_editor/include/se_editor/panel.h`, written by M10a first and
committed early (M10b copies it verbatim onto its branch until the merge):

```cpp
// Read-only state a panel draws from, valid for one frame.
struct PanelContext {
  const World* world = nullptr;        // the document, or the play World
  bool playing = false;                // Play.Playing / Paused / Stepping
  bool faulted = false;                // Play.Faulted: read-only, no edits
  const Selection* selection = nullptr;
  EditorCommands* commands = nullptr;  // null while Playing or Faulted
  TextureIds* textures = nullptr;      // Handle<Texture> -> ImTextureID,
                                       // resolved every frame (never stored)
  const EditorStats* stats = nullptr;  // pacing, schedule, render, ForkStats
  LogView* log = nullptr;              // the main-owned formatted log ring
  double real_delta_sec = 0.0;
};
class Panel {
 public:
  virtual ~Panel() = default;
  virtual std::string_view Name() const = 0;   // the window title
  virtual void Draw(const PanelContext& context) = 0;
};
class EditorCommands {                 // every Edit-mode mutation
 public:
  virtual ~EditorCommands() = default;
  virtual void Submit(std::unique_ptr<EditorCommand> command) = 0;
  virtual void BeginCoalesce(std::string_view label) = 0;  // a drag
  virtual void EndCoalesce() = 0;
  virtual void Select(std::span<const Guid> guids) = 0;
};
```

`EditorCommand` (se_editor/commands.h, M10a): Apply(World&) and Revert(World&)
returning std::expected<void, Error>, plus a label; the kinds are set,
add and remove a component (before/after JSON through the
ComponentRegistry), create and destroy an entity (stored serialized), and
path-point commands (insert, move, delete, the waypoint canvas's).
`TextureIds` wraps GpuResources (M7) and the ImGui backend: panels hold
`Handle<Texture>` and ask for an ImTextureID each frame.

### M10a: ImGui, documents, commands, Play, faults, recovery, quit

**New module engine/se_imgui** (STATIC; Dear ImGui v1.93.0-docking from
.deps, already declared; the only target that compiles ImGui):
- imconfig override: `GImGui` is `thread_local` (the documented
  override), so any ImGui call off main fails at once; asserts on;
  imgui_stdlib; the first-party backends imgui_impl_sdl3 and
  imgui_impl_sdlgpu3 from the archive.
- The renderer integration with M7: ImGui draws in the swapchain pass after
  the composite blit; PrepareDrawData runs before the pass; ImGui's vertex
  and index buffers are pre-sized and growth is logged (its growth calls
  SDL_WaitForGPUIdle); ImGui texture updates submit before the frame's
  command buffer. A debug wrapper of RenderDrawData asserts that no texture
  it binds was released this frame.
- A checks rule (planted violation): no `ImTextureID` or `SDL_GPUTexture*`
  member in se_editor (extends M7's check_gpu_members.py).
- Fonts: the dynamic font atlas; gamepad navigation on; multi-viewports
  opt-in and off by default; one DockSpace seeded once with DockBuilder and
  persisted to the pref path.

**New module engine/se_editor** (STATIC; deps se_runtime, se_imgui,
se_render, se_assets, se_audio, game modules through GameModule):
- `EditDocument`: a World that is never stepped, opened from a scene file
  (M3's loader), with its own undo stack, selection and dirty flag. Panels
  get `const World&` after the frame's ticks, in Edit and Play, so there is
  one inspector path and no mirror.
- Commands and undo: every Edit-mode change is an EditorCommand; undo and
  redo; drags coalesce into one command. Document edits apply after
  ImGui::Render and before the RenderList build, so they show in the same
  frame; commands that release GPU resources (document close, viewport
  resize) apply at the next frame start. A headless test fails on any
  Edit-mode mutation made outside commands (the World's mutation gate,
  M2, closed while in Edit mode outside command application).
- Play controls take effect between frames and are logged with their tick:
  - Play serializes the document into a fresh play World (a serializer
    round trip); the bytes go to the lane as a recovery file and are kept
    as the Play-start bytes (M12 attaches them to hang dumps); gameplay
    assets freeze until Stop (visual ones may hot reload, logged in the
    SessionLog);
  - Pause stops stepping, sets prev := current once and resets the
    accumulator; Step runs exactly one tick;
  - Stop destroys the play World at the next frame start, stopping its
    voices first, and asserts the document hash is unchanged;
  - edits during Play queue in the SessionLog against the next tick (at
    once while paused) and are discarded at Stop;
  - 'Play game' runs the GameModule's GameStateStack from its start state.
- Play.Faulted: an exception in a play tick or in the play World's
  RenderList build (both fault boundaries, ADR-006 list extended) freezes
  the play World read-only, renders it at alpha 1 on a guarded path with
  edits disabled, saves the replay, the SessionLog and the breadcrumbs to
  the pref path, and requests a dump; Stop leaves Faulted for Edit.
- Pacing in the editor: Standard with frame latency 2 by default; editor
  Play uses the Overloaded floor of 0 ticks (M2's `editor_play`); Idle
  (waitevent) after 1 s without input in Edit mode, woken by input or
  WakeMain; a 60 Hz preview toggle.
- Recovery and autosave: dirty documents autosave after 2 s with no input
  (Background lane jobs, unique temp and rename); the next start offers
  recovery files; WM_QUERYENDSESSION writes recovery files synchronously.
- Quit: two-phase (M5's QuitController): a prompt and a progress modal run
  over normal frames, saves are Durable lane jobs, a failed save keeps its
  document dirty and cancels the quit, and quit waits while a file dialog
  is open.
- File dialogs: SDL_ShowOpenFileDialog / SaveFileDialog with a refcounted
  `DialogMailbox`: the callback (SDL's dialog thread) copies the paths in,
  calls WakeMain (inside the ADR-047 window) and returns; main acts at the
  next iterate. A callback that arrives after the editor started quitting
  or after the mailbox's owner is gone is safe (the refcount keeps the
  mailbox alive; nothing is touched but the mailbox).
- The editor's own input: ImGui first, then the InputService only while
  the viewport has focus in 'Play game'; raw keyboard off (IME, hotkeys).

**apps/editor: the simple_editor executable** (SE_DEFINE_SDL_APP like
rampage): opens rampage's sandbox scene by default, with `--scene FILE`,
and the test hook `--editor-script FILE` (test builds): a line-based script
of editor actions run at frame boundaries (open, select, set component
JSON, add/remove component, create/destroy, path-point ops, undo, redo,
play, pause, step N, stop, play-game, inject-throw tick|render, wait-ticks
N, wait-frames N, dialog-callback PATHS [from-thread], quit, expect-hash
document|play VALUE, expect-faulted, expect-dirty, dump-hash FILE). Every
desktop test drives the editor through scripts; none uses real input.

**Tests (M10a):**
- headless (everywhere, TSan in WSL where threads are involved): commands
  apply/revert for every kind, undo∘redo is the identity on the document
  hash, coalescing, the no-mutation-outside-commands check, Play/Pause/
  Step/Stop state machine, Stop's document hash, SessionLog edits applied
  at tick boundaries and discarded at Stop, Play.Faulted from a tick throw
  and from a render-build throw (inspectable afterwards), the dialog
  mailbox from another thread including after quit started (TSan),
  autosave and recovery through the lane;
- desktop (unfocused window, scripts only): the editor opens the sandbox,
  plays 300 ticks, pauses, steps, stops with an identical document hash;
  a tick throw and a render throw each leave the editor alive and the
  World inspectable; quit with a dirty document saves through the lane
  (throttled disk) and exits; a foreign-thread dialog callback at quit;
- replay: an editor session recorded through a script replays headless
  (se_replay) to the same per-tick hashes.

ADRs 095-099.

### M10b: the panels (engine/se_editor/panels)

Panels are classes deriving from `Panel`; they read `PanelContext` and
change state only through `EditorCommands`. Ported pieces keep the legacy
behaviour named in docs/design/costfree-end-state.md "Kept From Todays
Code", rebound to the new model (no sf::, no live registry writes):
- Hierarchy (Guid, name, parent; selection through the commands).
- Inspector: reflection-generated drawers from the ComponentRegistry's
  inspect hooks, plus the ported custom drawers for effects, team and
  stats (app/editor/EditorComponentVisitor.cpp, with imgui_stdlib in place
  of the const_cast buffer at :39-41) and the waypoint canvas
  (app/editor/Editor.cpp:677-931): its path-editing rules (:828-885) and
  controls (:903-930: left-click adds a point, right-drag pans, right-click
  opens a menu, the wheel zooms in 0.1 steps within 0.1-10) become path
  commands, with command tests for each rule. Every edit is a command;
  a value drag coalesces into one.
- Viewport: an M7 ViewportTarget shown with ImGui::Image through
  TextureIds; pan and zoom; picking (the broadphase scratch when its valid
  tick is current, else exact AABBs ordered by layer and z; a faulted World
  picks by brute force); a 2D move gizmo as an ImGui overlay whose drag
  previews and commits one coalesced command; the ported entity overlays
  (Editor.cpp:538-620) and letterbox math (:451-464, :632-656).
- Log: lines formatted once into a main-owned ring fed by a log consumer
  (M4's LogWorker panel sink), with severity colours and layout ported from
  Editor.cpp:380-427 and dropped-record counts; never re-reads the file.
- Profiler: per-system ring buffers with the ported axis rule
  max(avg*1.5, peak*1.1, 1.0) (EditorSceneSystemVisitor.cpp:29); pacing
  (mode, cadence, B, frame latency, misses, HalfRate); overload and
  dilation badge; render stats; ForkStats when M14's pool runs (join
  p99.9, missed wakes, fallbacks); a flight-recorder dump button.
- Schedule: serial and parallel time per phase and each fork site's state.
- Stress generator: the ported enemy archetype (Editor.cpp:966-980) as a
  prefab-like spawn spec, spreading spawns across ticks through commands
  (Edit) or the SessionLog (Play), logged.
- Scene settings: camera, tick rate display, background colour.
- Tests (headless, no window): each panel draws in an ImGui context with a
  fake PanelContext (a World built by the test, a recording
  EditorCommands) and synthetic ImGui IO events (io.AddMousePosEvent,
  AddMouseButtonEvent, AddMouseWheelEvent, AddKeyEvent; never OS input):
  the waypoint canvas rules each produce the expected path commands;
  inspector edits produce set-component commands with correct before/after
  JSON; a drag coalesces; panels never mutate the World (the test World's
  mutation gate is closed); the log panel shows records and drop counts;
  the profiler's axis rule.

ADRs 100-103.

### Exit (M10)

- Stop leaves an identical document hash.
- Tick and render exceptions leave the editor alive and the World
  inspectable (Play.Faulted).
- A recorded editor session replays headless to the same hash.
- A foreign-thread dialog callback is safe, including at quit.
- Every Edit-mode change goes through commands (the headless check), the
  ported waypoint canvas rules hold as command tests, and the editor opens,
  plays and stops the sandbox in a window (I check a screenshot).

## M12 Watchdog (design build step 9)

Design sources: docs/design/mt-design.md "Threads" (watchdog), "Lifecycle"
(Hung, Running -> Hung -> Running), "Failure Handling", "Determinism And
Debugging", build step 9; ADR-043 (M4's skeleton: hang declaration off).

- se_crash watchdog: hang declaration on. A hang is 5 consecutive 1 s
  periods (on the unbiased clock: QueryUnbiasedInterruptTime / CLOCK_
  MONOTONIC, which exclude system sleep) with no progress of main's iterate
  counter, while not Suspended, not Modal, without a heartbeat and without
  a debugger. Armed after the first SDL_AppIterate; until then only the 30 s
  boot budget applies (M4). Joined last.
- An in-driver flag: main sets it around SDL_GPU acquire, present and
  fence waits (and the pacer's waitable and fence waits); a hang during it
  is labelled a GPU or compositor stall in the dump and the log.
- One out-of-process dump per hang episode (DumpWithoutCrash from the
  watchdog thread), carrying the registered streams, a new session-header
  stream (scene, seed, tick rate, build id, start tick: what a replay needs)
  and, in the editor, the Play-start bytes (M10; registered as a stream
  when Play starts). The episode ends when progress resumes (Hung ->
  Running, logged with its length).
- The heartbeat: long work on main that is known (a large synchronous
  load, a blocking save at session end) raises it; nothing else does.
- The shutdown deadline (M4 integration) stays: 5 s after the Durable
  drain, dump request, log drain, termination. Release builds' fast exit
  after the final flush (M5) stays.
- `tools/se_replay --from-dump DUMP`: reads the session-header stream and
  the InputFrame ring (tools/minidump/read_dump.py --json, or a C++ reader
  of the same layout) and replays the session headless to the breadcrumb's
  tick; with the hang test hook it reports reaching the hanging tick with
  the per-tick hashes of the ticks before it.
- Test hooks: `--hang-test after-ticks:N` (test builds) puts main in an
  infinite loop inside a system at tick N.
- Tests:
  - desktop (rampage, unfocused): the injected hang yields an
    out-of-process dump within 6 s of the hang's start, with main's
    breadcrumb at tick N, decoded by read_dump.py; se_replay --from-dump
    replays to tick N;
  - unit (everywhere, TSan in WSL): the HangDetector with a fake clock:
    5 periods to declare; Modal, Suspended, heartbeat and a debugger
    each prevent it; a slow boot under 30 s and system sleep (the unbiased
    clock does not advance) trigger nothing; one dump per episode;
  - teardown: the CI's App tests tear down in under 2 s each (measured and
    gated in the callback tests);
  - sleep/resume and a real debugger attach stay a gates.md row (manual
    procedure in tests/platform/watchdog_manual.md).

ADRs 104-106.

### Exit (M12)

- An injected infinite loop yields an out-of-process dump within 6 s that
  replays to the hanging tick.
- Modal, Suspended, a debugger (faked), a slow boot and system sleep (fake
  unbiased clock) trigger nothing; real sleep/resume is a gates.md row.
- CI teardown under 2 s.

## M14 ForkJoinPool threaded, still disabled (design build step 10)

Design sources: docs/design/mt-design.md "Threads" (fork-join workers),
"Job System" (all of it), "Measurements" (fork-join overhead, tails,
determinism across worker counts, unexplained wake stalls), "Risks"
(fork-protocol bug, wake stalls, hybrid CPUs), build step 10.

- se_core jobs: the threaded ForkJoinPool replaces M2's inline-only one
  behind the same API; ParallelFor runs the same fixed chunks inline when
  a site is disabled, inside a fallback window, below minItems, or at
  W = 0, so results are identical.
  - Workers: role kForkWorker, started with se_os StartThread (HighQoS,
    soft affinity through CpuSetRequest), one per physical core up to
    ForkPoolSize (M4's CpuTopology); PerformanceOnly sites use
    PerformanceOnlyCount. No worker exists until a site is enabled.
  - The wake protocol: each worker has its own padded wake word; main does
    fetch_add(release) and notify_one for participants - 1 workers; the
    worker loop loads the wake word, tries to claim, then wait(old), so no
    bump is lost; after a join a worker spins at most 20 us when another
    fork is predicted (never on battery), then parks.
  - The descriptor and the remaining count on separate cache lines; large
    sites claim in batches; participants = min(W + 1, chunks / 4).
  - Per-chunk outputs grown from per-chunk arenas and concatenated in chunk
    order; per-chunk DebugDraw slices; World-level sinks assert
    not-in-fork; nested forks run inline; the first exception is rethrown
    on main after the join (a fork chunk is a fault boundary, ADR-006).
  - ForkStats per site: join p50, p99, p99.9 and max, missed wakes,
    fallbacks, participants; the fallback rule (a join slower than 4x its
    median runs the site inline for 5 s, then retries; counted).
  - Calibration: a 10 ms dummy fork per EfficiencyClass at startup when the
    pool starts.
  - Chaos mode (`SE_CHAOS_SEED`): seeded claim permutation, yields, delays
    between claim and descriptor read and between last write and
    decrement, injected late wakes.
- Sites stay disabled in every shipped configuration (the enabling rules
  are M16's); tests enable them explicitly.
- Tests:
  - TSan property tests in WSL: no lost wake (a worker never sleeps
    through a bump; stress with chaos seeds), every chunk runs exactly
    once, joins see every chunk's writes, exceptions propagate, stop joins;
  - determinism: identical per-tick hashes for W = 0/1/3/7 and 8 chaos
    seeds, including pile-up scenes (all colliders in one cell), with the
    candidate sites of rampage's systems enabled in the test (movement,
    broadphase, narrowphase, RenderList cull/keys/vertices where they are
    written as fork sites; write the ones that are not yet sites);
  - M17a's pending check: the threaded sim driver's hashes equal the inline
    driver's at W = 1 and 3 (remove the gates.md row);
  - bench: fork overhead (empty and 50 us chunks) and the join tails at
    W = 1/3/7, recorded in se_bench (Release, label bench, thresholds from
    an idle run);
  - the 12-29 ms wake stalls: a soak (label slow, 10 minutes, Release) that
    records every wake's latency with QPC stamps and reports p99.9 and max;
    either explain stalls seen (with the stamps, Windows Performance
    Recorder `wpr.exe`, which is built into Windows, or a Tracy capture if
    present) or bound them (no wake over 2 ms in the soak); record it in an
    ADR and gates.md.

ADRs 107-110.

### Exit (M14)

- TSan property tests green, including no lost wake.
- Hashes identical for W = 0/1/3/7 and 8 chaos seeds, including pile-ups.
- The threaded sim driver matches inline at W = 1 and 3.
- The wake stalls are explained or bounded by a trace or a soak.
- No site is enabled in any shipped configuration.

## Integration (M10a + M10b + M12 + M14)

- M10b's panels in M10a's editor: the dock layout, every panel live, the
  viewport through ViewportTarget and TextureIds; the editor's desktop tests
  with the panels open.
- M12 in the editor: the Play-start bytes as a dump stream while Playing; a
  hang in editor Play dumps and the editor-session replay reaches the
  hanging tick.
- M14's ForkStats in the profiler and schedule panels.
- The full matrix (-Milestone -Legacy, which includes -Desktop, plus
  wsl:linux-clang-debug and msvc-unpatched-dev).
- ADRs 111 and on.

## M11 Save/load, prefabs, hot reload, component removal (CF d)

Design sources: docs/design/costfree-end-state.md "Scenes, content and game
flow" (scenes, prefab instances with overrides), "Serialization,
reflection and config" (one RFC 7386 merge patch per component, arrays
replaced whole, no nested prefabs, versioned formats, atomic writes),
"Editor" (documents for scenes and prefabs, every change a command, "Hot
reload polls file timestamps. Save writes the same files rampage loads"),
"Tests, determinism and CI" (load-save-load equality for every scene
fixture, prefab overrides, undo then redo is the identity, the editor and
the runtime load a scene into identical Worlds), "Kept From Todays Code"
(cd906a37b: component removal restored as a real registry command; the
stress generator); docs/design/mt-design.md "Editor" (documents, edits,
resource-releasing commands at the next frame start, gameplay assets frozen
in Play) and "Audio And Assets" (hot reload); ADR-095 to ADR-103, ADR-112.

What exists: the scene loader already reads prefab references and
"overrides" (games/rampage/src/scene.cc CreateEntity), but SaveScene
flattens every entity, so the editor's Save (EditDocument::Serialize ->
EditorGame::save_scene) writes the sandbox back without its prefabs; the
World keeps no record of which prefab an entity came from. M10a's
RemoveComponentCommand already removes a component (ledger row "Component
removal in the inspector", open M11); what is missing is what a removal
means for prefab instances and saved files, and the components it must
take with it. The editor holds one document (Editor::document_). M11
closes the M10/M12/M14 review carry-overs:

- a stat name typed in the Stats or effect drawers lives in
  StatDrawer::typed_names_ (se_editor_panels), not in the World's
  StatNames, so SaveScene writes that stat in registry form and the name is
  lost (ADR-100); the name must reach the document through a command and
  the file through the writer;
- the stress generator keeps one coalesce scope open across frames in
  Edit mode (ADR-102); a hidden dock tab leaves it open (its Draw does not
  run), and a document change mid-press sends the remaining batches to the
  new document (ADR-112, Consequences).

M11 runs as two tracks that meet in the scene contract below: M11a (the
formats: game_rampage's scene writer, prefab documents, the prefab link)
and M11b (the editor: several documents, prefab-aware commands and
panels, hot reload into documents, the carry-overs).

### The scene contract (between M11a and M11b)

Written by M11a first and committed early; M11b copies the declarations
verbatim onto its branch until the merge.

`engine/se_core/include/se_core/core_components.h` (M11a, additive):

```cpp
// The prefab an entity is an instance of: its reference relative to the
// asset root ("prefabs/enemy.prefab.json"). Registered "PrefabInstance",
// kSerialized | kEditorVisible, not kHashed: linking or unlinking an
// entity changes no tick result and no golden. The scene writer writes it
// as the entity's "prefab", never under "components"; the loader sets it
// and nothing else may (it is reserved like IdComponent, Name, Spawned).
struct PrefabInstance {
  std::string prefab;
};
```

`games/rampage/include/game_rampage/scene.h` (M11a, additive; the
existing LoadScene and the SaveScene default keep their behaviour):

```cpp
enum class SceneWriteMode : std::uint8_t {
  // Every entity's components whole (M3's SaveScene): the Play-start
  // bytes, fault files and dumps, which must load without prefab files.
  kFlatten,
  // Prefab instances as "prefab" plus "overrides" (one RFC 7386 merge
  // patch per component against what loading the bare instance gives;
  // null removes a member, or the component), other entities as
  // "components": Save, Save As, autosave and recovery files.
  kPrefabs,
};

struct SceneWriteOptions {
  SceneWriteMode mode = SceneWriteMode::kFlatten;
  // kPrefabs: prefab references resolve here, or through |resolver|.
  std::string asset_root;
  PrefabResolver resolver;
};

[[nodiscard]] std::expected<std::string, Error> SaveScene(
    const World& world, std::string_view name,
    const SceneWriteOptions& options = {});

// SceneLoadOptions gains (additive):
//   // Called once per prefab the load read, with its text (the editor
//   // keeps them: a later change of the file is rebased against them).
//   std::function<void(std::string_view ref, std::string_view text)>
//       on_prefab;

// Prefab documents: a prefab opened in the editor is a World with one
// entity whose Guid is PrefabRootGuid(ref) (a fixed hash of the
// reference, so a prefab document hashes the same everywhere).
[[nodiscard]] Guid PrefabRootGuid(std::string_view ref) noexcept;
std::expected<SceneSummary, Error> LoadPrefabDocument(
    World& world, std::string_view text, std::string_view ref,
    const SceneLoadOptions& options) SE_REQUIRES(g_sim_owner);
// The root entity as se-prefab-v1 (its name, its components in the
// scene form: stats and textures by name).
[[nodiscard]] std::expected<std::string, Error> SavePrefabDocument(
    const World& world, std::string_view ref, std::string_view name);

// One entity's components in the registry's JSON form, by registered
// name, in registration order.
struct EntityComponentsJson {
  Guid guid;
  std::vector<std::pair<std::string, std::string>> components;
};

// What a bare instance of the entity's prefab loads to (the writer's
// baseline, and the Inspector's "Revert to prefab").
[[nodiscard]] std::expected<EntityComponentsJson, Error> PrefabBaseline(
    const World& world, Guid guid, const SceneLoadOptions& options);

// Every instance of |ref| in |world| as it loads once the prefab's text
// changes from |old_text| to |new_text|: each instance's overrides,
// taken against the old text, applied over the new one. Instances whose
// result equals what they hold are left out.
[[nodiscard]] std::expected<std::vector<EntityComponentsJson>, Error>
RebasePrefabInstances(const World& world, std::string_view ref,
                      std::string_view old_text, std::string_view new_text,
                      const SceneLoadOptions& options);

// The components removing |component| takes with it (those FinishLoad
// derives from it: PathCache with Path; ActiveEffects with Stats;
// PairCooldowns with EffectSource), and whether it may be removed at all
// (IdComponent, Name, Spawned, PrefabInstance and Transform may not).
[[nodiscard]] std::span<const std::string_view> RemovedWith(
    std::string_view component) noexcept;
[[nodiscard]] bool Removable(std::string_view component) noexcept;
```

`engine/se_editor/include/se_editor/editor_game.h` (M11b, additive; the
editor executable fills the new members from the functions above):

```cpp
struct EntityJson {
  Guid guid;
  std::vector<ComponentJson> components;
};

struct EditorGame {
  // ... M10a's members; load_scene gains a callback for the prefab texts
  // it read:
  std::function<std::expected<void, Error>(
      World& world, std::string_view text, const std::string& asset_root,
      const std::function<void(std::string_view ref,
                               std::string_view text)>& on_prefab)>
      load_scene;
  // save_scene stays the flattened form (the Play-start bytes, faults).
  // Save, Save As, autosave and recovery: with prefab references.
  std::function<std::expected<std::string, Error>(
      const World& world, std::string_view name,
      const std::string& asset_root)>
      save_scene_file;
  std::function<std::expected<void, Error>(
      World& world, std::string_view text, const std::string& ref,
      const std::string& asset_root)>
      load_prefab;
  std::function<std::expected<std::string, Error>(
      const World& world, const std::string& ref, std::string_view name)>
      save_prefab;
  std::function<std::expected<EntityJson, Error>(
      const World& world, Guid guid, const std::string& asset_root)>
      prefab_baseline;
  std::function<std::expected<std::vector<EntityJson>, Error>(
      const World& world, std::string_view ref, std::string_view old_text,
      std::string_view new_text, const std::string& asset_root)>
      rebase_instances;
  std::function<std::vector<std::string>(std::string_view component)>
      removed_with;
  std::function<bool(std::string_view component)> removable;
};
```

### M11a: the formats (games/rampage, engine/se_core)

- `PrefabInstance` in se_core's core components (above), registered after
  DestroyRequest. Not hashed, so determinism_kernel, determinism_rampage,
  the reference comparisons and every golden are unchanged (a test runs
  them; nothing is re-baselined). The serialization round trip "for every
  registered component" covers it without a new case.
- The loader (scene.cc CreateEntity) adds PrefabInstance{ref} to every
  entity with a "prefab" and reports each prefab text it read through
  SceneLoadOptions::on_prefab; "PrefabInstance" joins the reserved names
  (a scene may not set it under "components").
- The writer, kPrefabs mode, per entity in spawn order:
  - no PrefabInstance: "components" whole, as kFlatten writes them;
  - an instance: "prefab" and "overrides" computed in two passes so that
    a load of the written entity gives exactly the entity:
    1. the scene form of each component (stats and textures by name,
       StatIdsToNames and TextureKeyToName as today) against the
       prefab's component: equal: nothing; absent from the prefab: the
       whole value (as a merge patch it adds the component); absent from
       the entity: `null`; otherwise the RFC 7386 diff (objects member by
       member, arrays and scalars whole, a member the entity lacks as
       null). Components FinishLoad derives (PathCache, ActiveEffects,
       PairCooldowns, MoveIntent, ActionBuffer, Moved) and Transform are
       left out of this pass;
    2. the entity with pass 1's overrides is loaded into a scratch World
       (the loader, R12 and FinishLoad included); each component still
       different from the entity's (Transform, a derived one with state)
       is written as its diff against the scratch entity's. So
       sandbox.scene.json, whose enemies have only Path overrides and
       start at their first point by R12, saves with only Path overrides.
  - "name" as today; Spawned is not written (the load records it).
- kFlatten mode is today's output exactly (a test compares it with the
  M3 writer's on every fixture), so the Play-start bytes, the editor's
  recovery bytes before M11 and se_replay's start scenes keep their form.
- Prefab documents: LoadPrefabDocument creates the root entity
  (PrefabRootGuid(ref)) with the prefab's components, settings defaults
  in the ctx (ApplySceneSettings with the default bounds), FinishLoad;
  SavePrefabDocument writes se-prefab-v1 with the root's components in
  scene form, the derived ones left out (the same rule as pass 1).
- PrefabBaseline and RebasePrefabInstances are built on the same pieces
  (load a bare instance into a scratch World; overrides = the writer's two
  passes against the old text; apply them over the new text). The rebase
  of an instance whose overrides remove a component the new prefab no
  longer has keeps it removed; one that overrides a member the new prefab
  renamed keeps the override (it becomes an added member): merge patches
  are applied as written, never interpreted.
- NameTable gains `Remove(id)` (stats.cc), for the editor's undo of a
  name it added (M11b).
- RemovedWith and Removable: the table above, written once beside
  FinishLoad so the two never disagree (a test checks that every component
  FinishLoad adds is listed under the component it derives from).
- Tests (tests/rampage, label unit, everywhere):
  - `scene.Load, save and load again give identical Worlds for every
    scene fixture` (kFlatten and kPrefabs; the sandbox scene, the two
    prefabs, tests/fixtures/scenes/*.scene.json written for M11: an
    instance with a removed component, one with an added component, one
    overriding a nested member, one with stats added by name, an entity
    without a prefab, a Camera2D, a SpriteRenderer tint): equal Hash,
    equal Digests, equal kFlatten text of the two Worlds;
  - `scene.The sandbox scene saves back to its own content`: kPrefabs
    output parsed as JSON equals assets/scenes/sandbox.scene.json parsed
    (only Path overrides; no Transform, no derived component);
  - `scene.Overrides survive a prefab change`: an instance overriding
    Stats.Speed and its Path; the prefab's HP and Speed change; the
    rebase gives the new HP and the overridden Speed and Path;
  - `scene.A removed prefab component saves as null and stays removed`
    (also across a rebase);
  - `scene.Flatten mode writes what M3's writer wrote` (byte equality on
    every fixture);
  - `scene.A prefab document saves back to its file` (both prefabs,
    parsed JSON equality) and `scene.A prefab root's Guid is fixed`;
  - `scene.Every component FinishLoad adds is removed with its source`;
  - the determinism and reference suites unchanged (the existing tests,
    no re-baselining: PrefabInstance is not hashed).

ADRs 114-116.

### M11b: the editor (engine/se_editor, engine/se_editor/panels, apps/editor)

**Documents.**
- `se_editor/documents.h`: `DocumentSet` holds every open EditDocument
  and the active one; each document has a kind (kScene, kPrefab), an id
  that is never reused, its own undo stack, selection, dirty flag,
  autosave timer and recovery file (M10a's per-document parts move into
  it). File > Open adds a document (or activates the one already open on
  that path); File > Close closes the active one (the M10a open prompt
  becomes the close prompt: Save, Don't Save, Cancel); a strip of
  document tabs in the Document panel switches the active one; Save All.
  Play plays the active scene document (Play is disabled on a prefab
  document); while a Play session exists the set is frozen: no open,
  close or switch (the panels show the play World anyway).
- The quit prompt lists every dirty document (Save All, Don't Save,
  Cancel); Save All submits each document's save as a Durable lane job;
  one failure keeps that document dirty, shows its error and cancels the
  quit; the others are saved (M10a's rule per document).
- `EditDocument::ContentHash()`: the hash of the document's kFlatten text
  (EditorGame::save_scene). The World hash covers kHashed components only,
  so SpriteRenderer, Camera2D, IdComponent, PrefabInstance and the ctx
  name tables are invisible to it; undo, redo and Stop's check use both.
  Stop asserts that both are unchanged.
- Saves use save_scene_file (kPrefabs) or save_prefab; autosave and
  recovery files keep the prefab references (se-recovery-v1 gains the
  document's kind; v1 files read as scenes, no migration needed since the
  field defaults). The Play-start bytes, fault files and dump streams stay
  flattened (save_scene): a replay or a dump must never depend on prefab
  files that may change after it was written.
- Opening a prefab document: load_prefab; its hierarchy is its one root
  entity; Save writes the prefab file; the Inspector edits it like any
  entity (commands; undo).

**Commands and the carry-overs.**
- A queued command records the id of the document it was submitted for
  (`Queued::document`); the frame's apply step applies it to that
  document, and drops it (logged) when that document was closed. A change
  of the active document never redirects an edit.
- A coalesce scope belongs to the document and to the panel that opened
  it (the shell knows the panel being drawn, Editor::drawing_panel_):
  the shell ends the scope at the frame's apply step when that panel did
  not draw in the frame (its tab hidden, its window closed), when the
  document is closed or deactivated, and at Play. Ending it is logged.
  This closes the hidden-tab case for every panel (the gizmo's drag too).
- The stress generator in Edit mode submits a press as one CommandGroup
  of create-entity commands in the frame of the press: no scope spans
  frames (ADR-102 amended; Play keeps its spread over ticks through the
  SessionLog). The spawns are prefab instances: each carries
  PrefabInstance{"prefabs/enemy.prefab.json"} and the full components (as
  M10b's spec built them), so a saved scene writes each spawn as the
  prefab plus its Path and, where R12 does not place it, Transform. A
  bench row records the cost of one 1000-spawn press in msvc-dev
  (`editor_panels.A 1000-enemy press applies in one frame`, label slow;
  the time is logged, the test fails above 250 ms).
- Stat names: `StatNameCommand` (se_editor_panels/stat_name_command.h,
  CommandKind::kGame, a new enumerator for commands a game-aware module
  defines): Apply records the name in the document World's StatNames
  under the kEditorCommand gate (it remembers whether the name was new);
  Revert removes a name it added (NameTable::Remove); ToSession gives no
  entries (stat ids are name hashes, so the play World needs no name; the
  Play-start bytes carry the names in the scene's own form). The Stats and
  effect drawers submit a new stat as a CommandGroup {StatNameCommand,
  SetComponentCommand}: one undo step. typed_names_ goes; StatName() reads
  the World's StatNames only.
- Removal: the Inspector's remove submits a CommandGroup of
  RemoveComponentCommands for the component and every component
  `removed_with` names that the entity holds (undo restores all of them);
  a component `removable` refuses has no close button (and the script's
  `remove` fails on it with the reason). On a prefab instance the saved
  file then holds `"<component>": null` in the overrides (M11a); the
  ledger row "Component removal in the inspector" closes as ported.
- "Revert to prefab" (Inspector, per component of an instance): a
  set-component command to the baseline's value (an add when the instance
  had removed it, a remove when the prefab lacks it); the Inspector marks
  the components that differ from the baseline (computed once per frame
  for the selected entity only).

**Hot reload into documents.**
- Each open document watches its own file and the prefab files its load
  reported (BackgroundLane::Watch with an editor tag; the completions
  reach the editor through AssetRuntime's foreign handler, M10a). A
  change is read on the lane (an Urgent read job) and compared by content
  with the text the document holds; equal content is ignored.
- A prefab that changed: every open scene document holding instances of
  it gets one undo step "Prefab changed: <ref>": a CommandGroup of the
  set, add and remove commands that turn each instance into
  rebase_instances' result. The document's recorded prefab text becomes
  the new one (not part of undo: after an undo the instance differs from
  the file's prefab, which the next save writes as overrides, which is
  what the user then has). Saving a prefab document rebases the other open
  documents at once from the save's completion (the scan's later report of
  the same content is a no-op).
- The document's own file changed by another program: a clean document is
  reloaded (logged; its undo stack starts again); a dirty one gets a
  prompt (Reload, Keep mine); Keep mine leaves the document dirty so the
  next save wins.
- During Play these wait for Stop (M10a's freeze of gameplay assets; the
  watch completions are kept and handled at Stop, in order).

**Panels.**
- Hierarchy: a prefab column (the reference's file name, "-" for none) and
  a marker on instances with overrides.
- Inspector: per-component override markers, Revert to prefab, removal as
  above; the prefab document's root shows no prefab column.
- Viewport, Log, Profiler, Schedule and Scene settings are unchanged.

**apps/editor**: fills the new EditorGame members from M11a's functions;
`--scene FILE` may name a prefab (kind by its "format").

**Script verbs** (editor_script.h): `open FILE` (adds or activates),
`activate NAME|INDEX`, `close [save|discard|cancel]`, `save-all`,
`expect-documents N`, `expect-content-hash document V|initial|play-start`,
`revert-to-prefab GUID COMPONENT`, `expect-prefab GUID REF|none`,
`expect-override GUID COMPONENT [yes|no]`, `stat-name GUID NAME VALUE`
(adds a stat by name through the drawer's command group),
`expect-stat-name NAME [yes|no]`, `write-file FILE TEXT` (writes a file
through se::fs from the script: an external change for the hot-reload
tests; never a user's file), `wait-reload`, `answer-reload reload|keep`.

**Tests (M11b):**
- headless (everywhere): `editor.Undo then redo is the identity on the
  document hash and content hash` for every command kind including
  StatNameCommand, the removal groups, Revert to prefab and a prefab
  rebase (each also through `undo` x N then `redo` x N over a mixed
  sequence of 50 commands); `editor.Commands apply to the document they
  were made for`; `editor.A coalesce scope ends when its panel stops
  drawing`; `editor.A stat typed in the Stats drawer is saved by name`
  (save, reopen, the name is in StatNames and the file has the named
  form); `editor.Removing a component removes what derives from it`;
  `editor.A saved prefab updates its instances in an open scene`;
  `editor.A prefab changed on disk is one undo step per document`;
  `editor.A changed document file reloads when clean and asks when
  dirty`; `editor.Play-start bytes stay flattened`; recovery files with
  prefab references round-trip;
- editor_panels (headless ImGui): the drawers' new-stat path submits the
  group; the Hierarchy's prefab column; the Inspector's override markers
  and revert; the stress generator's one-frame press with the tab hidden
  in the next frame (nothing is left open) and `editor_panels.A
  1000-enemy press applies in one frame` (slow);
- scripts (headless, run_editor.py): `editor.script.prefabs` (open the
  sandbox and the enemy prefab, change the prefab's HP, save it, the
  sandbox's enemies follow, an instance's override survives, undo, redo,
  save the sandbox, its file has only overrides), `editor.script.documents`
  (three documents, switch, close with the prompt), `editor.script.
  hot_reload` (write-file on a prefab and on the document), `editor.
  script.stat_names`, `editor.script.stress_prefabs` (a press in Edit
  mode, save, the file's spawns are prefab instances);
- desktop (unfocused window, scripts only): `editor.desktop.prefabs` (the
  prefab script in a window; the Viewport's readback changes after the
  prefab's texture region change).

ADRs 117-121.

### Exit (M11)

- Undo followed by redo is the identity on the document hash and on its
  content hash, for every command kind, the prefab rebase included.
- Prefab overrides: an instance keeps its overrides across a prefab change,
  a removed component stays removed and saves as null, and the sandbox
  saves back to its own content.
- The editor and the runtime load identical Worlds from every scene
  fixture (equal Hash, Digests and flattened text; the test is in the
  integration, which has both loaders on one branch).
- A stat name typed in the editor is saved; no coalesce scope outlives its
  panel's frame or its document.
- Prefab documents, several documents and hot reload into documents work
  in a window (a gates.md row: the owner checks a screenshot).

## M13 rampage runtime (CF e)

Design sources: docs/design/costfree-end-state.md "Scenes, content and
game flow" (Title, Gameplay, Pause and GameOver states that own Worlds and
request transitions; the deferred pushdown semantics; "the editor is never
a scene"), "Runtime and main loop", "Assets and content pipeline", "Kept
From Todays Code" (the SceneManager semantics; the MainMenu's wallpaper);
docs/design/mt-design.md "Lifecycle" (QuitPending, Faulted in the game
build, the Shutdown Order), "Audio And Assets" (a World never steps
before its assets are resident; bounded lane steps; hot reload),
"Determinism And Debugging" (GameState transitions at tick boundaries,
red team C6; what determines the simulation), "Tests And Tooling" (quit
durability, shutdown stress); docs/design/dedicated-driver.md "Lifecycle"
(the runtime pushes its own Pause on focus loss; Loading waits for its
scope); ADR-079, ADR-081, ADR-089, ADR-092.

What exists: rampage runs one state, "sandbox", which loads its scene in
OnEnter with LoadSceneFile, a synchronous std::ifstream read on main
inside a tick boundary (games/rampage/src/module.cc), so neither M9's "a
World never steps before its scope is resident" nor the lane's bounded
reads cover scenes and prefabs; the AssetRegistry's Prefab and SceneChunk
stores have no caller. The module's start state is "sandbox", which the
editor's Play (ADR-097), se_replay and every rampage golden depend on.
GameStates cannot read input or content; systems request transitions
with RequestGameState. The ledger's open row "Escape replaces the
Sandbox with MainMenu" is M13's.

M13 runs as two tracks that meet in the content contract below: M13a (the
engine: content through the lane, the stack's content gate, the runtime's
start state, focus-loss and empty-stack rules, replays and dumps of
multi-state sessions, the quit and shutdown tests) and M13b (the game:
rampage's four states, its flow rules, their scenes and the executable).

### The content contract (between M13a and M13b)

Written by M13a first and committed early; M13b copies it verbatim.

`engine/se_sim/include/se_sim/content_source.h` (new):

```cpp
// The files a GameState builds its Worlds from (scenes, prefabs), by path
// relative to the content root. The runtime's source is the AssetRuntime's
// text assets: a state is created only once its files are loaded and its
// assets resident (the stack's content gate), so Text() never touches a
// disk on the owner thread. Tools and tests read synchronously.
class ContentSource {
 public:
  virtual ~ContentSource() = default;
  // The text of |path|; an Error when it is not loaded or does not exist.
  [[nodiscard]] virtual std::expected<std::string_view, Error> Text(
      std::string_view path) const = 0;
};

// Reads through |read| on first use and keeps the text (se_replay, tests).
// A content bundle (content_bundle.h) is the other source tools use.
class CachedContentSource final : public ContentSource {
 public:
  using Reader =
      std::function<std::expected<std::string, Error>(std::string_view)>;
  explicit CachedContentSource(Reader read);
  [[nodiscard]] std::expected<std::string_view, Error> Text(
      std::string_view path) const override;

 private:
  Reader read_;
  mutable std::map<std::string, std::string, std::less<>> texts_;
};
```

`engine/se_sim/include/se_sim/game_state.h` and `game_module.h`
(additive):

```cpp
struct StateEnv {
  // ... M2's members
  // Where the state's Worlds come from; null: the state reads nothing
  // (M2's tests) or reads files itself (the sandbox state before M13).
  const ContentSource* content = nullptr;
};

// Asked before a push or replace creates its state (owner thread, at a
// tick boundary).
class StateContentGate {
 public:
  virtual ~StateContentGate() = default;
  // Whether the state |state_id| may be created now: its files loaded and
  // its assets resident.
  [[nodiscard]] virtual bool Ready(std::string_view state_id)
      SE_REQUIRES(g_sim_owner) = 0;
};

class GameStateStack {
 public:
  // ... M2's members
  // A push or replace whose state |gate| does not report ready stays
  // queued, with every command behind it (FIFO); ApplyPending returns and
  // Waiting() names the state until a later boundary applies it. A null
  // gate (replays, tests) applies at once, so the same commands apply
  // before the same tick either way.
  void SetContentGate(StateContentGate* gate) noexcept;
  // The state the queued push or replace at the front waits for (World
  // requests collected first); empty when nothing waits.
  [[nodiscard]] std::string_view Waiting() const noexcept;
};

struct GameModule {
  // ... M2's members
  // The state a player's session starts in (rampage: "title"); the
  // runtime and the editor's 'Play game' start here. start_state stays
  // the one-World state editor Play, se_replay and the goldens use.
  std::string_view game_start_state;
};
```

`engine/se_runtime/include/se_runtime/app.h` GameSetup and
`asset_runtime.h` StateAssets (additive):

```cpp
struct StateAssets {
  // ... M9's members
  // Files loaded as text through the lane before the state is created,
  // relative to the content root, and every file text_dependencies names
  // for them.
  std::vector<std::string> texts;
};

struct GameSetup {
  // ... M5-M10's members
  // The state the App pushes at boot (empty: the module's
  // game_start_state, else its start_state).
  std::string start_state;
  // The files a loaded text needs too (rampage: a scene's prefabs), by
  // path relative to the content root.
  std::function<std::vector<std::string>(std::string_view path,
                                         std::string_view text)>
      text_dependencies;
  // Focus lost while a session runs: the GameState command the game wants
  // (rampage: push "pause" when "gameplay" is on top), queued in the
  // SessionLog against the next tick, so a replay pauses where the live
  // session did.
  std::function<std::optional<StateCommand>(std::string_view top_state)>
      on_focus_lost;
};
```

### M13a: the engine (engine/se_sim, engine/se_runtime, engine/se_assets, tools/se_replay)

- se_assets: a Text store (`TextAsset`, `RequestText(scope, path)`,
  `Text(handle)` returning the file's bytes as a shared immutable string)
  read on the lane in steps of at most 1 MB like every read, watched by
  the hot-reload scan, under SetGameplayFrozen's freeze like prefabs. The
  Prefab and SceneChunk stores stay as they are, without a consumer
  until M19's importer (decision 6 below; a ledger row records it).
- AssetRuntime is the StateContentGate and the runtime's ContentSource:
  - Ready(state) creates the waiting state's AssetScope the first time it
    is asked (StateAssets of the state: textures, sounds, texts and the
    texts' dependencies found as they land), and answers true once every
    entry is resident or failed; HoldTicks holds while Waiting() is not
    empty, as it holds for a scope that is not resident (no tick runs, the
    hold is logged), so asset timing changes no tick;
  - Text(path) serves the loaded texts of the scopes on the stack (and of
    the waiting one);
  - a text file changed while a state that read it runs: the running
    Worlds are not touched (gameplay content never changes under a running
    World in the runtime); the new text serves the next state that loads
    it, and that state's entry is logged in the SessionLog as kAssetSwap
    with the content hash before the command that creates it (logged: "a
    changed text applies to the next state that loads it").
- The App: pushes GameSetup::start_state (else game_start_state, else
  start_state) at boot step 10 with the gate set; StateEnv::content is the
  AssetRuntime. After a frame's ticks, an empty stack starts the
  two-phase quit (logged "the game ended: no state left"). Focus loss
  (the lifecycle's FocusLost, M5) while ticks run calls on_focus_lost and
  enqueues the command as a kGameStateCommand SessionLog entry against
  the next tick. A failed load of a state's text is a boot or state error:
  the state is not created, the error is logged with its path, line and
  column, and the game takes the fault path (game build: dump, message
  box, quit).
- Replays and dumps of multi-state sessions:
  - `se_sim/content_bundle.h`: `se-content-bundle-v1`, a JSON object
    {format, texts: [{path, hash, text}]}: every text a session's states
    loaded, each version once; `ContentBundleSource` serves it (a path's
    newest version, or the one a kAssetSwap entry names by hash).
  - The App's `--record` replay takes the bundle as its start bytes when
    the session ran more than its start scene (otherwise the scene file,
    as today, so existing replays and goldens are unchanged): the
    recorder's start bytes are set at shutdown, before the Durable save.
  - The runtime's dumps carry the SessionLog: the App registers ADR-111's
    stream (the session's replay file without frames: header, start
    bytes, SessionLog) and attaches it again before a frame's ticks when
    the log gained an entry (the editor's rule; until M13 the runtime's
    log was always empty).
  - se_replay: start bytes that are a bundle run through a
    ContentBundleSource from `--start-state` (default the module's
    game_start_state); `--from-dump` does the same for a runtime dump
    whose stream holds a bundle.
- The quit and shutdown tests the design lists, runtime side (the
  editor's N-documents case is in the integration, which has M11b's
  document set):
  - `platform.quit_durability.replay_throttled` (ADR-089's test extended):
    `--record` on a throttled disk (`--durable-save-test`'s injector, 3 s):
    QuitPending until the rename, the file complete and equal to the
    recorder's bytes, the log's "quit waits for 1 Durable lane job";
  - `platform.quit_durability.deadline_after_drain`: a Durable drain of
    7 s (injector), then the teardown: the watchdog's 5 s shutdown deadline
    did not fire (no dump, the log says the deadline started after the
    drain), teardown under 2 s after the drain;
  - `platform.quit_durability.failed_durable_save`: a rename refused past
    2 s (the injector's sharing violation): the game build logs the failure
    and quits (decision 5 below); the editor keeps M10a's cancel;
  - `platform.shutdown_stress` (label slow; msvc-dev, msvc-asan,
    linux-clang-asan and linux-clang-tsan): 100 headless rampage runs
    (`--headless --audio none --fixed-frame-dt 8.3333`) from seeded
    frames 1-600, quitting by `--frames`, by an SDL_EVENT_QUIT pushed with
    SDL_PushEvent and by SDL_EVENT_TERMINATING; every run exits 0, logs
    "teardown took" under 2 s, and reports no engine thread left
    (`--expect-no-threads`, test builds: ThreadRegistry counts none
    before SDL_AppQuit returns).
- Tests (everywhere, TSan in WSL where threads are involved):
  `sim.A push waits at the content gate and applies before the same tick`
  (per-tick hashes equal with a gate that holds for 0, 1 and 7 frames and
  without one); `sim.Commands behind a waiting push keep their order`;
  `sim.A content bundle replays a multi-state session`;
  `assets.Text assets load in bounded steps and hot-reload`;
  `runtime.The start state is created once its texts are resident`
  (headless App: the log line, no tick before it); `runtime.A changed
  scene applies to the next state that loads it` (kAssetSwap logged, the
  running World's hash unchanged); `runtime.Focus loss is a logged state
  command` (an SDL window focus event pushed with SDL_PushEvent; the
  replay pauses at the same tick); `runtime.An empty stack quits the
  game`; `crash.rampage_dump_with_session_log` (a dump after a focus-loss
  pause replays to its tick with se_replay --from-dump).

ADRs 122-124.

### M13b: the game (games/rampage, assets/scenes)

- States (games/rampage/src/flow_states.cc; ids in game_rampage/flow.h):
  - "title" (game_start_state): the Title World from
    assets/scenes/title.scene.json: the legacy MainMenu's wallpaper (the
    "bg" texture over the arena, app/scene/MainMenu.cpp), nothing else
    until M19's text and UI;
  - "gameplay": the Sandbox World from the configured scene (by default
    assets/scenes/sandbox.scene.json), loaded through StateEnv::content
    (LoadScene with a resolver over the same source, so prefabs come from
    the loaded texts);
  - "pause": an overlay World over the paused Gameplay
    (assets/scenes/pause.scene.json: the wallpaper tinted to a dark,
    half-transparent veil); its World steps, the Gameplay below does not
    (push pauses the top), and both draw (RenderSources: bottom at alpha
    1);
  - "game_over": assets/scenes/game_over.scene.json (the wallpaper
    tinted red);
  - "sandbox" stays exactly as it is (the module's start_state: editor
    Play, se_replay, every golden), except that it reads through
    StateEnv::content when the environment gives one.
- `FlowSystem` (the last system of GameRules; reads the tick's
  InputFrame): it acts only in a World whose ctx holds a `FlowContext`
  with a role (the four states put one there; the sandbox state does
  not), so every existing golden and reference comparison is unchanged
  (the tests run them). The rules, one transition per tick at most, all
  applying at the next tick boundary:
  - Title: Enter replaces it with "gameplay"; Exit pops it (the stack is
    empty: the game ends, M13a);
  - Gameplay: Exit pushes "pause"; no PlayerControl entity left alive
    (destroyed, or carrying DestroyRequest, the legacy HP <= 0 rule)
    replaces it with "game_over";
  - Pause: Exit pops it (Gameplay resumes); Jump pops it and replaces
    Gameplay with "title" (two commands, FIFO);
  - GameOver: Enter replaces it with "gameplay" (a fresh World from the
    scene); Exit replaces it with "title".
  The ledger row "Escape replaces the Sandbox with MainMenu" closes as
  re-specified: Escape opens Pause; Pause's Jump returns to the Title (the
  legacy MainMenu); written into docs/rules/gameplay-rules.md as a
  declared rule.
- `game_rampage/content.h`: `SceneDependencies(path, text, asset_dir)` (a
  scene's prefab references as content-root paths), what the executable
  gives GameSetup::text_dependencies.
- games/rampage/app/main.cc: start state "title"; state_assets per state
  (each state's scene as a text, the textures it draws, the sounds of the
  table); text_dependencies; on_focus_lost (push "pause" over
  "gameplay"); `--scene FILE` sets the Gameplay scene; `--start-state ID`
  (default "title") lets tests start in "gameplay" or "sandbox".
- The window title shows the top state's id (it shows the tick and the
  lifecycle state today).
- Tests (tests/rampage everywhere; desktop on this machine):
  - `rampage.flow.*` (headless, a GameStateStack with the module and a
    CachedContentSource): each rule above from synthetic InputFrames;
    one transition per tick; GameOver on the player's destroy request;
    Pause's two-command return; the sandbox state's World has no
    FlowContext;
  - `determinism_rampage_flow` (label determinism; goldens per platform
    and compiler, tests/golden/*/determinism_rampage_flow.txt): a
    recorded session (tests/fixtures/flow/flow.seinput, written by a
    checked-in generator through StampedInput) from Title through
    Gameplay, Pause, Gameplay, the player's death on
    tests/fixtures/flow/doomed.scene.json (the player at HP 10 on an
    enemy's path), GameOver, a retry and back to the Title, under 30, 60,
    144 and 160 Hz synthetic clocks and FMA3 on and off: identical
    per-tick hashes, including the transition ticks;
  - `rampage.headless_flow` (the executable headless, `--push-key` at
    frames: Enter, Escape, Escape, then the doomed scene's death, Exit):
    the log's state sequence and the replay (`--record`) reproduced by
    se_replay from its bundle;
  - `rampage.desktop.flow` (an unfocused window, the same push-keys,
    `--readback` in each state: Title, Pause and GameOver images differ
    from Gameplay's and from each other; no real input);
  - `rampage.frames_300` and the M4/M12 crash tests keep their meaning
    with `--start-state sandbox` (their tick counts and hashes are the
    sandbox's), and `rampage.frames_300` runs once more from "title"
    (every state's scene now loads through the lane).

ADRs 125-127.

### Exit (M13)

- rampage plays the Sandbox from files: Title, Gameplay, Pause and
  GameOver load their scenes and prefabs through the background lane, no
  World steps before its content is resident, and a recorded session
  through every state replays headless to the same per-tick hashes under
  30/60/144/160 Hz clocks.
- No ImGui in rampage (the link-graph check, unchanged).
- Quit durability: the runtime tests above, and the editor's
  N-documents test in the integration (M11b's document set), pass;
  shutdown stress passes under ASan and TSan.
- Playing it with a keyboard and a gamepad is a gates.md row (a person).

## Integration (M11a + M11b + M13a + M13b)

- `editor.The editor and the runtime load identical Worlds` (headless,
  every scene fixture and the sandbox): EditDocument::Open against the
  "gameplay" state's World from a headless App boot of rampage on the
  same file (through the lane and the gate): equal Hash, Digests and
  flattened text, equal StatNames and TextureNames. Then the reverse
  direction: a scene the editor saved (kPrefabs) plays in rampage from the
  saved file (`rampage.plays_an_editor_saved_scene`: the stress script
  saves a scene with spawned prefab instances; rampage `--start-state
  gameplay --scene` it; its first 600 tick hashes equal se_replay's on
  the editor's Play-start bytes of the same document).
- The editor reads documents and Play game's content through the
  AssetRuntime's texts where M13a made them available; 'Play game' starts
  at game_start_state ("title") with the Gameplay scene pointed at the
  Play-start bytes (EditorGame::set_play_game_scene, now the gameplay
  state's scene), and its states load through the gate like the
  runtime's.
- The design's quit durability test, editor side:
  `editor.script.quit_durability` (three dirty documents, a scene and two
  prefabs, on a throttled disk): QuitPending until all three are renamed
  into place, the bytes on disk equal each document's Serialize, one save
  failed by the injector keeps its document dirty, shows its error and
  cancels the quit while the other two are saved; a second quit with no
  failure exits; the shutdown deadline starts after the drain.
- rampage's sounds play on its flow's rules unchanged (the audio tests).
- docs: README (the editor's documents and prefabs, rampage's states and
  keys, the content bundle), the ledger (component removal ported; the
  Escape row re-specified; the MainMenu wallpaper and the GameData
  serializer stubs recorded), gates.md rows.
- The full matrix (-Milestone -Legacy, which includes -Desktop, plus
  wsl:linux-clang-debug and msvc-unpatched-dev).
- ADRs 128-130.

## M17b Dedicated sim driver, integrated (design build step 13, part 2)

Design sources: docs/design/dedicated-driver.md (all of it: "Threads",
"Frame Timeline", "Data Flow", "Snapshot And Interpolation",
"Synchronization", "Lifecycle" and its shutdown order, "Failure
Handling", "Editor", "Input And Latency", "Assets Audio And Physics",
"Determinism And Debugging", "Changes To Cost Free End State");
docs/design/mt-design.md build step 13 ("an editor mirror for the Play
panels; inline stays the default wherever it holds the frame rate") and
"Editor" ("Escalation cost: a dedicated simulation thread would need a
Play-mode mirror for these panels"); ADR-083 to ADR-086 (M17a), ADR-097,
ADR-099, ADR-106, ADR-107, ADR-111, ADR-112.

What exists: M17a's SimThread, PacedClock, InputFolder, SnapshotChannel,
the World-to-slot fill and SnapshotRenderSource, proved headless (the
threaded hashes equal the inline ones at W = 0, 1 and 3 on every golden,
ADR-083, M14). The App accepts only `--sim-driver inline`
(app_config.cc). Everything the App does around a tick is written for
the inline driver on main: the TickGate (AssetRuntime, main only), the
AssetRuntime's scopes that follow the GameStateStack it reads on main,
the AudioBridge draining World audio events, the session header's last
hash, the dump's InputFrame ring and SessionLog stream, and in the editor
the PlaySession, the panels' const World, FillStats and FillForkStats
(ForkJoinPool::Sites() and the Schedule's SystemStats are the tick owner's
data, read on main because the editor ticks inline, ADR-112). The
watchdog counts main's iterates only, so a sim thread stuck in a system
would never be declared a hang.

M17b runs as two tracks that meet in the threaded host contract below:
M17b.1 (the threaded driver in the App, for rampage and every App) and
M17b.2 (the editor's Play on the sim thread and its mirror).

### The threaded host contract (between M17b.1 and M17b.2)

Written by M17b.1 first and committed early; M17b.2 copies it verbatim.

`engine/se_core/include/se_core/snapshot.h` (additive):

```cpp
inline constexpr std::size_t kMaxTickSystems = 64;
inline constexpr std::size_t kMaxTickForkSites = 32;

// One system's cost in the tick, copied out of the Schedule's ring on the
// sim thread.
struct TickSystemCost {
  std::uint32_t system = 0;   // the Schedule's index
  std::uint32_t ns = 0;
  std::uint32_t fork_ns = 0;  // inside threaded forks (ADR-112)
};

// One fork site's record as the pool kept it (the fields of the editor's
// ForkSiteStats), copied at most every 100 ms.
struct ForkSiteCopy {
  std::array<char, 48> name{};
  bool enabled = false;
  std::uint64_t inline_until_ns = 0;
  jobs::ForkStats stats;  // trivially copyable (asserted)
};

// The tick's costs, for profilers: copied out of the slot, never pointed
// into. Part of the render part (every published slot).
struct TickStats {
  std::uint32_t systems = 0;
  std::array<TickSystemCost, kMaxTickSystems> cost{};
  Tick sites_tick = 0;  // when |site| was copied (0: never)
  std::uint32_t sites = 0;
  std::uint32_t active_workers = 0;
  std::array<ForkSiteCopy, kMaxTickForkSites> site{};
};

// The GameStateStack after a tick that changed it (the platform's asset
// scopes follow it).
struct StackChange {
  std::uint32_t revision = 0;
  std::vector<std::string> states;  // bottom to top
};

// Snapshot gains `TickStats stats;`. ReliableDeltas gains
// `std::vector<StackChange> stack_changes;` and
// `std::vector<std::string> content_waiting;` (a queued push waits for
// that state's content: Waiting(), M13a).
```

`engine/se_sim/include/se_sim/sim_thread.h` (additive):

```cpp
enum class SimOp : std::uint8_t {
  // ... M17a's ops
  // Destroy the session on the sim thread and go idle (the editor's Stop;
  // Quit still ends the thread).
  kStopSession,
  // A state's texts (content), then its residency: the session's gate
  // reports the state ready once both arrived.
  kStateContent,
  kStateResident,
  // Focus lost while running: the session asks GameSetup::on_focus_lost
  // with its top state and logs the command (M13a's rule, on the sim).
  kFocusLost,
  // The display period is a whole multiple of dt: the wake phase target.
  kSetWakePhase,
};

struct SimCommand {
  // ... M17a's members
  // kStateContent, kStateResident: the state id.
  std::string state_id;
  // kStateContent: the texts, immutable from now on.
  std::shared_ptr<const ContentBundle> content;
  // kSetWakePhase.
  std::int64_t phase_ns = 0;
  // kStartGame: what the factory builds (the editor's Play-start bytes);
  // empty for a game's start.
  std::vector<std::byte> start_bytes;
};

class SimSession {
 public:
  // ... M17a's members
  // Before each tick: true holds this tick and the rest of the burst (the
  // threaded TickGate: a state waiting for content, a scope not resident).
  [[nodiscard]] virtual bool HoldTick(Tick /*next*/) { return false; }
  // Ops M17a's thread does not handle (kStateContent, kStateResident,
  // kFocusLost, the editor's): applied at the tick boundary, in order.
  virtual void OnCommand(SimCommand& /*command*/) {}
  // After a slot was filled (the editor makes its mirror here).
  virtual void AfterPublish(const Snapshot& /*slot*/) {}
  // A tick threw: the session's last look before it is destroyed on the
  // sim thread (the editor freezes its mirror). Must not throw; the
  // editor's own boundary contains what it calls.
  virtual void OnFault() noexcept {}
};
```

`engine/se_runtime/include/se_runtime/threaded_host.h` (new): the platform
side both the App and the editor drive.

```cpp
// What one iterate received from the sim (reliable sections applied in
// tick order).
class SimReceiver {
 public:
  virtual ~SimReceiver() = default;
  virtual void OnTickRecord(const TickRecord& record) = 0;
  virtual void OnAudioCue(const AudioCue& cue, std::uint64_t anchor_ns) = 0;
  virtual void OnSessionEvent(const SessionEvent& event) = 0;
  virtual void OnStackChange(const StackChange& change) = 0;
  virtual void OnContentWaiting(std::string_view state_id) = 0;
  virtual void OnScopeRelease(const ScopeRelease& release) = 0;
  virtual void OnEndedReplay(std::span<const std::byte> replay) = 0;
};

struct ThreadedHostConfig {
  SimSessionFactory factory;
  PacedClockConfig clock;
  // Test builds: the sim's deadlines advance with the App's frame clock
  // (--sim-clock frame), so threaded tests are as repeatable as inline
  // ones under --fixed-frame-dt.
  bool frame_clock = false;
};

class ThreadedHost {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ThreadedHost>, Error>
  Start(ThreadedHostConfig config) SE_REQUIRES(g_main);
  ~ThreadedHost();  // Stop()

  // Main, at iterate start: every published slot into the render source,
  // its reliable section to |receiver| in tick order.
  void Receive(SimReceiver& receiver) SE_REQUIRES(g_main);
  // The slots received so far; Pick(frame_ns) gives the frame's pair and
  // alpha (M17a's SnapshotRenderSource).
  [[nodiscard]] SnapshotRenderSource& Render() noexcept;
  // The newest slot's TickStats (null before the first).
  [[nodiscard]] const TickStats* LatestStats() const noexcept;
  [[nodiscard]] bool Send(SimCommand command) SE_REQUIRES(g_main);
  [[nodiscard]] InputEventRing& Input() noexcept;
  [[nodiscard]] std::uint32_t AckedSeq() const noexcept;
  [[nodiscard]] const FaultBox& Fault() const noexcept;
  // --sim-clock frame: advance the sim's clock to |frame_ns| (main).
  void AdvanceTo(std::uint64_t frame_ns) SE_REQUIRES(g_main);
  // The design's shutdown steps 1-4: stop routing input, Quit, keep
  // pumping (waits of at most 5 ms) until the thread finished, drain and
  // return every slot. The App's 5 s deadline covers it.
  void Stop() noexcept;
};
```

### M17b.1: the threaded driver in the App (engine/se_runtime, engine/se_sim, engine/se_core, engine/se_crash, tools/se_replay)

- `--sim-driver inline|threaded` (AppConfig; default inline, and an
  unknown value is a boot Error naming the choices). Threaded on a machine
  with fewer than 3 physical cores still runs threaded (an explicit
  request) and logs that the design recommends inline there. Test builds:
  `--sim-clock frame`.
- `GameSimSession` (se_runtime/game_sim_session.h): the game's SimSession:
  the GameStateStack, SimDriver (the game's Schedule and pool: the pool's
  owner thread is now the sim thread, which ADR-107's "owner thread only"
  allows; its workers start there when a site is enabled), SessionLog,
  ReplayRecorder, a `SnapshotAudioSink` (World audio events become the
  slot's AudioCues), FillRender through AppendRenderView per drawn World
  with GameSetup::camera (a pure function of a const World, run on the sim
  thread), TickStats after each tick, the content gate (a
  StateContentGate fed by kStateContent and kStateResident; Ready answers
  from them; HoldTick holds while the stack waits or the top's scope is
  not resident) and StateEnv::content (the bundles the platform sent:
  immutable shared data, the design's ScopeLoaded).
- The App in threaded mode: no FixedStepper ticks; each iterate receives
  (SimReceiver: tick records to the session header's last hash and the
  frame report, audio cues to `AudioBridge::Schedule(cue, anchor_ns)` at
  anchor + D, stack changes to the AssetRuntime, which now follows a
  StackChange list instead of reading the stack, content waits to the
  AssetRuntime's scopes, whose texts and residency go back as
  kStateContent and kStateResident; scope releases free GPU objects only
  after every slot with a tick below release_tick is returned and the
  retire queue's fence passed), picks the pair at the frame start and
  builds the layers with BuildSnapshotRenderList (M17a), then presents
  through the FramePacer as before. `InputRouter` (se_runtime/
  input_router.h): InputService's mapping without its latch: one press and
  one release per physical input (ADR-086) into the InputEvent ring,
  stamped on the engine's monotonic clock at pump time; focus loss sends
  ReleaseAll and kFocusLost; a heartbeat each iterate; a full ring
  coalesces axes and counts drops. The lifecycle: Suspended sends Suspend
  and the restore Resume (the sim re-bases); Modal keeps the sim ticking
  (the reason the design builds it); the display period sets the wake
  phase when it is a whole multiple of dt.
- Dumps and replays: RunTick writes the dump's InputFrame ring on the sim
  thread (its only writer). ADR-111's Play-start / SessionLog stream
  becomes a single-writer, write-ahead stream the session owns
  (`integration::SessionLogStream`): the sim appends an entry's bytes and
  release-stores the committed length before the entry applies, the
  record keeps the seqlock form of the InputFrame ring, and read_dump.py
  and se_replay's reader take the committed part. So no applied entry is
  ever missing from a dump; an entry queued and not yet applied can be,
  and a replay to the dump's tick never needs it (ADR-111's consequence,
  which the threaded driver would otherwise make a divergence: the sim
  applies an edit and runs on before main attaches the stream again).
  The inline driver uses the same stream (one format).
- The watchdog (se_crash): a second progress source, the sim's tick
  counter, armed while a session runs and is not paused, suspended,
  holding or stopping; 5 consecutive 1 s periods without a tick declare a
  hang ("hang N: the sim made no progress for X s at tick T"), with the
  sim thread's breadcrumb; one dump per episode; main's rule unchanged.
  `--hang-test` in threaded mode loops on the sim thread.
- Shutdown order: the design's steps 1-4 (ThreadedHost::Stop) run before
  shutdown step 5; the Worlds are destroyed on the sim thread (its
  session's end); the trace (ADR-088) names "sim". The 5 s shutdown
  deadline starts after the Durable drain, as before, and covers the
  join.
- se_replay `--sim-driver threaded`: the session runs on a SimThread with
  a ManualClock (auto-advance) and recorded frames (RecordedFrame), with
  `--jobs W` as before.
- The thread-safety analysis: every platform-side function of the
  threaded path is SE_EXCLUDES(g_sim_owner); clangcl-dev's
  -Werror=thread-safety covers them. (Editor documents keep their
  InlineOwnerScope on main: the capability is per thread, the owner
  assert per World.)
- Tests (everywhere; TSan in WSL headless, SDL's dummy video):
  - `determinism.threaded.*`: se_replay `--sim-driver threaded` against
    the committed goldens (determinism_kernel, determinism_rampage,
    determinism_rampage_flow, the three reference scenarios over 3600
    ticks, an editor session replay) at `--jobs 0`, `1` and `3`: every
    per-tick hash equal to the golden;
  - `runtime.threaded.*` (headless App, `--sim-clock frame
    --fixed-frame-dt 8.3333`): boot, Title to Gameplay with --push-key,
    Pause, GameOver, quit; the `--record` replay replayed inline by
    se_replay gives the recorded hashes; Suspend and Resume re-base (no
    catch-up); a state waiting for content holds the sim (no tick before
    residency); focus loss pauses through the sim; scope releases free
    GPU objects only after their slots return (a counting fake); AppInit
    failure at every step with the threaded driver is clean (msvc-asan);
  - property tests (label unit, TSan in WSL, seeded, thousands of runs):
    the content handoff (kStateContent then kStateResident in any order
    relative to ticks: the push applies before the same tick as inline),
    the write-ahead SessionLog stream (a reader at random points sees a
    committed prefix of whole entries, never a torn one), TickStats
    copies (complete per slot), and M17a's SnapshotChannel properties
    re-run with the App's consumer pattern (Receive each iterate, keep 3);
  - `crash.rampage_hang_threaded` (desktop): `--sim-driver threaded
    --hang-test after-ticks:240`: a dump within 6 s naming the sim, its
    breadcrumb at tick 240, se_replay --from-dump reaching tick 240;
  - `platform.shutdown_stress.threaded` (slow; msvc-asan, linux-clang-asan,
    linux-clang-tsan): M13a's 100 runs with `--sim-driver threaded`;
  - `rampage.desktop.threaded` (an unfocused window, 600 frames: the
    readback shows the sprites; the log shows the sim's ticks and no
    dropped publishes at idle);
  - `runtime.The inline driver is the default`.
- Bench (msvc-release): `bench.threaded_platform_frame_10k` (the
  platform's frame CPU from Receive to the layers at 10k sprites: the
  design's "platform frame CPU at 144 Hz" gate; threshold the measured p95
  with 2x headroom) beside M17a's bench.snapshot_publish_10k.

ADRs 131-135.

### M17b.2: the editor's Play on the sim thread (engine/se_editor, engine/se_editor/panels, apps/editor)

- Edit mode is unchanged in both drivers: documents are main's Worlds
  (dedicated-driver.md "Editor": documents on the platform, the sim
  owns only Play-session Worlds; no World is ever shared).
- `PlaySession` splits into its core (the stack, driver, log and
  recorder, built and destroyed on the thread that owns them) and two
  hosts: inline (M10a's, unchanged) and threaded (`ThreadedPlay`): Play
  sends StartGame with the Play-start bytes in `start_bytes` and a factory
  that builds the core on the sim thread; Pause, Resume and Step{1} are
  SimCommands; Stop is kStopSession (the sim destroys the play World on
  its thread, the slot's ended replay becomes last-play.sereplay; the
  document's hash and content hash are unchanged by structure and still
  asserted); edits (PlayEdits) become kApplySessionEntry commands;
  'Play game' starts the module's stack from game_start_state the same
  way, with the InputRouter fed by the editor's view rules (ADR-113).
- The Play mirror: panels keep reading a `const World*` (one inspector
  path, mt-design "Editor"). In threaded Play it is a mirror World: the
  sim clones the play World after a published tick (World::Clone plus
  `EditorGame::clone_context`, which copies the ctx resources panels read:
  rampage's ArenaBounds, EffectSettings, SceneSeed, StatNames,
  TextureNames) into one of two slots of a `MirrorChannel`
  (se_editor/mirror_channel.h: the SnapshotChannel's protocol with two
  World slots, published and returned rings, one owner per slot); main
  reads the newest published mirror for the frame and returns the older
  one; the sim reuses or destroys returned Worlds on its own thread, so
  Worlds still die on their owner. A mirror is made after every tick
  while paused or stepping, after a tick that applied an edit, and at most
  every 33 ms while running (`kMirrorPeriodNs`); the panels label the
  values with the mirror's tick (PanelContext gains `world_tick`).
  `bench.mirror_clone_5k` (msvc-release) measures a clone of the sandbox
  with 5000 enemies on the sim thread; its p95 is a threshold, and above
  1 ms (an eighth of a tick) the integration moves to the design's
  selected-entities payload instead (decision 7 below).
- Drawers in threaded Play keep showing the value they submitted while
  its command is unacknowledged (`PlayEdits::Pending(guid, component)`:
  the command's sequence above AckedSeq), then the mirror's again, so a
  value drag does not jump back a mirror period.
- The Viewport in threaded Play draws the session from the snapshots
  (interpolated, the App's path), the overlays and picking from the mirror
  (ADR-101's exact path; positions are those of the mirror's tick).
- Stats: in threaded Play, FillStats takes the systems' costs and each
  phase's parallel time from the newest slot's TickStats, and
  FillForkStats reads TickStats' site copies (the carry-over: the pool's
  records are the sim's data from Play to Stop). The profiler's rings
  keep 1024 entries copied out of slots, on the frame's wall-time axis.
- Play.Faulted in threaded Play: the sim's fault boundary catches the
  throw (M17a); OnFault makes one more mirror of the faulted World inside
  the editor's RunContained (play_fault_boundary.cc, already a fault
  boundary) before the session is destroyed, so the panels inspect the
  World the throw left, as M10a's Play.Faulted does; if that clone throws,
  the last mirror stays and the fault screen says which tick it shows. The
  replay, SessionLog and breadcrumb come from the ended session's slot and
  the FaultBox. A throw in the platform's snapshot RenderList build is
  contained in the editor's boundary as before and pauses the session.
- Dumps: the threaded session writes the SessionLog stream itself
  (M17b.1's write-ahead stream), so a dump during threaded Play holds every
  applied edit; the editor no longer re-attaches it per frame in either
  driver.
- Tests (everywhere; TSan in WSL headless):
  - every `editor.script.*` test that plays runs a second time with
    `--sim-driver threaded --sim-clock frame` (CTest names
    `editor.script.threaded.*`), with the same `expect-hash` values;
  - `editor.Threaded Play mirrors the play World` (the mirror's hash and
    content equal the sim's World at its tick, sampled over 300 ticks);
    `editor.A mirror is never written while main reads it` (TSan property
    test of MirrorChannel); `editor.Threaded Stop leaves the document
    untouched`; `editor.A threaded tick fault leaves the faulted World
    inspectable`; `editor.Drawers show a pending edit until it is
    acknowledged`; `editor.Fork stats in threaded Play come from the
    slots`;
  - `crash.editor_play_hang_threaded` (desktop): M12's play_hang script
    with the threaded driver: the dump within 6 s, the SessionLog stream
    complete, se_replay --from-dump reaching the hanging tick with every
    edit applied.

ADRs 136-139.

### Exit (M17b)

- Inline and threaded per-tick hashes are equal on every golden replay
  for W = 0, 1 and 3 (determinism.threaded.*), and the editor's scripted
  Play sessions give the same hashes in both drivers.
- SnapshotChannel, MirrorChannel, the content handoff and the
  write-ahead stream pass their property tests; TSan clean in WSL for
  the App's and the editor's threaded paths.
- The editor's Play panels read a mirror in threaded Play (the design's
  "an editor mirror for the Play panels").
- `--sim-driver inline` stays the default for rampage and the editor.
- A person comparing the two drivers' feel, PresentMon's displayed
  latency for both, and the weak machines are gates.md rows.

## Integration (M17b.1 + M17b.2)

- The editor runs on the App's ThreadedHost: 'Play game' and Play in a
  window with the threaded driver (`editor.desktop.threaded`), the dock
  layout and every panel live; the M12 hang test and the M10a fault
  scripts in both drivers.
- The App's and the editor's threaded paths in one TSan run (WSL, the
  headless script tests and runtime.threaded.*).
- se_replay replays an editor session recorded with the threaded driver
  inline, and the reverse.
- docs: README (the switch, `--sim-clock frame`, what differs: input
  latency by design, the mirror's lag), the ledger, gates.md rows, the
  plan's tracking of design changes #1-25 (the dedicated driver's column).
- The full matrix (-Milestone -Legacy, which includes -Desktop, plus
  wsl:linux-clang-debug and msvc-unpatched-dev), once with
  `SE_SIM_DRIVER=threaded` set for the runtime and editor test labels
  (the CTest variants above), once without.
- ADRs 140 and on.

## gates.md rows these milestones add

| Gate | Milestone | Needs | Command / procedure |
|---|---|---|---|
| Documents, prefabs and hot reload in a window, checked by eye | M11 | the owner looking at a screenshot | `out\build\msvc-dev\bin\simple_editor --audio null`: open the sandbox and assets/prefabs/enemy.prefab.json, change the prefab's sprite region or HP, save it, switch to the sandbox: every enemy follows, an enemy with an overridden Path keeps it; remove a component, undo, redo; save the sandbox and look at the file (only overrides); `ctest --test-dir out/build/msvc-dev -L desktop -R "editor.desktop.prefabs"` runs the scripted equivalent |
| rampage played by a person | M13 | a person with the keyboard and a gamepad | `out\build\msvc-dev\bin\rampage --audio null`: Title (Enter or the pad's start), play, Escape opens Pause and Escape resumes, Space in Pause returns to the Title, walk into the enemies until GameOver, Enter retries, Escape at the Title ends the game; switching to another window pauses; with `--record run.sereplay`, `se_replay --module rampage --inputs run.sereplay` reproduces it |
| The threaded driver played by a person | M17b | a person at the machine | `out\build\msvc-dev\bin\rampage --audio null --sim-driver threaded`, then the same without the switch: the feel of input (the design's 4-6 ms more latency) and no visible stutter while dragging the window (the threaded driver keeps ticking in the modal loop) |
| PresentMon: displayed latency, inline against threaded | M17b | PresentMon 2.x (not installed; not approved for download), an elevated shell | `tools/presentmon/run.ps1 -Mode paced -Seconds 20 -PresentMon <path>` with and without `-ExtraArgs "--sim-driver threaded"`; compare MsUntilDisplayed p50/p99 |
| The threaded driver on weak machines | M17b, M15 | a 4-core laptop; a 2P+8E laptop on battery | `se_bench --suite threaded_platform_frame_10k`, then rampage `--sim-driver threaded --log-level debug` for 60 s: dropped publishes, dilations and the sim's wake error p99 from the log |

The existing rows "ThreadSanitizer" (M0, M4, M14, M17) and "Release
bench gates on an idle machine" gain M17b's tests and bench rows.

## Decisions for M11, M13 and M17b (2026-11-17)

Taken when these specs were written; each track records the one it
implements in its ADRs.

1. The editor holds several documents (M11b's DocumentSet): a scene and
   the prefabs it uses can be open together.
2. A prefab change applied to an open document is an undo step, so the
   content hash is always explained by the stack. It marks the document
   dirty only when the document's own file text would change.
3. Until M19's menus, Pause returns to the Title with Jump (Space, the
   pad's south button); Exit resumes Gameplay.
4. rampage persists no game save yet (no consumer reads one). The runtime's
   durability tests use the `--record` replay.
5. A failed Durable save at quit in the game build is logged and the quit
   goes on; the editor keeps M10a's cancel (ADR-089).
6. M9's Prefab and SceneChunk stores are kept, unused, for M19's importer;
   scenes and prefabs load as text through the lane.
7. The editor's Play mirror is a cloned World with panels unchanged, gated
   by `bench.mirror_clone_5k` (p95 at most 1 ms at 5000 enemies). Above
   that the integration moves to the design's selected-entities payload.
8. `--sim-driver threaded` is obeyed on every machine; below 3 physical
   cores the App logs a warning and does not fall back to inline.
