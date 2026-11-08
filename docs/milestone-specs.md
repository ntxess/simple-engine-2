# Milestone specs: M1 (legacy reference) and M2 (headless kernel)

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
- R9 System order is Input → WayPoint → Collision → Event → GameRules → Cleanup (Sandbox.cpp:90-92).
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
