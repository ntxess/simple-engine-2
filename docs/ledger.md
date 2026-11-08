# Completeness ledger

Every legacy feature, rule and editor operation is recorded here as ported,
re-specified (with the rule that replaces it) or deleted (with the reason).
The legacy reference retires when nothing is left open
(docs/design/costfree-end-state.md, step 6).

| Legacy item | Where | Status | Notes |
|---|---|---|---|
| JsonDataStoreSerializer tests (GoogleTest) | tests/test.cpp | deleted (M0) | Included non-existent `../src/core/...` paths and a missing `pch.h`; never built. Serialization is re-specified as ComponentRegistry round-trip tests (M2: tests/kernel/serialization_test.cc). |
| Thread-per-subsystem runtime | core/Engine.cpp:116-243 | open (M2, M5) | The reference runs one paced main-thread loop since M1a (Engine::run); the engine gets the owner-thread design |
| EventBus / TaskScheduler / BoundedQueue | core/util/EventBus.*, TaskScheduler.*, BoundedQueue.hpp | deleted (M1a) | Two idle TaskScheduler threads with no callers (only ApplicationContext held an EventBus) |
| Wall-clock effect and cooldown timing | core/system/EventSystem.cpp:95,100,135,144; core/component/UpdateEntity*.cpp | re-specified (M1a) | Ticks of LegacySimTime (120 Hz); authored milliseconds convert with msToTicks = ceil(ms * rate / 1000); the editor edits cooldowns in ticks |
| Effect model INSTANT / OVERTIME / TEMPTIMED | core/system/EventSystem.cpp:54-176 | re-specified (M1b); open (M3) | Rules R1 (TEMPTIMED always reverts) and R7 (OVERTIME timing and count) applied to the reference; ActiveEffects implement them (M3) |
| Raw `Effects*` from a pending event into the source's EffectsList | core/component/StatusModEvent.hpp:26; core/system/CollisionSystem.cpp:104 | re-specified (M1a) | StatusModEvent copies the effect at contact; the pointer dangled when the inspector erased or the vector grew (app/editor/EditorComponentVisitor.cpp:67-70) |
| Per-source collision cooldown | core/component/UpdateEntityPolling.cpp:18-24 | re-specified (M1b); open (M3) | Rule R2 in the reference: per (source, receiver) pair, decided once per tick, readyOnStart honoured; the engine's PairCooldowns implement it (M3) |
| Self and same-team contact skips | core/system/CollisionSystem.cpp:78-87 | open (M3) | Characterization test; the reference scenario `effects` has a friendly source next to the player |
| Sprite clamp to the scene bounds in render() | app/scene/Sandbox.cpp:155 | re-specified (M1a, M1b) | A tick step against the configured resolution, so rendering never moves entities (`legacy.selfdet.*` at --fps 30/60/144/160); BoundarySystem after movement since rule R9 |
| Clamp corrections read the position from before the first correction | app/scene/Sandbox.cpp:222-238 (checkBoundary; core/system/BoundarySystem.cpp since R9) | open (M3) | Characterization: a corner correction on one axis is undone by the other axis's `setPosition`, and the clamp uses the rotated AABB size against the centred origin, so sprites can leave the arena (seen in the `stress` and `patrol` projections) |
| A path step is taken in full past the waypoint | core/system/WayPointSystem.cpp:37-51 | open (M3) | Characterization: the tick's whole step follows the current segment even beyond its end; the switch to the next segment happens once the travelled distance exceeds the next waypoint's total, so corners are overshot by up to one step |
| Moved sources have no cooldown | core/system/CollisionSystem.cpp:53-60 | open (M3) | Characterization: an entity with an EffectsList and UpdateEntityEvent (set when WayPointSystem moved it) affects every overlapping receiver on every tick it moved; only polled sources have cooldowns (R2) |
| An EntityStatus without HP is destroyed | app/scene/Sandbox.cpp:129 (`getOr("HP", 0.f) <= 0`) | open (M3) | Characterization: a missing HP stat counts as 0, so the entity is destroyed at the end of its first tick (R10) |
| Repeating paths restart progress, not position | core/system/WayPointSystem.cpp:27-34 | open (M3) | Characterization: at the last waypoint `repeat` resets `currentPath` and `distance` but keeps the sprite where it is, so the path replays offset from there |
| Immediate HP <= 0 destroys inside update | app/scene/Sandbox.cpp:126-136 | re-specified (M1b) | Rule R10: DestroyRequest from GameRules, destroyed in Cleanup at the end of the tick in SpawnIndex order; the engine's World::RequestDestroy / DrainDestroyRequests (M2) |
| Semantic projection and hash | core/util/SemanticProjection.*, core/component/SpawnIndex.hpp | ported (M1a, reference only) | Per tick, per entity in SpawnIndex order: x, y, rotation, HP and sorted stats (%.9g), FNV-1a 64 per tick and per run; `.seproj` text. The engine's WorldHash (M2) is separate; refcmp (M3) compares the two trees through projections |
| Quadtree broadphase | core/util/QuadTree.cpp | re-specified (M1b); open (M3, M10) | Rule R3: the reference tests all pairs in SpawnIndex order; the quadtree remains only for the editor's visualizer (M10 decides its fate); spatial hash in the engine (M3), checked against brute force |
| Waypoint path following | core/system/WayPointSystem.cpp | open (M3) | Rule R6 applied to the reference (zero-length segments skipped); see also the path characterization rows (overshoot, repeat) |
| Scene stack push/replace/pop | core/manager/SceneManager.cpp:40-69 | ported (M2) | se_sim GameStateStack: push pauses the top, replace destroys it, pop resumes the one below; adds a FIFO queue, explicit ops (no replace by default) and tick-boundary application. tests/kernel/game_state_stack_test.cc |
| System ordering and profiler | core/manager/SystemManager.cpp, util/SystemProfiler.cpp | ported (M2) | se_core Schedule: fixed phases, registration order within a phase, a 1024-entry timing ring per system. The editor panel arrives in M10. tests/kernel/schedule_test.cc; the reference runs rule R9's order (Input, WayPoint, Boundary, Collision, Event, GameRules, Cleanup), which M3's phases reproduce |
| Resource cache semantics | core/manager/ResourceManager.hpp:64-120 | open (M9) | AssetRegistry; key checked before decode |
| Waypoint canvas editing rules | app/editor/Editor.cpp:828-885, :903-930 | open (M10) | Characterization tests |
| Profiler axis rule | app/editor/EditorSceneSystemVisitor.cpp:29 | open (M10) | |
| Keyboard polled with isKeyPressed in processInput | app/scene/Sandbox.cpp:111-114 | re-specified (M1a) | LegacyInputFrame per tick (held sampled once, edges from events), recorded and replayed as `.seinput`; the engine gets InputFrame (M2, M8) |
| Escape replaces the Sandbox with MainMenu | app/scene/Sandbox.cpp:97-100 | open (M13) | Read from the tick's input (Exit pressed), only when the Sandbox is the root scene |
| Effect stat name edited through `const_cast` into std::string storage | app/editor/EditorComponentVisitor.cpp:39-41 | re-specified (M1a) | `ImGui::InputText` on the std::string (misc/cpp/imgui_stdlib.h) |
| ImGui frame time passed in milliseconds as seconds | app/editor/Editor.cpp:136 against core/Engine.cpp:143 | re-specified (M1a) | `ApplicationContext::realFrameSeconds`, the measured frame time; `deltaTime` is gone |
| Debug panel Frame Rate slider | app/editor/Editor.cpp:331-333 | re-specified (M1a); open (M10) | Set `ApplicationContext::deltaTime = 1000 / target`, which was both the simulation step and ImGui's frame time (and divided by zero at the slider's 0). Now a presentation frame-rate limit (`window.setFramerateLimit(target)`, 0 = unlimited); the simulation stays at `sim-tick-rate`. The engine editor decides whether it keeps a presentation limit (FixedStepper and the present modes pace frames; M2, M6, M10) |
| Polled-source inspector | app/editor/EditorComponentVisitor.cpp:382-398 | re-specified (M1a, M1b); open (M10) | Edited only the cooldown ("I-Frame duration", milliseconds); the cooldown is edited in ticks since M1a, and a "Ready on start" checkbox was added with rule R2, which honours `readyOnStart` (ignored before, core/component/UpdateEntityPolling.cpp:8-11). The engine inspector edits the cooldown component of M3 (M10) |
| Unseeded randomness | app/editor/Editor.cpp:939-940; app/scene/GameOfLifeSim.cpp:29-30 | re-specified (M1a) | `std::mt19937(seed ^ stream)` (core/util/LegacyRandom.hpp); seed from config "seed" or `--seed`; the editor generator takes stream StressGenerator + press index |
| Stress generator distributions | app/editor/Editor.cpp:941,948,950 (now core/util/StressGenerator.cpp) | open (M10) | Seeded and written down (M1a); see "Stress generator distributions" below. The engine's generator decides its own distributions explicitly (M10) |
| Component removal in the inspector | app/editor/Editor.cpp:508-517 | open (M11) | Restored as a real registry command |
| Game of Life simulation | app/scene/GameOfLifeSim.cpp:51-87 | open (M16) | Sample state and first fork-site candidate |

