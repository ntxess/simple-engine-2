# Completeness ledger

Every legacy feature, rule and editor operation is recorded here as ported,
re-specified (with the rule that replaces it) or deleted (with the reason).
The legacy reference retires when nothing is left open
(docs/design/costfree-end-state.md, step 6).

| Legacy item | Where | Status | Notes |
|---|---|---|---|
| JsonDataStoreSerializer tests (GoogleTest) | tests/test.cpp | deleted (M0) | Included non-existent `../src/core/...` paths and a missing `pch.h`; never built. Serialization is re-specified as ComponentRegistry round-trip tests (M2). |
| Thread-per-subsystem runtime | core/Engine.cpp:116-243 | open (M2, M5) | The reference runs one paced main-thread loop since M1a (Engine::run); the engine gets the owner-thread design |
| EventBus / TaskScheduler / BoundedQueue | core/util/EventBus.*, TaskScheduler.*, BoundedQueue.hpp | deleted (M1a) | Two idle TaskScheduler threads with no callers (only ApplicationContext held an EventBus) |
| Wall-clock effect and cooldown timing | core/system/EventSystem.cpp:95,100,135,144; core/component/UpdateEntity*.cpp | re-specified (M1a) | Ticks of LegacySimTime (120 Hz); authored milliseconds convert with msToTicks = ceil(ms * rate / 1000); the editor edits cooldowns in ticks |
| Effect model INSTANT / OVERTIME / TEMPTIMED | core/system/EventSystem.cpp:54-176 | open (M1b, M3) | Rules R1, R7 |
| Raw `Effects*` from a pending event into the source's EffectsList | core/component/StatusModEvent.hpp:26; core/system/CollisionSystem.cpp:104 | re-specified (M1a) | StatusModEvent copies the effect at contact; the pointer dangled when the inspector erased or the vector grew (app/editor/EditorComponentVisitor.cpp:67-70) |
| Per-source collision cooldown | core/component/UpdateEntityPolling.cpp:18-24 | open (M1b, M3) | Rule R2: per (source, receiver) pair |
| Self and same-team contact skips | core/system/CollisionSystem.cpp:78-87 | open (M3) | Characterization test |
| Quadtree broadphase | core/util/QuadTree.cpp | open (M1b, M3) | Rule R3 brute force in the reference; spatial hash in the engine |
| Waypoint path following | core/system/WayPointSystem.cpp | open (M1b, M3) | Rule R6 zero-length segments |
| Scene stack push/replace/pop | core/manager/SceneManager.cpp:40-69 | open (M2) | GameStateStack keeps the semantics |
| System ordering and profiler | core/manager/SystemManager.cpp, util/SystemProfiler.cpp | open (M2) | Phased Schedule with timing ring |
| Resource cache semantics | core/manager/ResourceManager.hpp:64-120 | open (M9) | AssetRegistry; key checked before decode |
| Waypoint canvas editing rules | app/editor/Editor.cpp:828-885, :903-930 | open (M10) | Characterization tests |
| Profiler axis rule | app/editor/EditorSceneSystemVisitor.cpp:29 | open (M10) | |
| Keyboard polled with isKeyPressed in processInput | app/scene/Sandbox.cpp:111-114 | re-specified (M1a) | LegacyInputFrame per tick (held sampled once, edges from events), recorded and replayed as `.seinput`; the engine gets InputFrame (M2, M8) |
| Escape replaces the Sandbox with MainMenu | app/scene/Sandbox.cpp:97-100 | open (M13) | Read from the tick's input (Exit pressed), only when the Sandbox is the root scene |
| Unseeded randomness | app/editor/Editor.cpp:939-940; app/scene/GameOfLifeSim.cpp:29-30 | re-specified (M1a) | `std::mt19937(seed ^ stream)` (core/util/LegacyRandom.hpp); seed from config "seed" or `--seed`; the editor generator takes stream StressGenerator + press index |
| Stress generator distributions | app/editor/Editor.cpp:941,948,950 | open (M1a, M10) | Seeded; distributions written down |
| Component removal in the inspector | app/editor/Editor.cpp:508-517 | open (M11) | Restored as a real registry command |
| Game of Life simulation | app/scene/GameOfLifeSim.cpp:51-87 | open (M16) | Sample state and first fork-site candidate |
