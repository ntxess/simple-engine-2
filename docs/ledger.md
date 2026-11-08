# Completeness ledger

Every legacy feature, rule and editor operation is recorded here as ported,
re-specified (with the rule that replaces it) or deleted (with the reason).
The legacy reference retires when nothing is left open
(docs/design/costfree-end-state.md, step 6).

| Legacy item | Where | Status | Notes |
|---|---|---|---|
| JsonDataStoreSerializer tests (GoogleTest) | tests/test.cpp | deleted (M0) | Included non-existent `../src/core/...` paths and a missing `pch.h`; never built. Serialization is re-specified as ComponentRegistry round-trip tests (M2: tests/kernel/serialization_test.cc). |
| Thread-per-subsystem runtime | core/Engine.cpp:116-243 | open (M1a) | Replaced by one paced loop in the reference; by the owner-thread design in the engine |
| EventBus / TaskScheduler | core/util/EventBus.*, TaskScheduler.* | open (M1a) | No callers; removed from the reference |
| Effect model INSTANT / OVERTIME / TEMPTIMED | core/system/EventSystem.cpp:54-176 | open (M1b, M3) | Rules R1, R7 |
| Per-source collision cooldown | core/component/UpdateEntityPolling.cpp:18-24 | open (M1b, M3) | Rule R2: per (source, receiver) pair |
| Self and same-team contact skips | core/system/CollisionSystem.cpp:78-87 | open (M3) | Characterization test |
| Quadtree broadphase | core/util/QuadTree.cpp | open (M1b, M3) | Rule R3 brute force in the reference; spatial hash in the engine |
| Waypoint path following | core/system/WayPointSystem.cpp | open (M1b, M3) | Rule R6 zero-length segments |
| Scene stack push/replace/pop | core/manager/SceneManager.cpp:40-69 | ported (M2) | se_sim GameStateStack: push pauses the top, replace destroys it, pop resumes the one below; adds a FIFO queue, explicit ops (no replace by default) and tick-boundary application. tests/kernel/game_state_stack_test.cc |
| System ordering and profiler | core/manager/SystemManager.cpp, util/SystemProfiler.cpp | ported (M2) | se_core Schedule: fixed phases, registration order within a phase, a 1024-entry timing ring per system. The editor panel arrives in M10. tests/kernel/schedule_test.cc |
| Resource cache semantics | core/manager/ResourceManager.hpp:64-120 | open (M9) | AssetRegistry; key checked before decode |
| Waypoint canvas editing rules | app/editor/Editor.cpp:828-885, :903-930 | open (M10) | Characterization tests |
| Profiler axis rule | app/editor/EditorSceneSystemVisitor.cpp:29 | open (M10) | |
| Stress generator distributions | app/editor/Editor.cpp:941,948,950 | open (M1a, M10) | Seeded; distributions written down |
| Component removal in the inspector | app/editor/Editor.cpp:508-517 | open (M11) | Restored as a real registry command |
| Game of Life simulation | app/scene/GameOfLifeSim.cpp:51-87 | open (M16) | Sample state and first fork-site candidate |