## Stress generator distributions

The editor's Generate and Replace buttons (Editor::generateEntities, paths from
core/util/StressGenerator.cpp; `legacy_replay --generate N` makes the same
draws). Each press seeds `std::mt19937` with the low 32 bits of
`seed ^ (0x53545200 + press index)`, the press index counting from 0 per
session. With `d = std::uniform_int_distribution<std::uint32_t>(0, W)` (MSVC
STL; W = width = 1920, H = height = 1080, both bounds inclusive), each entity
draws, in this order:

- **Waypoint count** (Editor.cpp:941): `for (j = 0; j <= d(rng) % 10; ++j)`
  redraws its bound before every iteration, including the first. With
  r = d % 10, P(r = 0) = 193/1921 and P(r = k) = 192/1921 for k = 1..9, so
  P(continue after k points) = 192 (10 - k) / 1921 and a path has 1 to 10
  points: P(N = 1..10) = 0.1005, 0.1803, 0.2160, 0.2014, 0.1510, 0.0905,
  0.0422, 0.0145, 0.0033, 0.0004 (mean 3.66). Each entity takes 3N + 1 draws.
- **x** (Editor.cpp:948): `d(rng) % W`: uniform on 0..W-1 except that 0 is
  twice as likely (d = 0 and d = W).
- **y** (Editor.cpp:948): `d(rng) % H`, drawn after x: d folds onto
  0..H-1, so y in 0..W-H (0..840) is twice as likely as W-H+1..H-1
  (841..1079).
- **Fixed** (Editor.cpp:950 and following): the sprite starts at the first
  point; HP 100, Speed 500 px/s (0.5 px/ms before rule R5), team ENEMY,
  repeating path, UpdateEntityEvent; no EffectsList, so generated enemies
  never damage the player. A one-point path never moves.
