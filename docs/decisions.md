# Architecture decision records

Each record states the decision, why, and what it rules out. Records are
append-only; a superseded record says so and points at its replacement.

## ADR-001 Threading model: one owner thread, plus the dedicated driver behind a switch (2026-11-08)

**Decision.** Implement the recommended design in design/mt-design.md: the SDL
main thread owns every World and runs input, the fixed 120 Hz ticks and
rendering; deterministic fork-join workers ship at W = 0; helper threads are a
background I/O lane, a log worker, a watchdog, an input sampler and, on
Windows in Paced mode, a vblank sampler. Also build design step 13, the
dedicated 120 Hz sim-thread driver (design/dedicated-driver.md), behind
`--sim-driver=inline|threaded`. Inline is the default.

**Why.** The owner chose to build both and compare them. The panel ranked the
single owner first at the stated targets, and the split stays the documented
escalation; building it now makes the comparison measurable instead of
argued. Both drivers run the same Schedule through `SimDriver::RunTick`, and
tests require identical per-tick hashes on every golden replay.

## ADR-002 Dependencies through pinned FetchContent (2026-11-08)

**Decision.** New dependencies come through CMake FetchContent, pinned by URL
and SHA256 (cmake/Dependencies.cmake). Archives are downloaded once into
`.deps/` (gitignored) and every build tree extracts the cached file, so only
the first configure needs the network. EnTT stays vendored at 3.16.0, the
version the design was verified against (4.0 is a breaking release). The
legacy tree keeps SFML's own unpinned fetches of its codecs.

## ADR-003 The legacy tree is a deterministic reference (2026-11-08)

**Decision.** core/ and app/ are stabilised in place (one paced loop, tick
time, effects by value, seeded RNG, recorded input, a per-tick projection
hash), then changed only by declared rules (docs/rules/gameplay-rules.md),
each re-baselining the golden projections. Built only with
`SE_BUILD_LEGACY=ON`. Legacy and se_* targets never link each other
(se_check_link_graph). The reference retires when docs/ledger.md closes.

## ADR-004 Module layering, with headless halves split out (2026-11-08)

**Decision.** Modules follow design/costfree-end-state.md ("Module layering
and build"), with one deviation: the headless parts of the runtime and the
renderer are separate modules (se_sim, se_render_core), so se_tests, se_bench
and the WSL sanitizer builds never compile SDL. Forbidden edges are declared
per module (`FORBID`) and checked at configure time; banned includes are
scanned by tools/checks/check_includes.py.

## ADR-005 Google C++ style for new code (2026-11-08)

**Decision.** All new C++ follows the Google C++ Style Guide, enforced by
clang-format, cpplint and clang-tidy (docs/style.md). The legacy tree keeps
its style so that rule commits stay reviewable diffs against the reference.
Design identifiers are translated: `parallelFor` → `ParallelFor`,
`tickInline` → `TickInline`, `deriveGuid` → `DeriveGuid`, `m_desc` → `desc_`,
`Cadence::Locked` → `Cadence::kLocked`, `g_simOwner` → `g_sim_owner`.

## ADR-006 Exceptions: never thrown, caught only at fault boundaries (2026-11-08)

**Decision.** Deviation from the Google guide, chosen by the owner. Engine
code never throws; errors are returned as `std::expected<T, se::Error>`.
`try`/`catch (...)` exists only in files marked `// se-fault-boundary`: the
SDL callbacks, Play tick and render isolation (Play.Faulted), ForkJoinPool
chunks and the FrameRecorder transaction. They contain throws from the
standard library, EnTT or glaze (for example `bad_alloc`), which the design's
fault containment (changes 8, 10, 11) depends on. Enforced by
tools/checks/check_banned_api.py.

## ADR-007 World ownership is its own capability (2026-11-08)

**Decision.** Two thread-safety capabilities: `g_main` (main-thread only:
window, GPU, ImGui) and `g_sim_owner` (World ownership). Main holds
`g_sim_owner` in inline mode; the sim thread holds it under the threaded
driver. World structural calls require `g_sim_owner`, not `g_main`, and
runtime checks compare the World's owner thread id. Deciding this in the
kernel avoids an annotation sweep when the threaded driver lands.

## ADR-008 k_rt in Locked cadence is the locked ratio (2026-11-08)

**Decision.** The design writes `k_rt = ceil(T/dt)`, which gives 3 at
59.94 Hz (T/dt = 2.002) and contradicts its own "exactly 2 ticks per vblank"
and "no 3-tick frames at 59.94 Hz Locked". In Locked cadence k_rt is the
locked integer ratio k; `ceil` applies in Free cadence only. A FixedStepper
test pins k_rt = 2 at 59.94 Hz.

## ADR-009 se_core never calls SDL to identify the main thread (2026-11-08)

**Decision.** se_core may not include SDL, so `SE_ASSERT_MAIN_THREAD` compares
against a thread id registered by `se::RegisterMainThread()`. se_platform
calls it from `SDL_AppInit` and cross-checks it with `SDL_IsMainThread()`.

## ADR-010 Toolchain floor: MSVC 14.50+, CMake 3.31+ (2026-11-08)

**Decision.** glaze v9 needs MSVC 14.50 or newer and CMake 3.31, so the
engine builds with Visual Studio 18 (MSVC 14.51, via scripts/with-msvc.cmd)
or clang-cl 21; Visual Studio 2022 (14.44) builds only the legacy tree.
Measured cost: a 20-aggregate glaze reflection translation unit compiles in
about 6 s (MSVC Dev), so glaze stays confined to the component-registry
translation units.

## ADR-011 Order-independent world hash (2026-11-08)

**Decision.** The per-tick world hash folds per-entity digests (Guid plus
hashed component bytes) by sum and xor. It is independent of entity storage
order and of fork chunking, needs no sort, and `--det-crosscheck` dumps
per-entity digests sorted by Guid to locate the first divergence. This is
stronger than the design's "partials combined in a fixed order".

## ADR-012 Local CI only (2026-11-08)

**Decision.** Nothing is pushed from this branch, so scripts/ci.ps1 is the CI:
the commit gate runs msvc-dev, msvc-asan and clangcl-dev with every test label
(unit, determinism, golden, checks, lint); `-Milestone` adds Debug, Release and
the WSL TSan and ASan presets. Nothing is committed with a failing gate.

## ADR-013 Embedded debug info on MSVC (2026-11-08)

**Decision.** `/Z7` (CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded) in every
configuration, so parallel Ninja builds never contend for a shared PDB.

## ADR-014 Stable component ids in every serialized form (2026-11-08)

**Decision.** Session logs, replays and world hashes identify a component by
the 32-bit FNV-1a hash of its registered name (`ComponentInfo::name_id`),
never by `entt::type_hash`, which is computed from the compiler's pretty
function name and differs between MSVC and clang. EnTT storages stay keyed
by `entt::type_hash` (`ComponentInfo::id`), so ForkSite read and write sets
use that id. With this, MSVC and clang-cl produce bit-identical per-tick
hashes (tests/golden/windows-msvc and windows-clangcl match).

## ADR-015 Replays are BEVE written by a first-party subset writer (2026-11-08)

**Decision.** Replay files are BEVE as specified, written and read by
se_sim/beve.h, a writer and reader for the subset replays use (objects with
string keys, strings, unsigned integers, u8 and u64 typed arrays). glaze
stays confined to reflect_glaze.h and `*_components.cc` (ADR-010);
tests/kernel/replay_test.cc proves glaze's `beve_to_json` reads the files.

## ADR-016 FrameClock times come from the vblank grid (2026-11-08)

**Decision.** The pacer reports a frame's start on the vblank grid (one
presentation interval before its target vblank, from present statistics)
and counts elapsed intervals between targets; the wall-clock wake time goes
only into `FrameClock::real_delta`. With planned late latching, B changes
from frame to frame, so wake-time deltas are not near multiples of T and
Free cadence could not snap them; the stepper scenarios in
tests/support/virtual_display model exactly this contract for M6.

## ADR-017 Accessors are PascalCase (2026-11-08)

**Decision.** clang-tidy's identifier rule (MethodCase CamelCase, only
`begin`, `end`, `size`, `empty`, `data` and `swap` exempt) rejects Google's
optional snake_case accessors, so the milestone spec's `owner()`,
`set_owner()`, `stats()`, `applied()`, `worlds` and `sim_running` are
written `Owner()`, `SetOwner()`, `Stats()`, `Applied()`, `Worlds()` and
`SimRunning()`, and the spec's public `World::clock` member is
`Clock()`/`MutableClock()` (Google style keeps class data private).

## ADR-018 SessionLog entries are logged where a replay applies them (2026-11-08)

**Decision.** `SessionLog::Enqueue(entry, next_tick, states)` takes the tick
the session runs next and stamps `apply_before` itself. While running, an
entry is queued against `max(apply_before, next_tick)`, so a stale tick
cannot make a replay apply it earlier than the live session did. While
paused, the session waits at the boundary before `next_tick`: Enqueue first
completes that boundary's pending work in RunTick's order
(`GameStateStack::ApplyPending`, then the entries due by `next_tick`), then
applies the entry and logs it against `next_tick`.

**Why.** The spec says paused entries apply at once and are logged against
the next tick, but the caller's `apply_before` was kept, so a replay applied
the entry at another boundary. Applying it at once while earlier work for
the same boundary was still pending also put it ahead of that work, while a
replay applies the boundary as ApplyPending, then (apply_before, seq). The
boundary is completed first so the live order and the replay order are the
same by construction. The visible effect: a GameState change the last tick
requested, or an entry already queued for the next tick, takes effect when
the first edit is made in a pause instead of at resume, before any tick runs
either way. tests/kernel/session_log_test.cc replays a paused session's
written log and compares every hash.

## ADR-019 Nested forks go through NestedParallelFor under g_fork_chunk (2026-11-08)

**Decision.** A third capability, `jobs::g_fork_chunk`, is held while a
thread runs fork chunks (`ForkScope`, taken by `ParallelFor`). Nested forks
call `jobs::NestedParallelFor(const ForkSite&, count, body)`, which requires
`g_fork_chunk`, runs inline with ParallelFor's chunking, leaves the site's
stats alone and restores the breadcrumb's outer chunk. A chunk lambda that
forks again declares `SE_REQUIRES(jobs::g_fork_chunk)`. `ParallelFor`
asserts it is not called from a chunk.

**Why.** The spec's "nested forks run inline" could only be written with
`SE_NO_THREAD_SAFETY_ANALYSIS` on the chunk lambda, because `ParallelFor`
requires `g_sim_owner`, which a chunk must not hold (that is what keeps
World calls out of chunks). The fork capability gives nested forks an
analysable path without granting chunks World access, and the const site
keeps M14's workers from racing on owner-thread stats. Checked by
tests/compile_fail/nested_fork.cc and death.owner_fork_inside_chunk.

## ADR-020 InlineOwnerScope takes a MainThreadProof (2026-11-08)

**Decision.** `InlineOwnerScope` is built as
`InlineOwnerScope owner(OnMainThread())`. `OnMainThread()` requires `g_main`
and asserts the main thread; the scope acquires `g_sim_owner`, releases only
`g_sim_owner`, and asserts the main thread again at run time.

**Why.** The spec's scope ("main takes g_sim_owner in inline mode") was
claimable from any thread. `SE_REQUIRES(g_main)` cannot go on the scope's
constructor: clang reads a requirement on a scoped capability's constructor
as adopting the capability, so the scope then released `g_main` at its end,
or the analysis rejected the paths joining after it (clang 21, checked with
clang-cl). A proof value made by a function that requires `g_main` gives
the same static check without that reading.

## ADR-021 ParallelView detects structural changes per storage (2026-11-08)

**Decision.** `ComponentInfo` gains `watch_structure`, which connects a
counter to the component's EnTT construction and destruction signals.
Worlds connect one counter per registered storage in assert builds
(`World::StructureGeneration`), and a ParallelView compares the counters of
its captured storages, as well as their sizes, at every use.

**Why.** Comparing sizes alone missed a remove plus an emplace (or a
destroy plus a create) between capture and fork, which moves entities in the
packed arrays the chunks index. The signals see every structural path,
typed or type-erased, and only the captured storages count, so unrelated
structural changes (a DestroyRequest, a new Name) do not invalidate a view.
Release builds connect nothing.

## ADR-022 RenderList conventions: degrees, canvas pixels, texel UVs (2026-11-08)

**Decision.** se_render_core (M3c) fixes the conventions the M3 spec leaves
open, so game modules, the GPU backend (M7) and the editor (M10) agree:
- `Transform::rot` and `PrevTransform::rot` are degrees, clockwise on screen
  (y grows downwards), as in the legacy tree's sf::Sprite and in the
  rotations game_rampage projects (R12's heading). The renderer converts
  them to radians for `se::dm::SinCos`. *Superseded at the M3 integration
  by ADR-023: the unit is radians, still clockwise on screen.*
- `SpriteQuad::pos` is in canvas pixels of the camera (0..virtual_size,
  y down): `(world - center) * zoom + virtual_size / 2`. The backend maps the
  canvas to the window (letterboxing, `Camera2D::integer_scale`).
- `SpriteQuad::uv` is texel coordinates inside the texture
  (`region.x .. region.x + region.w`); only the backend knows texture sizes.
- Corners are ordered top-left, top-right, bottom-right, bottom-left of the
  unrotated sprite. `SpriteRenderer::origin` is the pivot of position,
  rotation and scale (sf::Sprite's origin), and `flip_x`/`flip_y` mirror the
  sprite about it, keeping the corner order (the texture coordinates swap).
- Culling keeps a sprite only when its rotated bounds overlap the canvas with
  a positive area; touching an edge is culled.
- Interpolation uses PrevTransform only when it is from the tick before the
  World's current tick and `teleport_tick` is not the current tick; a half
  turn between ticks goes the negative way.
- `RenderList` carries a `scratch` member (sort items and unsorted quads)
  beside the spec's `quads`, `batches` and `stats`, so `BuildRenderList` keeps
  the spec's signature and still allocates nothing once warm.

**Why.** The spec gives the types but not the units. Degrees match the
reference the M3 comparison runs against and the `.seproj` rotations;
canvas pixels and texel UVs keep the headless list free of GPU and asset
knowledge until M7 and M9. (The rotation unit was settled the other way by
ADR-023 when M3b and M3c were merged.)

## ADR-023 Transform::rot is in radians, the way sf::Angle stores it (2026-11-08)

**Decision.** `Transform::rot` and `PrevTransform::rot` hold radians.
game_rampage keeps them in [0, 2 pi): a heading in degrees becomes
`degrees * (pi / 180)` wrapped by sf::Angle's `positiveRemainder`, with
SFML's float constants and operation order (`RotationFromHeading`), and the
projection prints `rot * (180 / pi)` normalised to [0, 360). Editors and
scene authors may show degrees; components store radians.

**Why.** se::dm's SinCos and Atan2 work in radians (the kernel test module
already stores Atan2's result), the RenderList (M3c) rotates corners with
`dm::SinCos(rot)`, and copying sf::Angle's operations made the engine's
legacy-ref-v1 projections bit-identical to the reference's (refcmp reports
zero deviation on all three scenarios), rotation included.

**Integration (M3).** M3b and M3c were written in parallel and chose
different units: ADR-022's first bullet (M3c) said degrees. Radians win,
and that bullet is superseded: the kernel test module, se::dm and
game_rampage already store radians, the determinism goldens hash them, and
the reference comparison is exact with them, while the RenderList needed
only its conversion factor removed. `BuildRenderList` now passes
`Transform::rot` to `dm::SinCos` unchanged, and the shorter arc of the
interpolation is taken in [-pi, pi) (a half turn still goes the negative
way). Positive angles still turn clockwise on screen (y down), as
sf::Sprite's do, so RotationFromHeading's values draw the legacy headings.

## ADR-024 Scene files name stats; components store their ids (2026-11-08)

**Decision.** In se-scene-v1 scenes and prefabs, `Stats` is written as an
object of named values (`"Stats": {"HP": 100, "Speed": 500}`) and an
effect's stat as its name (`"stat": "HP"`). The loader turns them into the
registry form (`{"values": [{"id": <Fnv1a32 of the name>, ...}]}`, sorted
by id) before the component's JSON hook reads it, and records the names in
the World's StatNames; SaveScene writes the named form back. The registry
form is accepted as well. An entity with a Path whose Transform sets no
position starts at the path's first point (R12).

**Why.** Authors cannot write FNV hashes, and with stats as an object a
merge patch changes one stat (`"overrides": {"Stats": {"Speed": 250}}`)
where an array would be replaced whole. glaze stays confined: the
translation is JSON to JSON, in game_rampage's scene loader.

**Review (M3).** The registry form went to glaze unchecked, so a scene
could load `Stats.values` out of id order, and FindStat's binary search
then missed stats that were there (an entity with HP read it as absent and
died in tick 1, R10). The JSON and BEVE hooks of `Stats`, `PairCooldowns`
and `ActiveEffects` (rampage_components.cc) now sort what they read, by
stat id, receiver Guid and seq, and reject a repeated stat id or receiver,
so scenes, snapshots and session-log edits all keep the order the systems
rely on. Stat names, in scenes and legacy scenarios alike, must be
non-empty and have distinct ids (`InternStatName`): two names with one
Fnv1a32 id would be one stat, and an empty name could not be printed in
the projection.

## ADR-025 The legacy comparison keeps both reference rule sets (2026-11-08)

**Decision.** game_rampage implements R11 to R13 by default. The World
setting `RulesConfig::version = kV1` (`se_replay --legacy-rules v1`)
restores the legacy-ref-v1 boundary clamp and path stepping, nothing else.
`ref.sandbox_v1.NAME` compares that build against the legacy-ref-v1
goldens, kept with their scenarios and inputs in
tests/fixtures/reference/v1/; `ref.sandbox.NAME` compares the default build
against tests/fixtures/reference/ (legacy-ref-v2 once M3a re-baselines it).
CMake registers `ref.sandbox.NAME` disabled while the two directories'
goldens are identical, that is until M3a's goldens arrive. (At the M3
integration M3a's goldens arrived and that condition was removed: both
comparisons always run, so a golden mix-up fails instead of disabling a
test.)

**Why.** The v1 comparison proves the port of R1 to R10 and everything the
projection shows (stat arithmetic, effect timing, the watchdog, spawn
order, the player's movement, rotation normalisation, the line format)
independently of the three new rules, and it stays a regression test after
M3a moves the main goldens on.

## ADR-026 game_rampage draws with SpriteRenderer; scenes name textures (2026-11-08)

**Decision.** game_rampage depends on se_render_core. Its GameModule
registers RegisterRampageComponents, then RegisterRenderComponents
(SpriteRenderer, Camera2D), so the render components come after every
hashed gameplay component and are themselves not hashed. Scenes and
prefabs write a sprite's texture by name, a key of config/texture.json
(`"SpriteRenderer": {"texture": "player", ...}`); the loader stores its
`TextureKeyOf` key (Fnv1a32 of the name, se_render_core/render_components.h)
and records the name in the World's `TextureNames` (a `NameTable` like
`StatNames`), and SaveScene writes the name back. The player and enemy
prefabs draw the legacy Sprite: the whole 48 x 48 `player` texture with its
origin at the centre; LoadLegacyScenario gives every entity the same from
its scenario texture and the PNG IHDR size. The sandbox scene places no
camera: `SandboxCamera(world)` returns the World's primary Camera2D if a
scene places one, otherwise `ArenaCamera`, the whole arena at zoom 1.

**Why.** The legacy Sandbox drew every entity with the `player` texture
into a scene render texture of the configured resolution, which is what
ArenaCamera describes. A camera entity in the sandbox scene would change
the world hash and add a line to every projection tick, breaking the
scene's exact match with the legacy `stress` scenario; the camera only
matters once something renders (M7). Authors cannot write FNV hashes, so
textures are named as stats are (ADR-024). With nothing hashed added, the
determinism_rampage goldens and the reference projections are unchanged.

## ADR-027 M3 behaviours the spec did not fix, accepted at integration (2026-11-08)

**Decision.** The M3 integration keeps these behaviours of the merged parts,
none of which the reference projections can tell apart:
- Cleanup drains destroy requests in Guid order (`World::DrainDestroyRequests`,
  M2), not in spawn-serial order as the M3b table says. game_rampage never
  creates entities after a load and nothing reads the order, and the world
  hash does not depend on it. Revisit when gameplay spawns at run time.
- Opposite directions held together (Up+Down, Left+Right) move the player
  by their net intent, 0, where the legacy Movement applied two sequential
  steps, `(y - s) + s`, which can differ in the last bit at non-integer
  positions. No reference input holds opposite keys.
- Degenerate paths (corrected after the M3 review). AdvancePath follows
  R12's loop with the legacy WayPointSystem's own guard and nothing else: at
  each wrap of a repeating path, a remaining step that is not smaller than
  at the previous wrap (a round lost in float rounding, or a NaN or infinite
  speed) becomes 0, so the step ends at the start of the path. A step can
  go round many times in one tick, as in the reference: a 1 px loop at
  2e7 px/s passes 166666 segments, a loop of 2^-14 px segments at the
  stress speed of 500 px/s passes 68266. The integration first stopped
  after 65536 iterations and extrapolated past the segment's end, which
  left the polyline at those inputs; the claim that its guard fired only
  near 1e9 px/s was wrong. One difference remains: a segment of NaN length,
  which only infinite coordinates (beyond 3.4e38) produce, is skipped like
  one of length 0, where the legacy code moves to NaN or, on a loop, goes
  round for ever. A path whose segment is out of range (after an edit)
  starts over, as in the legacy code. RotationFromHeading returns 0 for
  non-finite or enormous headings (an undefined int cast in the legacy
  code).
- LoadLegacyScenario rejects what the World cannot represent, which the
  legacy loader accepts: an empty stat name (the projection cannot print
  it, ADR-024) and two stat names with the same Fnv1a32 id (they would be
  one stat). The legacy Sandbox prints the empty name as `=<value>`.
- `se_replay --module rampage` rejects `--edits` and `--det-crosscheck`:
  World::Clone does not copy the registry ctx that holds the scene settings
  (a kernel question for M10 and M14).
- The PathCache is rebuilt when a path's point or segment count changes; an
  edit that moves a point without changing the count (M10) must rebuild it
  explicitly.

**Why.** Each is either unobservable in M3's content or a guard for input
the legacy code handled with undefined or endless behaviour; fixing them
now would change kernel contracts (M2's drain) or the legacy reference
without a rule to state the new behaviour.

## ADR-028 Moved is serialized (2026-11-08)

**Decision.** game_rampage registers `Moved` as hashed and serialized,
where the M3b spec says "not serialized". Between ticks only a load leaves
it: LoadLegacyScenario sets it on a scenario's `"moving"` entities (the
legacy UpdateEntityEvent at spawn), and it decides which event-driven
sources act in tick 1 (R2). SaveScene therefore writes `"Moved": {}` for
such an entity and LoadScene reads it back, so saving a World straight
after a load and reloading it gives the same hash and the same ticks
(tests/rampage/scene_test.cc, on the `effects` scenario). A scene may also
mark an entity as moved for its first tick. Cleanup removes every Moved at
the end of a tick, so scenes saved later are unchanged.

**Why.** A hashed component that is not serialized changes the world hash
across a save and reload, and here it also dropped the tick-1 contacts of
event-driven sources. Leaving Moved out of the hash would only hide that
difference, and a scene-level `moving` flag would say through a new key
what the component registry already expresses.

## ADR-029 se_os is the headless OS layer; Tracy is linked by se_core (2026-11-08)

**Decision.** A new STATIC module, se_os (engine/se_os; ALLOW_ONLY se_core;
not DETERMINISTIC), holds everything threads need from the OS on every
platform: StartThread, Thread and RoleScope (OS thread names, QoS, CPU
sets), the CPU topology and its pool-size and CPU-set rules, the unbiased
clock, debugger detection and ExecutableDir. It may include `<windows.h>`
and POSIX headers but never SDL, so the SDL-free WSL sanitizer builds have
it, and it is the only module that may create a thread. Its rules:
- The PerformanceOnly count is the P-cores on main's last-level cache minus
  one, capped at ForkPoolSize: the spec's formula alone gives 3 on the
  4-core laptop, where the design's table (and the pool) has 2.
- Main's last-level cache is the one with the most P-cores (the lowest id
  on a tie); "P-cores" are the cores of the highest efficiency class, so
  every core on a homogeneous CPU.
- Linux kEco raises the thread's nice value by 5, which lowers its
  priority, as EcoQoS does on Windows; the spec's "lowers the nice value"
  would have raised the priority of background work. Restoring the old
  value needs CAP_SYS_NICE, so the destructor tries and stays silent.
- CPU sets name the OS's ids (CPU set ids on Windows, CPU numbers on Linux)
  as CpuTopology reports them, so a request needs no lookup at apply time.
- QueryUnbiasedInterruptTimePrecise is looked up in KernelBase at run time:
  kernel32.lib does not export it and linking mincore.lib would break the
  ALLOW_ONLY list.
- Tracy is a private dependency of se_core (TracyClient, SE_TRACY builds
  only), so the profiling macros sit beside the code they measure; only
  se_core/src/profile.cc includes Tracy. TracyClient is built with
  TRACY_DELAYED_INIT and TRACY_MANUAL_LIFETIME (nothing starts until
  `profile::Start()`, and every macro checks the started flag), keeps
  TRACY_ON_DEMAND, TRACY_ONLY_LOCALHOST and TRACY_NO_BROADCAST, sets
  TRACY_DBGHELP_LOCK=SeDbgHelp (se_core defines the lock) and
  TRACY_NO_CRASH_HANDLER (crashpad owns crashes). Zones with run-time names
  (schedule systems, fork sites) use `SE_PROFILE_ZONE_DYNAMIC`, an addition
  to the spec's macro list. Thread names given before the start are applied
  by each thread's first profiled event.

**Why.** Threads must have the same names, roles and placement in the
SDL-free test builds as in the runtime, and se_core may not include
platform headers. Linking Tracy from se_core keeps the zone macros free of
a dependency on a higher module; the manual lifetime is the only Tracy mode
that guarantees no socket and no thread in tests, tools and benches.

## ADR-030 Two more fault boundaries: thread creation and the log sinks (2026-11-08)

**Decision.** ADR-006's list of `// se-fault-boundary` files gains:
- se_os/src/thread.cc: `std::jthread` reports a thread the OS cannot
  create with std::system_error, which `StartThread` returns as an Error.
  An exception that escapes a thread's body is not caught: the entry is
  noexcept, so std::terminate runs at the throw and the crash handler
  records it there.
- se_log/src/log_worker.cc: spdlog reports file errors (opening, writing,
  rotating, flushing) with spdlog_ex. The sinks are created and called only
  there; `LogWorker::Start` turns a failure into an Error with the path, and
  a failure while running is counted (LogWorkerStats::sink_errors),
  reported once on stderr, and survived.

**Why.** Both are third-party throws at the edge of engine code: the
standard library's thread constructor and spdlog's sinks. Returning
std::expected from StartThread is the spec's contract, and a full disk must
not take the log worker down.

## ADR-031 Breadcrumbs and rings live in dump-stream memory (2026-11-08)

**Decision.** The ThreadRegistry's table, every log ring and the InputFrame
ring are the plain-data layouts of docs/milestone-specs.md "Dump stream
layout" (se_core/dump_stream.h, pinned by static_assert), and the live
counters are those fields: `head`, `dropped`, a slot's `packed` breadcrumb
and `state`. Fields other threads read while they change are accessed
through `std::atomic_ref` (`AtomicLoad`, `AtomicStore`, and byte-wise
relaxed stores for names), never through a second copy. A thread's
`Breadcrumb` (se_core/breadcrumb.h) keeps its API but becomes a handle: it
writes its own word until a `ThreadSlotScope` binds it to the slot's
`packed` word, and it carries the value over on bind and unbind. The
registry hands every region to an optional `DumpStreamHook` (the
breadcrumb table, the library ring, and each slot's ring when it is
allocated), which the integration points at se_crash's RegisterStream.
Rings and the table are allocated once and never freed; the InputFrame ring
a process registers must likewise never be destroyed. Registry names keep 16
characters.

**Why.** A crash dump must show the real state, so the memory the threads
write is the memory the handler reads; a copy refreshed by the producer
would lag exactly when it matters. `std::atomic_ref` over plain structs
keeps the layout checkable with `offsetof` and the accesses race-free under
TSan. Binding the existing Breadcrumb rather than replacing it leaves every
M2 call site (the Schedule, ForkScope, the tests) unchanged.

## ADR-032 The log backend: rings, a consumer, and ordered output (2026-11-08)

**Decision.** The SE_LOG_* front end (se_core/log.h; call sites unchanged)
writes a LogRecord into the calling thread's LogRing while a `LogConsumer`
is attached (`AttachLogConsumer`; se_log's LogWorker is the only one), and
writes synchronously in the M2 format, `[I t=12 main] message (file:line)`,
otherwise. Details the spec leaves open:
- Ordering. A producer marks its ring before it stamps a record
  (`LogRing::BeginWrite`) and clears the mark after the push. Each wake the
  worker writes only the records older than every mark in progress and
  holds the rest for the next wake, so lines from different threads come
  out in (timestamp, sequence) order across wakes, not only within one.
  Stop and the fatal flush write everything.
- Detach. `DetachLogConsumer` clears the consumer and then waits (at most
  1 s) for marks in progress; a producer checks the consumer after setting
  its mark, so every record is either in a ring the worker drains a last
  time or written synchronously, never lost. *(Superseded by ADR-045: the
  wait has no deadline.)*
- The main thread without a slot (it normally has slot 0) uses the library
  ring only through try_lock, and counts a drop when the lock is busy.
- SE_LOG_FATAL pushes the record and asks the consumer to flush while the
  mark is held (so the worker cannot be detached meanwhile); the worker
  writes everything and flushes its sinks; the caller waits up to 2 s. If
  the record was dropped, the flush timed out, or the fatal call is on the
  worker itself, the synchronous line goes to stderr before the abort.
- `LogConfig` adds `wake_on_push` (default on) so tests can let a ring fill
  up, and `rate_limit_per_second = 0` turns the rate limit off. The time
  column counts from `LogEpochNs()`, the first time it was read (LogWorker
  Start reads it).
- `kLogMaxMessage` is 448, the record's message field, as the dump layout
  requires; longer messages are still truncated, not dropped.

**Why.** The design requires that nothing blocks a producer and that the
rings double as the crash tail; the watermark keeps the file's order
meaningful without making producers wait for the worker, and the mark is
the same mechanism Stop needs to drain without losing records.

## ADR-033 se_crash drives crashpad's client API directly; nothing is uploaded (2026-11-08)

**Decision.** se_crash (M4b) uses crashpad's client API from the pinned
sentry-native archive (CrashpadClient, CrashpadInfo, CrashReportDatabase,
crashpad annotations), not sentry_init: the sentry library is never linked
(se_crash FORBIDs it; only `crashpad::client` is), and the build copies
`crashpad_handler` next to each executable that starts crash capture
(`se_crash_copy_handler`). The contract the M4 integration and later
milestones build on:
- `Start(CrashConfig)` launches the handler synchronously with no URL, no
  metrics directory and no extra arguments, after turning uploads off in
  the database settings. Failures are Errors: kInvalidArgument (empty path,
  already started), kNotFound (no handler executable), kIo (database or
  launch), kUnsupported (stub builds). crashpad's process-wide state allows
  one launch per process; if the launch fails, the exception filter and the
  SIGABRT handler that StartHandler installed first are put back.
- Fatal faults reach the handler from Start to process exit: crashpad
  cannot uninstall its exception filter. Destroying the CrashGuard ends
  DumpWithoutCrash (kUnsupported afterwards) and restores the previous
  terminate handler.
- The terminate handler records why std::terminate ran in the annotation
  object `se.terminate` ("std::terminate with exception: <what>" or
  "std::terminate without an active exception"; for a throw through
  noexcept MSVC keeps the exception active, clang-cl does not), then calls
  the previous handler or std::abort, which crashpad's SIGABRT handler
  turns into a dump (exception code STATUS_FATAL_APP_EXIT). It rethrows
  the active exception to read its message, so
  engine/se_crash/src/terminate_handler.cc is a `// se-fault-boundary`
  file, one more boundary than ADR-006 lists.
- `DumpWithoutCrash(reason)` sets the annotation object `se.dump_reason`,
  asks the handler for a dump and returns when crashpad's dump-completed
  event fires (crashpad waits without a timeout). Requests are serialised
  by a mutex held across the dump, which the watchdog and main's fault
  path can both take: an exception to "no lock shared between main and a
  helper", accepted because crashpad itself admits one dump at a time and
  the handler suspends every thread of the process while it writes one.
- `RegisterStream(type, data, size)` wraps
  CrashpadInfo::AddUserDataMinidumpStream under a mutex (crashpad does not
  lock its list) and rejects types outside 0x53450001-0x5345FFFF, empty
  regions and duplicates (the minidump writer would drop a second stream
  of a type). Types: 0x53450001 breadcrumbs, 0x53450002 InputFrame ring,
  0x53450003 SessionLog tail, 0x53450004 flight recorder, and
  0x53450100 + slot for the log ring of ThreadRegistry slot `slot`, with
  slot 64 for the shared library ring. tools/minidump/read_dump.py knows
  these values and decodes each stream by its own header.
- `CrashConfig::annotations` become crashpad's process annotations.

**Why.** Registered streams need AddUserDataMinidumpStream, which
sentry_init does not expose, and the engine wants no upload path at all
rather than a configured-off one. The mutex in DumpWithoutCrash keeps the
reason annotation of concurrent requests apart at no cost the dump itself
does not already impose.

## ADR-034 Crash capture runs on Windows only for now, and not under ASan (2026-11-08)

**Decision.** se_crash has two backends with one API: crashpad (Windows,
`SE_CRASH=ON`) and a stub (Start and DumpWithoutCrash return kUnsupported,
RegisterStream validates and keeps nothing). The stub is built on Linux
and in msvc-asan:
- Linux: crashpad's Linux build calls `find_package(CURL REQUIRED)` for its
  HTTP transport (external/crashpad/util/CMakeLists.txt) even with
  SENTRY_TRANSPORT=none, and the WSL machine has libcurl's runtime but not
  its development files. Installing packages is the owner's step, so the
  Linux presets keep `SE_CRASH=OFF`, the crash tests are not registered
  there (crash.stub runs instead) and docs/gates.md, "Linux crash capture",
  names the package and the remaining work.
- msvc-asan keeps `SE_CRASH=OFF`, as since M0. Measured with crashpad
  built under /fsanitize=address: ASan's own exception handler takes
  access violations and stack overflows before crashpad's filter, prints
  its report and exits 1 without a dump, so those two crash tests cannot
  pass there; abort, std::terminate and DumpWithoutCrash dumps work but
  take 7-10 s each (docs/gates.md, "Crash capture under ASan"). The crash
  tests run in msvc-dev and clangcl-dev; msvc-asan runs crash.stub, and
  M5's init-failure tests under ASan see crash capture fail as it may in
  the field.

**Why.** The stub keeps every caller free of build switches, and a crash
test that cannot run is better absent than skipped silently: the gates.md
entries say exactly what would enable each.

## ADR-036 The App runs headless for its callback tests (2026-11-08)

**Decision.** AppConfig gains `--headless`: SDL starts with its dummy video
driver, and boot steps 6 (window) and 7 (GPU device) do nothing; the frame
plans and runs ticks but records nothing, and `--frames` counts iterates.
The callback tests (AppQuit(NULL), throws injected in AppIterate and
AppEvent, a failure at every boot step, the two-phase quit with providers,
LOW_MEMORY and TERMINATING from an engine thread through the inbox) run the
real App this way as unit tests, on Windows and under TSan and ASan in WSL.
The windowed paths (Modal, NULL acquires, Suspended, session end, power,
the boot-step sweep of the rampage executable) are desktop tests. AppConfig
also gains `--no-activate` (the window is shown without focus, for tests),
`--content-root`, `--pref-dir`, `--return-on-quit` (Release builds end the
process after SDL_AppQuit otherwise) and `--log-level`; arguments after
`--` go to the game. Test hooks (`--fail-init-step`, `--inject-throw
iterate|event[:N]`, `--inject-null-acquire FROM:COUNT`) exist in every build
but Release (SE_RUNTIME_TEST_HOOKS). The injected throw is a standard
library one (`std::vector::at` past the end), so engine code still never
writes `throw`.

**Why.** The spec lists the callback tests apart from the desktop ones, and
TSan exists only in WSL, which has no window system or Vulkan driver; a
headless App lets the callbacks' fault paths and the quit drain run under
the sanitizers instead of only on this desktop.

## ADR-037 The M4 parts plug into se_runtime through one hooks file (2026-11-08)

**Decision.** M5 builds se_runtime without se_os, se_log and se_crash (the
M4 tracks build them in parallel). Every place they plug in is a function in
`engine/se_runtime/include/se_runtime/integration.h`, marked
`INTEGRATION(M4a)` or `INTEGRATION(M4b)`: EnterMainThread/LeaveMainThread
(the main RoleScope, HighQoS), StartCrashCapture (boot step 1),
RequestFaultDump (the callbacks' fault path), StartLogging (boot step 2),
ReadCpuTopology (step 8), StartHelperThreads with the WatchdogInputs (step
9: the iterate counter, Modal, Suspended and heartbeat atomics),
HelperThreads::ArmWatchdog (after the first iterate), StartShutdownDeadline
(when the quit drain ends), HelperThreads::Stop (shutdown steps 16 and 17),
EngineThreadsRunning and ShutdownLoggingOnly. In M5 they keep M2's
behaviour (RegisterMainThread, synchronous logging) and start no thread.
se_runtime's link list gains se_os, se_log and se_crash at integration.
The integration filled them in (ADR-042, ADR-043).

**Why.** Three tracks edit in parallel; one file of documented no-ops makes
the integration a fill-in rather than a search through the App.

## ADR-038 Fault paths do not allocate; the window procedure is not a catch boundary (2026-11-08)

**Decision.** Inside the noexcept callbacks' catch handlers, the exception
text is copied into a fixed buffer and the message box text is assembled
in one (ShowFaultMessage), so a fault caused by exhausted memory can still
be reported. The Inbox is two fixed buffers of 64 events swapped at each
take (the design's "mutex-guarded vector" without allocation in the filter,
which runs on SDL's pushing thread). The window subclass procedure calls a
WindowListener whose methods are noexcept and is not a fault boundary: an
exception there ends the process through std::terminate (the crash handler
records it from M4 on) instead of unwinding through the Windows modal
loop. ADR-006's list of boundaries is unchanged.

**Why.** The design forbids unwinding through SDL's C frames and the modal
loop; std::terminate inside a noexcept frame is defined behaviour, a catch
in the window procedure would have to invent a recovery the design does not
have (the quit path runs from the callbacks).

## ADR-039 M5's frame clock falls back to measured frames; its costs leave out the swapchain wait (2026-11-08)

**Decision.** Until M6's pacer and M7's renderer measure them, the App
feeds CostTracker each tick's cost and, as the non-tick cost R, the
iterate's time minus the ticks and minus the whole frame transaction
(command buffer, SDL_WaitForGPUSwapchain, acquire, submit); the GPU cost is
unknown (NaN). The present mode is VSYNC, so the transaction includes the
wait for a swapchain image.

**Why.** Counted as CPU time, the VSYNC wait makes R about one period, k_95
falls below k_rt and the stepper enters Overloaded after a second, running
one tick per frame: at 60 Hz the game would run at half speed. *(Since
M7 R leaves out only the swapchain wait: the render build, recording and
submit are CPU work and count; ADR-064.)*

**Decision (frame clock).** The FrameClock's period is SDL's display mode
unless a moving average of the frame deltas (weight 0.1, stalls over
0.25 s left out) differs from it by more than 5%; then the clock reports
the measured period, and the FixedStepper runs Free cadence on real time.

**Why.** Running the rampage executable on this machine, SDL reported a
60 Hz display for a window whose VSYNC presents came at 165 Hz. In Locked
cadence with unknown vblank counts the stepper rounds each 6 ms delta to
zero vblanks: 300 presented frames ran 2 ticks. The same happens in a
modal loop, whose WM_TIMER iterates do not follow the display. M6's
pacer uses present statistics and drops to Standard on such a
disagreement; until then the measured rate keeps 120 ticks per second.

## ADR-040 Lifecycle details the design leaves open (2026-11-08)

**Decision.**
- Suspended has two sources, the window (MINIMIZED, HIDDEN, OCCLUDED) and a
  power suspend; each is cleared only by its own resume (RESTORED, SHOWN,
  EXPOSED or MAXIMIZED, or the 10 Hz probe, for the window; power resume
  for power). The probe renders only for a window suspension.
- FocusLost iterates at the full rate and gameplay continues by default
  (`LifecycleConfig::pause_on_focus_lost`); the design's 30 Hz present cap
  belongs with the game's Pause state (M13). Focus is read from the window
  at boot, so a window shown without focus (tests) starts in FocusLost.
- QuitPending dominates the state shown; a draining quit iterates at 10 Hz
  while nothing is visible and at the full rate otherwise.
- WM_QUERYENDSESSION, WM_ENDSESSION and TERMINATING run the recovery hook
  once and end the app at the next iterate without a drain, as the design's
  Running -> ShuttingDown says, even if the session end is later cancelled.
- `--frames N` counts presented frames (NULL acquires do not count).

**Why.** The spec states the rules but not these interactions; each choice
keeps a condition from being cleared by an unrelated event.

## ADR-041 Linux builds the platform with SDL's dummy and offscreen video (2026-11-08)

**Decision.** The Linux presets build se_platform, se_render, se_runtime,
the rampage executable and the platform tests. SDL3 is configured without
window-system and system-library detection (X11, Wayland, KMSDRM, OpenGL,
IBus, D-Bus, libudev, liburing, libusb, ALSA, JACK, PipeWire, PulseAudio,
sndio, OSS off; dummy and offscreen video on; `SDL_UNIX_CONSOLE_BUILD=ON`,
without which SDL refuses to configure). `SE_SDL_WINDOW_SYSTEMS=ON` restores
SDL's own detection for M18. The Vulkan GPU backend still compiles (SDL
bundles its headers) but no WSL test creates a device: window, GPU and
desktop tests carry the desktop or gpu labels, which the Linux test presets
exclude.

**Why.** The inbox, wake, lifecycle, quit and callback tests must run under
TSan and ASan, and WSL has no window-system development packages (installing
them is out of scope for M5; docs/gates.md).

## ADR-042 How the M4 parts run in the App (2026-11-08)

**Decision.** The hooks of ADR-037 are filled in as follows.
- Main thread: EnterMainThread calls RegisterMainThread and holds
  `RoleScope{kMain, "main", Qos::kHigh}` (registry slot 0, its 4096-record
  ring) until the end of SDL_AppQuit. It applies no CPU set: main's set
  matters once fork workers exist (M14).
- Boot step 1 resolves the pref path first (SDL_GetPrefPath and
  SDL_CreateDirectory need no SDL_Init), because the crash database
  (`<pref>/crashdb`) and the log file (`<pref>/logs/<app>.log`) live under
  it; step 5 now resolves only the content root.
- Crash capture is per process, not per App. The first StartCrashCapture
  registers the dump streams (the ThreadRegistry hook: the breadcrumb table,
  every slot's log ring as it is first allocated, the library ring as
  `LogRingStream(64)`; and one never-destroyed InputFrameRing that every
  App's SimDriver writes) and starts crashpad's handler from next to the
  executable. The CrashGuard is then kept until the process exits:
  crashpad cannot uninstall its filter or launch a second handler, and the
  SDL_AppInit catch-all, which runs after a failed App has been torn down,
  must still be able to request a dump. Later boots in the process report
  the same state. `--no-crash-capture` skips the handler (the in-process
  App tests, which boot many Apps in one process); the streams are still
  registered.
- The log worker starts at boot step 2, not step 9: LogWorker::Start
  configures the sinks and starts its thread together, and no other engine
  thread exists yet, which is what the design requires. A log file that
  cannot be opened is logged and the worker runs without it; it is not a
  boot failure. Step 9 starts the watchdog. Shutdown: Logging::Stop
  (SDL's log output to stderr, drain, spdlog::shutdown) at step 16, the
  watchdog join at step 17, then profile::Stop when `--profile` started it.
- The log worker's stderr sink writes through se::WriteConsoleLine, so a
  sink a test installed with SetLogSink receives the worker's lines as it
  receives synchronous ones (the callback tests read the fault lines that
  way).
- A fault in a callback, or in SDL_AppInit, requests its dump with
  DumpWithoutCrash on the faulting thread, so the dump shows that thread as
  it was; the watchdog's queue is for dumps its own thread writes.
- Boot step 8 reads the CPU topology and gives the (still empty) fork pool
  `max_workers = ForkPoolSize(topology)`.
- Test builds add the M4 exit hooks `--crash-test after-ticks:N` (an access
  violation on main right after tick N) and `--dump-test after-ticks:N`
  (the watchdog writes a dump after tick N; the ticks stop until it is
  written, then the App quits). The frame's StepPlan is cut so that the
  last tick it runs is N.
- The heartbeat flag is no longer set on every iterate: it marks long work
  on main that still makes progress (M9's loads, M12), so a flag that is
  always set would excuse every hang.

**Why.** Each point follows from a constraint the parts brought: crashpad's
one launch per process, LogWorker::Start's single call, SDL_AppQuit after a
failed boot, and in-process tests that boot repeatedly.

## ADR-043 The watchdog skeleton (2026-11-08)

**Decision.** `se::crash::Watchdog` (se_crash, which now links se_os) runs
as role kWatchdog through StartThread. It waits on a binary_semaphore with
a 1 s timeout (requests, the deadline start and the stop also release it,
at most once per wait) and measures periods on the unbiased clock. Each
period goes into the pure `HangDetector`: until Arm() (the first iterate)
only the 30 s boot budget applies, which is reported once with a dump and
never while a debugger is attached; armed, a period without iterate
progress that Suspended, Modal, the heartbeat or a debugger does not excuse
is a stall, and 5 in a row are a hang, once per episode, but only with
`declare_hangs`, which stays off until M12. `RequestDump(reason)` is main
only (an SPSC queue of 8): the watchdog thread writes one dump per request,
and Stop still writes the ones queued. `StartShutdownDeadline()` starts the
5 s deadline; on expiry the watchdog requests a dump, has the log worker
flush (se::FlushLog, 1 s) and ends the process with TerminateProcess
(exit code 3). The dump function is injectable for tests.

**Why.** The design's watchdog reads only atomics and never waits on main,
and main never waits on it; a queue main fills and a semaphore it releases
keep both true. Declaring hangs needs M12's GPU and compositor labels, so
M4 gives the interface and the boot budget only, as the spec asks.

## ADR-044 Game code run during shutdown is contained (2026-11-09)

**Decision.** The game hooks that run inside the App's teardown,
`GameSetup::on_shutdown` and every `GameState::OnExit` (from
`GameStateStack::Clear` at shutdown steps 5-6), go through `RunGameHook`
(engine/se_runtime/src/game_hook.h). It is defined in sdl_app.cc, the SDL
callbacks' fault boundary, because the teardown only ever runs inside an
SDL callback (SDL_AppQuit, or SDL_AppInit undoing a failed boot), so
ADR-006's list of boundary files is unchanged. A throw is logged
("fault in GameSetup::on_shutdown: ..."), a dump is requested, and the
remaining steps run, so logging still drains and stops (step 16) and the
watchdog still stops (step 17). No message box is shown: the shutdown
deadline is already running. A state whose OnExit throws stays on the
stack with the ones below it; destroying the GameRuntime destroys them and
their Worlds without another OnExit. `App::Shutdown` now returns the result
the process ends with, SDL_APP_FAILURE after such a fault (the exit code of
Release builds, which end the process themselves; elsewhere SDL has
already fixed the exit code from the last iterate). SdlAppQuit destroys the
App before main's RoleScope ends on every path, and `App::Teardown` marks
itself done only after its last step, so a teardown that an engine-side
throw cut short runs its remaining (idempotent) steps from ~App instead of
leaving the log worker, the watchdog, the device, the window and SDL
alive.

**Why.** A throwing hook used to skip the rest of the teardown and leave
the App to be destroyed after LeaveMainThread, where ~App's main-thread
assertion aborted (Debug and Dev), or the Release process ended with the
log worker undrained, so the fault's own records could miss the log file.

## ADR-045 The log consumer is detached without a deadline, and attached after its thread starts (2026-11-09)

**Decision.** `DetachLogConsumer` waits for every ring write mark in
progress without a deadline, replacing ADR-032's "at most 1 s". When it
returns, no thread uses the consumer any more, so LogWorker::Stop can
destroy its State. Each wait ends: a mark covers one push, or a flush
(SE_LOG_FATAL, 2 s; `FlushLog`, its caller's timeout) that the worker
keeps serving, because Stop detaches before it asks the worker to stop; at
shutdown the watchdog's 5 s deadline bounds the whole step 16 as well. The
caller must not hold a mark itself (no SE_LOG_* call path detaches).
`LogWorker::Start` now starts its thread first and attaches the consumer
after it: the thread waits for the outcome and runs only when the attach
succeeded. A thread that cannot be created therefore leaves no records
stranded in the rings, and a second worker that finds the first one
attached never drains the first one's rings.

**Why.** After a timed-out detach a producer still inside its mark (a
fatal flush lasts up to 2 s, longer than the old 1 s) could call
OnRecordPushed or poll the flush counter of a freed State. The old start
order attached before the thread existed, so a failed thread start left
whatever was pushed meanwhile in the rings with nobody to write it.

## ADR-046 The dump reader checks every record against its index (2026-11-09)

**Decision.** tools/minidump/read_dump.py relies on two writer rules and
reports records that break them under "problems" (and `--strict`):
- InputFrame stamps follow InputFrameRing: 2n + 1 while frame n is being
  written, 2n + 2 once complete (se_core/input_frame_ring.h). A slot in
  the ring must say "frame n, complete"; each record gets a `status` (ok,
  torn, overwritten, not written) and the frame it `holds`, and only ok
  slots are decoded. The crash probe writes its synthetic ring the same
  way (it used to count writes per slot).
- Log records in one ring take increasing sequence numbers (one producer
  at a time; the library ring assigns them under its lock).
One record is exempt: the oldest slot of a full ring is the one the
producer writes next, so a dump of a running writer may catch it being
overwritten. Such a slot is marked (an InputFrame slot holding frame
`head`, torn or not yet published; a log record flagged `may_be_torn`, and
`torn` when its sequence shows the next record's header) and not decoded
as its own index, but it is not a problem. Anything else that does not fit
is.

**Why.** The reader is both the diagnostic tool and the crash tests'
oracle; it accepted a slot holding a later frame, and a log record with a
new header over old text, as valid records of their index. Treating the
oldest slot of a live ring as a problem would make every dump of a busy
thread fail `--strict` (the probe's torn slot is that case on purpose).

## ADR-047 Other threads push SDL events only between the first iterate and shutdown step 4 (2026-11-09)

**Decision.** `WakeEventScope` gains `Open()` and `Close()`. The App opens
it at its first iterate and closes it at the start of shutdown step 4,
before the event filter is removed; outside that window WakeMain does
nothing, and Close returns only when no WakeMain that saw the window open
is still inside SDL_PushEvent. Any other engine code that pushes SDL events
from a thread other than main (M8's samplers, M9's lane) keeps to the same
window; WakeMain is the only push engine code makes today.

**Why.** SDL_PushEvent reads its event-watch list without SDL's lock
(SDL_DispatchEventWatchList checks the filter, and with no filter the
watcher count), while main changes that list: SDL_InitMainCallbacks adds
SDL's own watcher right after SDL_AppInit returns, SDL_QuitMainCallbacks
removes it before SDL_AppQuit, and the App sets the event filter at boot
step 4 and clears it at shutdown step 4. TSan reported the first race in
M5's wake test. The App's window lies inside the time its filter is set
and after SDL's watcher exists, so a push there never overlaps a change.
The design starts helper threads at boot step 9, inside SDL_AppInit, so the
rule has to live in the push itself rather than in the start order.

## ADR-048 The M4 bans have no opt-out and cover more forms (2026-11-09)

**Decision.** check_banned_api.py's rules thread, debugbreak, dump,
dump_header and timer cannot be silenced with `// se-allow(<rule>)` (the
thread rule's exception remains the se_os module). The thread rule also
matches `std::async`, `std::execution::par` and `par_unseq`,
`CreateRemoteThread*`, `QueueUserWorkItem`, `CreateThreadpool*` and
`TrySubmitThreadpoolCallback`; the debugbreak rule matches
`__builtin_debugtrap` and `std::breakpoint*`. check_includes.py's windows
ban matches `<windows.h>` in any letter case and the Win32 headers it
gathers (winbase.h, processthreadsapi.h, synchapi.h, ... ; the list is
WIN32_HEADERS) outside se_os, se_crash, se_platform and se_render. With
`--expect RULE`, a planted fixture passes only if every one of its files
violates RULE, so each form has its own fixture file; `dump_header` has its
own test (`checks.planted.dump_header`, on the dump_write fixture).

**Why.** The spec states these bans without exceptions ("anywhere",
"never DebugBreak"), and each gap the review found (an async launch, a
debug trap, `<WINDOWS.H>`, an opt-out comment) passed CI; a fixture whose
first file fires no longer hides a second form that does not.

## ADR-049 The log worker and the watchdog emit a zone per wake (2026-11-09)

**Decision.** `LogWorker drain` (each DrainOnce) and `Watchdog wake` (each
watchdog wake) are profiler zones. Inert unless `--profile` started Tracy,
they make both helper threads appear in a capture, and their first zone
after profile::Start() applies the Tracy name RoleScope recorded before the
start (ADR-029): the log worker starts at boot step 2, before the profiler.

**Why.** Tracy names a thread only through that thread's own events, and
neither helper had any, so the "named threads" gate could not pass.

## ADR-050 The SDL presentation patch: one header, hooks, files of its own (2026-11-09)

**Decision.** cmake/patches/sdl3-3.4.20-se-presentation.patch (a `git
diff` against the extracted SDL3-3.4.20 tarball, with a short description
above the first hunk) adds:
- `include/SDL3/SDL_gpu_se.h`, the only new public header: nine
  `SDL_SE_`-prefixed functions (capabilities, frame latency set and wait,
  present statistics, GPU timing begin/end/read, device status, native
  handles) and their types, versioned by `SDL_SE_GPU_PRESENTATION_VERSION`
  (1);
- `src/gpu/SDL_gpu_se.c`, the entry points: argument checks, then a
  dispatch on the device's backend name ("direct3d12", "vulkan") to an
  `SDL_SE_GPUBackend` table (`src/gpu/SDL_gpu_se_c.h`); other backends
  report every capability unsupported;
- per backend, `SDL_gpu_<backend>_se.h` (the state the backend's window,
  renderer and command-buffer structures carry as one `se` member each,
  and the hook prototypes), included once after the backend's fence type,
  and `SDL_gpu_<backend>_se_impl.h` (the hooks and the table), included
  right before the backend's `SDL_GPUBootstrap`, so it sees the private
  structures and static functions. Both are headers so that SDL's
  `*.c` source glob does not compile them on their own.

The edits to SDL's own files are hook calls and the three `se` members,
each marked `// simple-engine-2 presentation patch`: 20 lines in
SDL_gpu_d3d12.c and 30 in SDL_gpu_vulkan.c (docs/deps.md lists them). The
new functions are not entered in SDL's dynamic API tables: the engine links
SDL statically, where that is not needed; a shared build or an upstream
offer would run SDL's gendynapi.py. Nothing changes until a new function is
called: the D3D12 waitable flag is added when SetFrameLatency first asks
for it, Vulkan present ids only while a latency is set, display timing
after the first statistics query, timestamp query objects at the first
Begin; the Vulkan extensions are enabled at device creation when the
device has them (ADR-051), which changes no behaviour. `.gitattributes`
keeps `cmake/patches/*.patch` byte-exact (`-text`): git's CRLF conversion
on Windows would break both `git apply` against the LF tarball and the
SHA256 the build checks. cmake/Dependencies.cmake sets `SE_SDL_PATCHED`
when it applies the patch and se_render passes it on as the compile
definition `SE_SDL_PATCHED=0/1`; `base` sets `SE_SDL_PATCH=ON` explicitly
and msvc-unpatched-dev `OFF`. msvc-unpatched-dev gains test and workflow
presets (labels bench and desktop excluded, as in msvc-dev) and
`SE_TRACY=ON`, so that it is msvc-dev without the patch: without Tracy,
clang-tidy rejects the non-profiling expansion of SE_PROFILE_ZONE_DYNAMIC
(`sizeof` of a string_view, bugprone-sizeof-container), which no other
preset that runs the lint label builds.

present.h is the spec's API with two additions that change no call: every
function carries `SE_REQUIRES(g_main)` (the thread-safety analysis, as in
gpu_device.h), and `present_count` is documented with DXGI's meaning, the
present count of the most recently displayed present in the numbering of
`last_present_count` (the spec's comment "presents the swapchain issued"
describes `last_present_count`).

**Why.** Rebasing onto a later SDL has to stay mechanical: the new code
lives in files SDL does not have, and each touched line in SDL's files is a
one-line hook whose position a later SDL keeps. SDL's backends keep their
structures private to one translation unit, so the backend code is
included into it rather than compiled beside it.

**Note.** The SDL 3.4.20 tarball's AGENTS.md and CLAUDE.md say that
contributions made with generative AI are not accepted by SDL. This patch
was written by an AI agent, so it must not be offered upstream as it is
(the design's "offered upstream" needs the owner's decision; docs/gates.md).

## ADR-051 The frame-latency wait on D3D12 and Vulkan (2026-11-09)

**Decision.**
- D3D12: SetFrameLatency(n) on a swapchain without the waitable records n,
  waits for the GPU (D3D12_Wait), destroys and recreates the swapchain as
  SetSwapchainParameters does; every swapchain created while a latency is
  set gets `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (with
  ALLOW_TEARING when supported), every `ResizeBuffers` passes the same
  flags, and after each creation the patch calls
  `IDXGISwapChain2::SetMaximumFrameLatency(n)` and takes
  `GetFrameLatencyWaitableObject` (closed when the swapchain is released).
  Later calls only set the maximum latency. WaitFrameLatency waits on the
  waitable and a high-resolution waitable timer due at the timeout
  (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
  100 ns units rounded down, a millisecond backstop), so the wait never
  exceeds the timeout by the 15.6 ms timer tick; a zero timeout polls.
  Present mode changes and resizes keep the waitable (desktop test).
- Vulkan: at device creation the patch appends `VK_KHR_present_id`,
  `VK_KHR_present_wait` and `VK_GOOGLE_display_timing` when the physical
  device has them and the application did not already request them, and
  chains `VkPhysicalDevicePresentIdFeaturesKHR` and
  `VkPhysicalDevicePresentWaitFeaturesKHR` when
  `vkGetPhysicalDeviceFeatures2KHR` reports the features (it needs
  VK_KHR_get_physical_device_properties2 on SDL's 1.0 instance). If
  vkCreateDevice fails with the additions, it is called again without
  them. While a latency L is set each present carries its window's present
  number as its present id; WaitFrameLatency waits with
  `vkWaitForPresentKHR` for id `last + 1 - L`, the present L back from the
  next one. Ready without waiting: fewer than L presents with ids, or a
  target that went to an earlier swapchain; VK_TIMEOUT is kTimeout; out of
  date, surface lost and device lost return kReady (and record a lost
  device) so the pacer never blocks on a swapchain SDL is about to
  recreate. SetFrameLatency fails without both features.

**Why.** DXGI accepts the waitable flag only at creation and refuses a
`ResizeBuffers` whose flags differ. vkWaitForPresentKHR waits until the
swapchain's presented id reaches the target, so a target from an earlier
swapchain could wait for a present that never comes. Retrying device
creation keeps "missing extensions never fail device creation" even for a
driver that lists an extension it cannot enable. Measured on this machine
(RTX 3080, 165 Hz output): 120 of 120 waits ready within two refreshes at
latency 1 and 2 on both backends; NVIDIA's Vulkan driver has present_id and
present_wait.

## ADR-052 Present statistics, and composed windows without them (2026-11-09)

**Decision.**
- D3D12: GetPresentStats returns `IDXGISwapChain::GetFrameStatistics`
  (PresentCount, PresentRefreshCount, SyncRefreshCount) and
  `GetLastPresentCount`; SyncQPCTime is converted into the SDL_GetTicksNS
  domain by sampling SDL_GetPerformanceCounter (QPC) around SDL_GetTicksNS.
  False while minimized, on a DXGI error (DISJOINT) and while PresentCount
  is 0. `Caps::present_stats` means the backend can report statistics, not
  that the current presentation path produces them.
- Measured: on this machine (Windows 11 26200, RTX 3080) DXGI returns
  S_OK with every field zero for a windowed flip-model swapchain that DWM
  composes, with and without the waitable, for a small unfocused window
  and a large topmost one; `IDXGIOutput::GetFrameStatistics` returns
  DXGI_ERROR_INVALID_CALL in windowed mode. DXGI fills the statistics for
  independent flip (a borderless fullscreen window in front), which a
  desktop test may not take the screen and the focus for. The windowed
  statistics test therefore skips on D3D12 with that reason when no
  statistics arrive, and a hidden Catch2 test (`[.fullscreen]`, never
  discovered by CTest) checks them fullscreen; docs/gates.md has the
  command.
- Vulkan: `VK_GOOGLE_display_timing` when the device has it: the first
  statistics query enables `VkPresentTimesInfoGOOGLE` on the following
  presents (ids are the window's present numbers);
  `vkGetPastPresentationTimingGOOGLE` gives the newest displayed present;
  refresh counts are derived from actualPresentTime and
  `vkGetRefreshCycleDurationGOOGLE` from the first displayed present of
  the swapchain (refresh 1), and sync equals present (the sample is the
  vblank of that present). Times are CLOCK_MONOTONIC on Linux and taken as
  QPC nanoseconds on Windows (unverified: NVIDIA's Windows driver does not
  offer the extension, so the statistics tests skip on Vulkan here).

**Consequence for M6b and the integration.** With the windowed D3D12 path
reporting no statistics on this machine, a pacer that requires statistics
for Paced mode runs Standard in a window. A composed-mode miss source that
needs no new SDL API: at frame latency 1, WaitFrameLatency returns when DWM
latched the previous present, so its return time placed on the vblank
sampler's grid tells the refresh that took the present. That choice (or
borderless fullscreen for Paced) is the owner's; docs/gates.md, "Present
statistics in independent flip". Corrected at the integration (ADR-066): the
zeros were measured while the display slept; with it awake DXGI fills a
window's statistics, with DWM's displayed refresh (one after the vblank
that took the present), and the pacer's misses come from the latency
wait on the sampler's grid.

**Why.** The spec's statistics are DXGI's; reporting zeros as statistics
would make every present look undisplayed, and inventing them from DWM's
desktop-wide timing would bring back red team P2.

## ADR-053 GPU timestamps, device status, native handles, tearing (2026-11-09)

**Decision.**
- Timestamps: one query heap (D3D12, `D3D12_QUERY_HEAP_TYPE_TIMESTAMP`) or
  query pool (Vulkan) of 64 slots of two queries, one readback buffer
  (D3D12), created at the first BeginGpuTiming. Begin takes the next slot
  that no command buffer is recording into and writes the start timestamp
  (Vulkan: `vkCmdResetQueryPool` first, so Begin and End must be outside
  any pass, which SDL_SE_BeginGPUTiming asserts); End writes the end
  timestamp, and Submit writes it when End was not called, resolves both
  (D3D12 `ResolveQueryData` before the command list closes) and binds the
  slot to the submission's fence, taking a reference so the pooled fence
  cannot be reused while the result waits. ReadGpuTime finds the slot by
  fence: nullopt (false) while the fence has not signalled or when the
  submission was not timed; a successful read converts with
  GetTimestampFrequency (D3D12) or timestampPeriod with the queue family's
  timestampValidBits (Vulkan) and frees the slot. A slot whose result
  nobody reads is reused 64 timings later; a cancelled command buffer
  frees its slot. This replaces the spec's "a query heap and a readback
  buffer per frame in flight" with one ring that serves any number of
  frames in flight up to 64 outstanding results; it is no contract change.
- Device status: D3D12 `GetDeviceRemovedReason` (S_OK is kOk). Vulkan:
  every result checked by SDL's `CHECK_VULKAN_ERROR_AND_RETURN` (one hook
  line in the macro) and every result of the patch's own calls records
  VK_ERROR_DEVICE_LOST, and an unsubmitted fence created on first use is
  polled with vkGetFenceStatus, which reports a lost device.
- Native handles: ID3D12Device, the direct ID3D12CommandQueue and the
  window's IDXGISwapChain3; VkInstance, VkPhysicalDevice, VkDevice, SDL's
  single queue and its family, and the window's VkSwapchainKHR (a 64-bit
  value in SDL's struct, a pointer-sized value in present.h). Borrowed;
  the swapchain changes whenever SDL recreates it.
- Tearing: SDL 3.4.20 already presents IMMEDIATE with sync interval 0 and
  `DXGI_PRESENT_ALLOW_TEARING`, and creates every swapchain with
  `ALLOW_TEARING`, when `CheckFeatureSupport(PRESENT_ALLOW_TEARING)`
  reports it (SDL_gpu_d3d12.c, D3D12_INTERNAL_Submit and CreateSwapchain;
  verified in the source, unchanged by the patch); `Caps::tearing` reports
  that support on D3D12 and the surface's IMMEDIATE support on Vulkan.
  SetPresentMode calls SDL_SetGPUSwapchainParameters with SDR composition,
  which recreates the swapchain (keeping the waitable, ADR-051).

- Amended at the integration: SDL_SE_BeginGPUTiming and EndGPUTiming
  check the command buffer's submitted and pass flags only when the device
  is in SDL's debug mode. SDL_AcquireGPUCommandBuffer resets those fields
  only in debug mode, so on a Release device (debug mode off) a pooled
  command buffer still said "submitted" from its last use, and
  SDL_assert_release stopped every timed frame with an assertion dialog
  (msvc-release: `present.vulkan.GPU timing of a 4096x4096 clear` and the
  `render_gpu.d3d12.*` tests timed out waiting on it). The patch grows by
  five lines in src/gpu/SDL_gpu_se.c (docs/deps.md).

**Why.** A fixed ring needs no allocation per frame and no bookkeeping of
frames in flight, which the pacer's latency changes; the fence reference
is what makes "Read after its fence signalled" safe against SDL's fence
pool. SDL already reports device loss from nearly every Vulkan call
through that macro, so one line covers them.

## ADR-054 The pacer presents through a PresentPort; the clear-only port until the integration (2026-11-09)

**Decision.** The FramePacer talks to `se::PresentPort`
(`se_runtime/present_port.h`) and never to `se::present`. Its data types
mirror the present API under their own names, so that the header needs no
SDL include and the pure PacerPolicy can use them: `PresentCaps`,
`PresentWait`, `PresentStatistics` (DXGI_FRAME_STATISTICS semantics, plus
`last_present_count`), `GpuTiming`, `GpuDeviceStatus`,
`PresentNativeHandles`, and `SwapchainMode` (VSYNC, IMMEDIATE, MAILBOX) for
SDL_GPUPresentMode; the SDL handles in the GPU-timing calls are forward
declared. Beyond the present API the port has:
- `OnFrameSubmitted(SDL_GPUFence*)`: each frame's fence, owned by the port
  from then on (nullptr when the recorder has none);
- `WaitFrameFence(frames_back, timeout_ns)`: the N-L wait; a zero timeout
  polls, which is how the pacer learns `gpu_fence_pending_at_start`;
- `RepresentLast()`: HalfRate's Standard re-present;
- `WaitSwapchain()`: Standard's SDL_WaitForGPUSwapchain, so a fake can stand
  in for it.
The pacer reads `GetPresentStats` right after each present to learn that
present's id (`last_present_count`); the integration's adapter must report
it then, before the present is displayed (DXGI's GetLastPresentCount does).

Until the integration, `ClearOnlyPresentPort` (clear_present_port.h) runs
over M5's GpuDevice and FrameRecorder, both unchanged. It reports what the
unpatched SDL 3.4.20 has: `tearing` from SDL_WindowSupportsGPUPresentMode
(IMMEDIATE), every other cap false. Its fences come from an empty command
buffer submitted with SDL_SubmitGPUCommandBufferAndAcquireFence after each
frame (it signals once the frame's work before it on the queue is done);
SDL has no timed fence wait, so WaitFrameFence polls or blocks in
SDL_WaitForGPUFences. RepresentLast records another clear frame, the whole
composite in M5.

**Why.** The spec makes the port the contract between M6a and M6b. Mirrored
types under distinct names keep PacerPolicy free of SDL and make the
adapter a field-by-field copy; a fence behind the frame gives the N-L wait
real data now without rewriting M5's FrameRecorder, which M7 replaces.

## ADR-055 SDL's 60.001 Hz is the wrong number; the pacer uses the OS's current mode (2026-11-09)

**Decision.** M5 saw SDL report 60.001 Hz for the window's display while
VSYNC presents ran at about 165 Hz. Measured on 2026-11-09 (the
`pacing.display_rates` desktop test and probes; one active output,
`\\.\DISPLAY2`, an ASUS VG27AQL1A on the RTX 3080 at 2560x1440):
- QueryDisplayConfig's target refresh for the path: 165003/1000 Hz;
  EnumDisplaySettings(ENUM_CURRENT_SETTINGS): 165 Hz; DWM's
  DwmGetCompositionTimingInfo: 10000000/60606 (165.0 Hz) and DwmFlush
  every 6.05 ms (median); VSYNC presents of a clear frame: 164.987 Hz.
- SDL 3.4.20's WIN_GetRefreshRate (src/video/windows/SDL_windowsmodes.c)
  starts from dmDisplayFrequency (165), then replaces it with the refresh
  of IDXGIOutput::FindClosestMatchingMode for 2560x1440 R8G8B8A8. DXGI's
  mode list for this output has one mode at that size, 60.001 Hz (24 modes
  in all, every one near 60 Hz; GDI's mode list likewise has only 60 Hz at
  2560x1440), so SDL reports 60.001 Hz. The test reproduces SDL's
  computation from the OS queries and checks it equals SDL's number.
So SDL's display query is wrong and the present rate is right. The vblank
sampler could not add its own measurement: the display had been idle for
2.8 hours and was asleep (WaitForVBlank returned at once, and
D3DKMTWaitForVerticalBlankEvent failed with
STATUS_GRAPHICS_PRESENT_OCCLUDED); whether DXGI's mode list also lacks
165 Hz while the monitor is awake is a gates.md entry. A list of only
near-60 Hz modes looks like the driver's list without the monitor's own
modes, so SDL may be right while the monitor is awake and wrong whenever
it sleeps (as during unattended test runs, M5's included); the pacer does
not depend on which, because it reads the current mode. (Measured at the
integration with the display awake: DXGI lists 7 modes at 2560x1440 up to
165.003 Hz, SDL reports 165.003 Hz, and the sampler 164.965 Hz; the
60.001 Hz is the sleeping display's.)

The "display mode" the pacer compares the sampler with, and the period it
falls back to, is therefore the OS's current mode of the window's monitor:
`se_platform/display_output.h` QueryMonitorMode reads QueryDisplayConfig's
target refresh of the monitor's active path (MonitorOfWindow, then the
path whose source GDI name is the monitor's). SDL's refresh is used only
when that query has no answer (Linux, no active path). Both are logged at
boot and after every display change ("display: SDL reports 60.001 Hz; the
OS reports `\\.\DISPLAY2` ... at 165.003 Hz ..."). ADR-039's measured-period
fallback stays for a mode that is unknown or wrong anyway.

**Why.** With SDL's number every comparison with a correct sampler would
fail and Paced could never run on this machine. This deviates from the
spec's "SDL's mode" in M6b's period and mode rules.

## ADR-056 The vblank sampler: when it runs, what it ignores, how it stops (2026-11-09)

**Decision.**
- It runs whenever the App has a window on Windows, started at boot step 9
  before the watchdog and stopped at shutdown step 9 (HelperThreads::
  StopSamplers; HelperThreads::Stop stops it too). Not only in Paced: the
  pacer needs it to choose Paced, and Standard's frame clock uses its grid
  (ADR-057). A sampler that cannot start is logged and the App runs
  without it. EngineThreadsRunning counts it; its registry slot is in the
  dumps (crash.rampage_* check it).
- Stamps are QueryPerformanceCounter values after WaitForVBlank returns,
  mapped to SDL_GetTicksNS by one calibration pair taken on main at Start
  (SDL's ticks are QPC scaled from SDL's start). The period is the median
  of the last 64 intervals, published after 16; the spread is p90 - p10.
  An interval over 1.5 periods counts as that many vblanks and stays out of
  the ring.
- A wait that returns less than 1 ms after the previous one is not a
  vblank: WaitForVBlank returns at once while the display is off (measured,
  ADR-055). Such returns publish nothing; three in a row mark the output
  off and back off for 50 ms.
- The 100 ms watchdog is the stamp's age, judged by every reader against
  its own clock (Read(now)): a WaitForVBlank that never returns cannot mark
  itself, so a reading is fresh only while the newest vblank is younger
  than 100 ms. A reading is valid when the thread runs, the output of the
  latest SetMonitor is resolved, the reading is fresh, the output is not
  off and the period is known.
- Stop waits min(period, 100 ms) + 50 ms for the thread to leave its wait,
  then joins; otherwise it marks the sampler stale and detaches the thread
  (new `se::Thread::Detach`), which owns its state through a shared_ptr and
  ends when the wait returns (design "Shutdown Order" step 9). Stop does
  not log (it is noexcept); the App logs a detach after its teardown.
- SetMonitor is two atomic stores (the monitor, then a generation with
  release); main never takes the sampler's lock or wakes it, and the
  sampler checks the generation every vblank and every 20 ms of a backoff.
  Each resolve creates a new DXGI factory (an old one does not see outputs
  a display change added). `MonitorHandle` is an opaque pointer (the
  HMONITOR). se_platform now links se_os (StartThread) and dxgi, the only
  module that does.

**Why.** The spec bounds Stop and asks for staleness when WaitForVBlank
does not return, which only a reader or a timed join can see; the early
returns were found on this machine and would otherwise publish a
2.5 MHz "display".

## ADR-057 Frame starts on the vblank grid; misses against each present's target (2026-11-09)

**Decision.**
- FrameClock. With a valid sampler whose period agrees with the display
  mode, frames start on its grid: Paced one presentation interval before
  the frame's target vblank (ADR-016), Standard at the grid vblank at or
  before now (Standard waits for the swapchain at the end of the previous
  iterate, so now is just after a flip), VRR at its deadline; elapsed
  intervals are the rounded grid difference, so `vblanks_known` is true.
  Otherwise the frame starts on the wall clock with the period from
  ChoosePeriod (the mode, or ADR-039's measured period), as in M5.
  `--fixed-frame-dt` keeps M5's synthetic clock.
- Targets. Paced: the vblank PlanPacedWake chose. Standard: the frame
  start plus one presentation interval. RepresentLast's present targets
  one vblank later. Each present is registered with the id
  GetPresentStats reports right after it.
- Misses (PresentTracker): the statistics name the newest displayed
  present and its refresh; it missed when that refresh is after its
  target's refresh (the target time converted through the statistics' sync
  pair). A present the statistics skipped over missed only when its target
  is before the earliest refresh it could have shown on. Counters that go
  backwards or a present id that restarts reset the tracking. BeginFrame
  reports one result per frame to FixedStepper::OnPresentResult: missed
  when any present it resolved missed.
- The N-L fence: BeginFrame polls the fence of frame N-L; pending sets
  `gpu_fence_pending_at_start` (so k_max holds at k_rt while g is
  unknown), then it waits, except while Modal.
- EndFrame waits only after a frame that presented, and never while Modal.
  Paced sleeps to target - StepPlan::budget_b of PlanNext (B capped at the
  period plus the stepper's miss offset) with SDL_DelayNS; the timer p99
  in m is the measured lateness of these sleeps once 16 exist (0.5 ms
  before).
- The mode is chosen at Attach and re-evaluated every 0.5 s and after an
  output change. Paced is taken only after the sampler has been valid and
  agreeing for 0.5 s; leaving Paced is immediate (at the next frame when
  the sampler goes invalid). So a boot usually starts in Standard and turns
  Paced within a second, and every change is logged with its reason. A
  display is fixed-refresh when the sampler's spread is at most 10% of its
  period.

**Why.** Frame starts on the grid let Locked cadence count vblanks rather
than round wall deltas, in Standard too. Per-present targets catch the
"one refresh late" equilibrium (red team P1), which refresh deltas alone
cannot see. A delayed promotion keeps a sampler that has just re-resolved
from flapping the mode.

## ADR-058 HalfRate, adaptive latency and VRR in the pacer; the M6b options (2026-11-09)

**Decision.**
- HalfRate is PacerPolicy's HalfRatePolicy with the M6b rule: above 100 Hz,
  entered when more than 1% of the presents resolved over the last 2 s
  missed or a full-rate frame's planned cost ((T/dt) S_hi + R_hi + g_hi +
  m) exceeded the vblank period for 2 s; left after 4 s in which the
  full-rate p95 cost stayed below 0.75 T. Modal frames do not count. It is
  a pacer state (FramePacer::HalfRate, PresentInterval 2, the title, the
  log), not a Lifecycle state. se_core's HalfRateController (M2, a 5 s
  exit) stays for the stepper's virtual-display scenarios.
- Frame latency 2 when k_rt S_95 + R_95 and g_95 each fit in the
  presentation period but their sum does not, for 2 s; back to 1 after 4 s
  without that; an unknown g never asks for 2; a base latency of 2 (the
  editor, M10) is fixed. Each change calls SetFrameLatency.
- VRR: IMMEDIATE, a deadline every 1/120 s, or 1/60 s when the display mode
  is below 119.5 Hz (Windows does not report a panel's VRR range; its mode
  is the range's top). A deadline already past re-anchors at now, so a
  late frame causes no burst. The sleep aims early by the median measured
  lateness (at most 1 ms): over 10 s here frame starts were within
  0.287 ms p99 of their deadlines (`pacing.vrr_deadline`). No miss or
  HalfRate logic runs in VRR.
- `--pacing paced|standard|vrr|auto` (AppConfig). Auto is Paced when
  possible on Windows and Standard elsewhere; `paced` asks for Paced on any
  platform, with the same requirements. Without the SDL patch the App runs
  Standard, or VRR when asked.
- `--inject-late-frame FRAME:MS` (test builds): iterate FRAME sleeps MS
  milliseconds after its ticks, before it renders.
- Test builds show FramePacer::Summary() ("Paced 165.00 Hz x1 L1 miss 0")
  in the window title; the pacer logs its per-second statistics at debug
  level and its totals at shutdown.

**Why.** These are the spec's rules with the choices it leaves open made
explicit; the VRR lead keeps frame starts centred on the deadline instead of
always late by the timer's lateness.

## ADR-059 One HLSL source for DXIL and SPIR-V; SPIR-V committed, DXIL built (2026-11-09)

**Decision.** The renderer's shaders are HLSL in engine/se_render/shaders/
(`<name>.<vert|frag>.hlsl`). Vertex inputs and interstage values carry both
`TEXCOORD<n>` semantics (SDL's D3D12 backend binds attribute location n to
TEXCOORD n) and `[[vk::location(n)]]`; resources follow SDL's sets and
spaces (vertex uniforms b0/space1, fragment textures t0+s0/space2).
- SPIR-V: `tools/shaders/build_shaders.py spirv` runs glslangValidator
  `-D -V -S <stage> -e main --auto-sampled-textures` (through wsl.exe on
  Windows) and commits one lint-clean header per shader under
  shaders/generated/. `--auto-sampled-textures` is added to the spec's
  command: SDL_GPU's Vulkan backend binds sampled textures as combined
  image samplers, and without it glslang emits a separate image and
  sampler. The WSL presets run `checks.shaders.spirv_fresh` (regenerate,
  fail on any difference, a missing header or one without a shader) and
  `checks.planted.spirv_stale` (a header with one byte changed must be
  reported).
- DXIL: se_render's build runs the Windows SDK's dxc.exe (`-T vs_6_0|ps_6_0
  -E main -Wno-ignored-attributes`; dxc ignores the vk attributes) into
  build-tree headers; configure fails with the searched paths when dxc is
  missing. Linux builds have SPIR-V only (`kHasDxil`).
- `se_render/shaders.h` lists every shader with the resource counts given to
  SDL_CreateGPUShader; `render_gpu.Every SPIR-V blob ...` and `... DXIL
  blob ...` reflect the blobs (SPIR-V DescriptorSet decorations, the DXIL
  container's PSV0 resource table) and compare, and a planted mismatch
  must be caught.

**Why.** One source keeps the two backends from drifting; glslang's output
does not depend on the source path and was checked to be deterministic, so
the committed bytes can be checked exactly.

## ADR-060 stb in se_render: PNG decoding at boot, PNG writing for tests and tools (2026-11-09)

**Decision.** `se::stb` is an INTERFACE target over the pinned archive's
headers (SYSTEM, so its warnings stay out). se_render defines each
implementation macro in exactly one translation unit: image_decode.cc
(stb_image, `STBI_ONLY_PNG`, `STBI_NO_STDIO`: files are read through
se::fs) and png_write.cc (stb_image_write, `STB_IMAGE_WRITE_STATIC`). Only
the readback test hook, tests and tools call the writer; executables that
do not leave its object file out of the link. The texture table
(config/texture.json, ADR-026) is read by a small parser for its one shape,
an object of strings (`ParseTextureTable`), so glaze stays confined. At
boot step 7 the App decodes every texture of `GameSetup::texture_table` on
main and flushes the uploads before the first frame (no magenta flash); a
missing or undecodable file is an Error naming the path in the load report,
logged as an error, and its sprites draw magenta.

**Why.** SFML decodes PNGs with stb_image too, so the engine samples the
same texels as the legacy reference; the comparison and goldens need PNG
output in tests and tools only.

## ADR-061 The sprite pipeline matches SFML's sprites: straight alpha, nearest texels (2026-11-09)

**Decision.**
- Straight (not premultiplied) alpha, blended as SFML's BlendAlpha:
  colour = src.a * src + (1 - src.a) * dst, alpha = src.a + (1 - src.a) *
  dst.a. Textures are uploaded as decoded; the tint multiplies the texel as
  sf::Sprite's vertex colour does.
- Nearest sampling, clamp to edge, no mipmaps; no culling (flips reverse
  the winding).
- Vertices stay in canvas pixels with texel coordinates (ADR-022): 20 bytes
  (position, texel, tint as UBYTE4_NORM). A per-draw uniform block carries
  the canvas-to-clip transform and 1/texture size, so a texture reloaded at
  another size needs no new vertices. Indices are 32-bit, the same two
  triangles per quad, written once per growth.
- One vertex/index buffer pair and one transfer buffer, grown by doubling in
  the build phase (logged); vertices go through the transfer buffer mapped
  with cycle and are uploaded with cycle. One draw per QuadBatch.
- The camera's canvas maps into a target with `FitCanvas`: letterboxed,
  centred on whole pixels, Camera2D::integer_scale honoured when the
  canvas fits at least once. Targets (the composite, viewports) are
  R8G8B8A8_UNORM; their passes clear with cycle. The composite follows the
  window's pixel size at each frame start (a minimized window keeps the
  last size); the blit draws it with a full-target triangle and nearest
  sampling, or clears the swapchain while the composite has never been
  drawn.

**Why.** The legacy textures are pixel art drawn by SFML with smoothing off
and BlendAlpha; matching both keeps the legacy comparison within tolerance
(99.994% of the stress scenario's pixels within 8/255 at tick 600) and the
goldens identical across D3D12 and Vulkan on this machine.

## ADR-062 The frame transaction's fault boundary and each step's policy (2026-11-09)

**Decision.** engine/se_render/src/frame_recorder.cc is the
`// se-fault-boundary` file ADR-006 means by "the FrameRecorder
transaction": `FrameRecorder::RunBuildPhase` runs the build phase and
returns a throw as an Error (text copied into a fixed buffer first,
ADR-038). Policies, which `--inject-frame-fault STEP[:EVERY[:UNTIL]]`
exercises (frames are iterates, as for --inject-null-acquire; UNTIL, the
last faulting frame, is an addition to the spec's syntax so a test can see
drawing resume):
- build, vertices: nothing was acquired; in the game build this is a fault
  like any other callback fault: the App requests a dump, shows the
  message box (suppressed in tests) and returns SDL_APP_FAILURE, so the
  shutdown order runs (M10's editor will enter Play.Faulted instead).
- prepare (the frame command buffer): the frame is skipped and logged
  (first, then every 120th); the uploads still go out through
  PumpUploads.
- upload (the upload command buffer): the uploads stay queued.
- copy_pass: no new vertices, so no target is drawn; each keeps its last
  image and the blit still presents the last composite.
- viewport_pass: that target keeps its last image.
- acquire: a NULL swapchain texture, never a state change (the lifecycle
  counts it as M5 does).
- swapchain_pass: no blit; the acquired texture is still submitted and
  presented.
- submit: SDL_SubmitGPUCommandBufferAndAcquireFence runs (a buffer that
  holds a swapchain texture cannot be cancelled) and the failure is
  reported as SDL would: no fence is kept, the frame is not counted as
  presented.
Prepare acquires the frame command buffer first and the upload command
buffer second (the spec lists them the other way round), so a failed frame
buffer never leaves an upload buffer to cancel. The only cancel left is of
an upload buffer on which nothing was recorded; it goes through
`CancelUnacquired`, which asserts that no swapchain texture was acquired.
The guard submits the uploads (with their own fence) and the frame,
keeps the frame's fence (`TakeFence`; Renderer2D keeps the last three for
the pacer's N-L wait) and runs the retire queue. The INTEGRATION(M6) marks
are in frame_recorder.cc.

**Why.** The design's game build treats every exception as fatal, the
editor isolates Play; containing the throw in the renderer leaves that
choice to the caller. The other steps are SDL calls that report failure by
value, and a frame that cannot draw must not stop the game.

## ADR-063 GpuResources: handles survive reloads; lifetimes are counted (2026-11-09)

**Decision.**
- `Handle<Texture>{index, generation}`: the generation changes only when a
  slot is removed, so a handle stays valid across reloads and device loss.
  The spec's "generation flip" at residency is the slot's content
  generation (`ContentGeneration`), which M10's ImGui ids can key on.
- Resolve never returns null: null, unknown and removed handles
  (`stale_resolves`) and valid handles with nothing resident yet
  (`unresident_resolves`) get the built-in 1x1 magenta.
- QueueUpload creates the slot's next texture at once and keeps the payload
  (for RecreateAll). PlanUploads takes whole rows, oldest upload first, at
  512-byte-aligned offsets into one 8 MB transfer buffer; an upload becomes
  resident only after the fence of the batch with its last rows signalled,
  and a newer upload of the slot supersedes older ones (retired, counted).
  Allocation happens in PlanUploads (build phase); RecordUploads and
  UploadsSubmitted are noexcept.
- The retire queue runs after every submit: the frame's, an upload-only
  iterate's, a readback's.
- Every SDL_GPU object the renderer creates goes through counting helpers;
  ~GpuResources releases what is left and logs "GPU objects: N created, N
  released, 0 alive", or an error naming the counts per kind. The
  fault-injection tests check that line: MSVC's AddressSanitizer has no
  leak detection.
- Device loss: ReleaseAll retires everything and keeps slots and payloads;
  after the old device is idle and the queue has run, RecreateAll creates
  render targets again (empty) and queues every payload, so handles and
  ViewportTargets keep working (`render_gpu.*.Device loss ...` recreates
  the device and matches the image before).

**Why.** Panels and sprites name textures by handle every frame; a reload
that invalidated handles would force every holder to look them up again,
and SDL's D3D12 backend frees a released texture's container at once (red
team C2), so releases must wait for the submit.

## ADR-064 The App draws its render sources; R includes the frame's CPU work (2026-11-09)

**Decision.**
- GameSetup gains `texture_table` (relative to the content root; empty: no
  textures) and `camera` (the Camera2D a World is drawn with; default its
  primary camera). Boot step 7 creates Renderer2D after the device claims
  the window and loads the table (ADR-060); no boot step was added, so
  --fail-init-step keeps its numbering.
- Each rendering iterate draws `GameStateStack::RenderSources(plan)` as
  layers, bottom first; iterates that do not render (Suspended) call
  PumpUploads, so uploads finish while the window is minimized.
- FrameCosts' R is the iterate minus its ticks minus the swapchain wait;
  the render build, recording and submit count. This replaces ADR-039's
  "minus the whole frame transaction", which predates any CPU work in the
  frame.
- Test hooks: `--reload-texture NAME@FRAME[=FILE]` (FILE relative to the
  content root) and `--readback FRAME:FILE` (the first rendered frame at or
  after FRAME), both repeatable, frames being iterates.
- The minimized-reload desktop test minimizes and restores its own window
  with ShowWindow(SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE), as M5's
  Suspended test does, not with SDL_MinimizeWindow and SDL_RestoreWindow,
  which activate the next window and ours (SW_MINIMIZE, SW_RESTORE) and so
  take the focus from whoever uses the machine. The window events and
  states are the same.

**Why.** The runtime stays game-agnostic (the camera and the table are the
game's); the swapchain wait is not CPU cost, while building and recording
are, and the pacer's B needs them.

## ADR-065 The legacy image comparison runs headless; goldens per driver (2026-11-09)

**Decision.**
- `legacy_replay --image-out TICK:FILE` (legacy tree, app/headless only)
  runs to TICK (it replaces --ticks), then draws the registry's Sprites in
  Sandbox::render's order into a fresh sf::RenderTexture of the configured
  resolution cleared to opaque black, created before the run while the
  headless engine's hidden 64x64 window provides the OpenGL context. No
  visible window is needed, so the gates.md fallback is not used; the
  comparison is a gpu and legacy test.
- tools/render_shot runs the same scenario, input and tick headless
  (rampage_test_support, one tick per frame) and draws the RenderList at
  alpha 1 into a composite of the camera's canvas size cleared to opaque
  black. `--compare` applies the tolerance in C++ (stb decoding): Python's
  stdlib decodes a 1920x1080 PNG in seconds; tools/render_shot/
  compare_images.py is the same rule as a stdlib tool with its own test.
- The engine presets register `render.legacy_image.<driver>` when this
  checkout has a legacy build tree (out/build/legacy-msvc-release or
  -debug) or SE_LEGACY_REPLAY names one; the legacy presets register
  `legacy.image_compare.stress` when an engine build has render_shot.
  Configure-time discovery, so a tree built later needs a reconfigure.
- The legacy Sandbox draws in EnTT's view order (newest first), the engine
  by (layer, texture, z, spawn serial); overlapping sprites can differ, and
  the tolerance absorbs it.
- Golden images (tests/golden/render/d3d12, vulkan) are compared in C++
  with the same rule; on this machine both drivers produce identical
  files.
- The comparison adds one condition to the spec's tolerance (same size,
  98% of the pixels within 8/255, mean absolute difference at most
  2/255): at least 90% of the drawn pixels are within 8/255, a drawn pixel
  being one more than 8/255 from the background (the expected image's
  most frequent colour) in either image. The spec's three conditions
  alone pass an image that is missing almost all of a sparse scene: the
  stress scenario's ships cover 4205 pixels at tick 600 (0.2% of
  1920x1080), and an image with only the player drawn passed against the
  sandbox (99.81% within, mean 0.294/255). With the condition the stress
  scenario still matches at tick 600 (97.19% of the drawn pixels within,
  on both drivers), as do the goldens and the fault-test readbacks, and a
  blank image fails (`render_gpu.An image missing its sparse sprites does
  not match`, tools.compare_images).
- `checks.gpu_texture_members` (tools/checks/check_gpu_members.py) scans
  every module but se_render, tests and tools included, for an
  SDL_GPUTexture* stored in a class, struct or union (a pointer member or a
  container of them), following braces through each file;
  `checks.planted.gpu_texture_member` must catch a struct member, a class
  member with a brace initializer and a vector of pointers.

**Why.** Rendering the reference headless keeps the comparison in the
commit gate instead of a desktop-only test; a separate checker keeps the
member scan (which needs brace tracking) out of the line-based banned-API
scan; the drawn-pixel condition makes "matches the legacy image" fail
when the sprites are missing, which the background alone would hide.

## ADR-066 Paced counts misses from the frame-latency wait on the vblank grid; statistics are counted beside it (2026-11-09)

**Context.** M6a found (ADR-052) that DXGI kept a window's frame
statistics at zero, and NVIDIA's Windows Vulkan driver has no
VK_GOOGLE_display_timing, so a source of hits and misses other than the
statistics was needed for Paced in a window. Measured at the integration
on 2026-11-09:
- While the display sleeps (as in every M6a and M6b run, and the
  integration's first ones) WaitForVBlank returns at once, the vblank
  sampler has no valid reading, Paced cannot run, and DXGI's window
  statistics are zero.
- With the display awake, DXGI fills a window's statistics, but they
  describe DWM's composition: over 20 s of Paced rampage on D3D12 each of
  3296 presents was reported displayed one refresh after the vblank its
  latency wait showed it taken at, although every present was submitted
  at least 0.8 ms (p50 1.5 ms) before its target vblank and every wait
  returned 0.35 ms (p50; p99 1.2 ms) after that vblank. Counted as misses
  against the target they read as 54% to 100% late (HalfRate's own phase
  sometimes lets DWM take a present one vblank early) and put Paced into
  HalfRate for nothing; M6a's per-refresh invariant also fails at times
  (one refresh shows two presents).

**Decision.**
- The pacer's miss source, per frame: the frame-latency wait on the vblank
  sampler's grid whenever it can resolve the frame's present (the waitable
  or present_wait exists, and the frame has a target on the grid: Paced,
  or Standard with a valid sampler); otherwise present statistics (a
  backend with statistics but no waitable, such as Vulkan with display
  timing and no present_wait); otherwise none. Present statistics are
  still read every frame and counted beside the source
  (PacerFrame::stats_resolved/stats_missed, PacerTotals::stats_*), and
  `pacing.paced_misses` prints both.

| Backend | Path | Pacer's source | Statistics |
|---|---|---|---|
| D3D12 | a window DWM composes | the waitable's return on the grid | DWM's displayed refresh (taken + 1), reported beside |
| D3D12 | independent flip (borderless fullscreen in front) | the waitable's return on the grid | DXGI's displayed refresh, reported beside (not measured: gates.md) |
| Vulkan | present_id and present_wait (NVIDIA here) | vkWaitForPresentKHR's return on the grid | none (no display timing) |
| Vulkan | display timing, no present_wait | present statistics (ADR-052's derived counts) | the source |
| any | no waitable (unpatched), no valid sampler (display asleep, Linux), VRR, `--fixed-frame-dt` | none: Standard, or no miss logic | |

- The wait. With frame latency L the wait after a present returns once the
  present L - 1 before it has left the present queue: DXGI signals the
  waitable when the compositor (or the display) takes the present at a
  vblank; vkWaitForPresentKHR returns when it has been presented. Every
  wait polls (timeout 0) first and then blocks. A wait that blocked
  returned at the vblank that took its present: it was taken
  floor((return - target + slack) / period) vblanks after its target,
  slack = min(1.5 ms, period / 4) (the sampler's stamps, taken after
  WaitForVBlank returns, trail the true vblank); one or more is a miss.
  A wait that was ready at once or timed out does not say when: the D3D12
  waitable is also signalled at vblanks while the queue has room, so the
  wait after a late frame (idle vblanks) is ready at once. Such a present
  is a miss when it was submitted after its target plus the slack (it
  cannot have been taken before it was submitted: at least
  floor((submit - target - slack) / period) + 1 vblanks late), and
  unresolved otherwise, never a hit. Presents without a target or whose
  wait never ran (Modal frames) are unresolved.
- The offset is 0, measured over six 20 s runs: the waits return after
  the target's sampler stamp at p50 0.02 to 0.35 ms and p99 0.12 to
  1.2 ms on both backends, at most 2.4 ms, against a 6.06 ms period; a
  one-vblank offset would have shown as every present late.
- Targets lie ahead of now by the same slack (EarlySlackNs): PlanPacedWake
  takes the first grid vblank after now + slack, and Standard's target is
  one interval after the grid vblank at or before now + slack (its frame
  start stays the vblank at or before now). Without this a fake display
  whose stamps trail the waits' returns by 0.5 ms put Paced into a
  one-refresh-late equilibrium (every target a vblank that had just
  passed), which the latency wait reported as every present missed.
- Paced needs the waitable and a valid, agreeing, fixed-refresh sampler,
  no longer present statistics. PacerFrame carries `miss_source`,
  `late_vblanks`, `unresolved`, `wait_blocked`, `wait_phase_ns` and
  `submit_ns`; PacerTotals counts the presents each source resolved and
  the unresolved waits; the per-second log, the shutdown line and the
  desktop tests print them. A wait's result is recorded in the frame that
  waited and reported to FixedStepper::OnPresentResult and HalfRate at the
  next frame; statistics' results arrive at a frame start.
- In Standard's HalfRate the re-presented composite is registered and
  waited for like the frame's own present (one wait per present).
- Measured with the display awake (desktop tests, 20 s of rampage, Paced
  at 165 Hz): D3D12 0 or 1 of about 3298 presents late (0.03% at most); the injected 30 ms frame one
  miss (submitted 28 ms after its target), on time from the next frame.
  Vulkan 50 to 82 of about 2250 (2.2% to 3.6%) one vblank late, with the
  same submit lead as the hits (p50 2.3 ms against 2.2 ms): NVIDIA's
  Vulkan path in a window drops that many whatever the lead, so
  `pacing.paced_misses_vulkan` reports it and the M6 exit stays on
  rampage's default D3D12 (gates.md).

**Why.** What the pacer controls is whether a present is taken at the
vblank it was planned for; the wait measures exactly that on both
backends, against the sampler's clock of the window's output. A window's
DXGI statistics add DWM's own refresh of composition, which the pacer can
neither avoid nor act on, and its displayed latency is what PresentMon's
capture measures (gates.md). This reverses the integration brief's order
("statistics preferred when they are non-zero"), whose premise was that
non-zero statistics come only from independent flip. Counting unknown
waits as hits would report numbers the hardware did not give.

## ADR-067 The PresentPort adapter and the GPU timing hooks (2026-11-09)

**Decision.**
- `RenderPresentPort` (se_runtime/render_present_port.h) is the PresentPort
  over se::present and Renderer2D, and replaces M6b's ClearOnlyPresentPort
  (deleted). Caps, latency, waits, statistics, timing, status, handles and
  the present mode are se::present field by field. Without the SDL patch
  `tearing` and SetPresentMode come from SDL itself
  (SDL_WindowSupportsGPUPresentMode, SDL_SetGPUSwapchainParameters), so
  VRR still runs unpatched as ADR-058 says; every other cap is false.
- Fences: Renderer2D keeps each submission's fence
  (FrameRecorder::TakeFence) in its ring of three, so OnFrameSubmitted has
  nothing to keep (a fence handed in is released) and WaitFrameFence reads
  Renderer2D::FrameFence(frames_back): a zero timeout polls, a longer one
  blocks in SDL_WaitForGPUFences (SDL has no timed wait). RepresentLast is
  Renderer2D::RepresentLast; WaitSwapchain is SDL_WaitForGPUSwapchain.
- GPU timing: FrameRecorder::Prepare calls present::BeginGpuTiming right
  after the frame command buffer is acquired, Submit calls EndGpuTiming
  after ending any pass and before SDL_SubmitGPUCommandBufferAndAcquireFence;
  at each Frame() start Renderer2D reads, oldest first, the kept frames
  whose fences have signalled (present::ReadGpuTime consumes the result)
  and reports the newest in Renderer2DStats (`gpu_timed`, `gpu_ns`).
  RepresentLast's blit is timed by the patch but not read. The App passes
  it to CostTracker::AddFrame, so FrameCosts' g_hi and g_95 are measured
  (unknown, NaN, when nothing arrived: unpatched, or a frame that did not
  render). Measured here: 0.012 ms (D3D12) and 0.017 ms (Vulkan) for the
  sandbox frame.
- FrameCosts' R is ADR-064's (the iterate minus its ticks minus the
  recorder's swapchain wait) minus the pacer's N-L fence wait at
  BeginFrame, which is GPU time. The pacer's other waits run in EndFrame,
  after the costs are recorded. Without the fence term, an unpatched build
  (no waitable, so the fence of the last frame is pending at each frame
  start for most of a vblank) carried the wait in R_hi, held k_max at 1 at
  165 Hz, and a 64 Hz modal loop then ran 65 ticks a second instead of
  120 (`platform.Ticks continue during a modal loop with Modal set`
  failed in msvc-unpatched-dev with the display awake).
- Measured on D3D12: in Standard the N-L fence (frame latency 1) is still
  pending at nearly every frame start, about 0.3 ms after the waitable
  returned (164 fence waits a second); in Paced, after the sleep, about
  one a second; on Vulkan none.

**Why.** Renderer2D already owns the frame's fences and the composite, so
the adapter is a thin copy; reading timings at the next frame start needs
no wait, and timing only the frame's own command buffer keeps g the cost
of a frame.

## ADR-068 Device loss: DeviceLost, recreation on main, the test hook (2026-11-09)

**Decision.**
- The App reads present::QueryDeviceStatus at every iterate's start and
  after a submit that failed (a failed submit only sets a flag for the next
  start). kRemoved, and nothing else, enters the lifecycle's new
  DeviceLost condition (dominant after the quit phases): no rendering, no
  gameplay, a 10 Hz callback rate while recreation keeps failing.
- Entering it: the pacer detaches from the port (the sampler keeps
  running), the port goes, Renderer2D::ReleaseGpuObjects moves every GPU
  object through the retire queue (the GPU idle, the queue run), and the
  old device releases the window. The old device stays alive until the new
  one exists, so the renderer never refers to a destroyed device.
- Recreation, in the same iterate when it succeeds: a new GpuDevice with
  the same driver claims the window, Renderer2D::RecreateGpuObjects
  (GpuResources::RecreateAll re-uploads every kept payload and flushes;
  handles keep their values), a new RenderPresentPort, the pacer attached
  again (it chooses its mode and frame latency afresh), and the lifecycle
  back to Running with a frame-clock reset (no catch-up). Worlds are
  untouched. A recreation that fails is logged and retried every second;
  the fifth failure takes the callbacks' fault path ("GPU device
  recreation").
- `--inject-device-lost FRAME` (test builds): at the start of iterate FRAME
  the port reports kRemoved once. The desktop tests
  `render.fault.<driver>.device_lost` run rampage with it at frame 100 on
  the fault suite's synthetic clock and require DeviceLost, the recovery,
  exit 0 after 150 presented frames, every GPU object released, and the
  frame-140 readback to match the suite's no-fault reference readback
  within the golden tolerance (ADR-065). The reference readback is the
  "golden" here: the sandbox's committed golden is drawn by render_shot at
  another size.
- Measured: recovery takes about 70 ms on D3D12 and 150 ms on Vulkan with
  the debug layers on.

**Why.** The design enters DeviceLost only on a device-removed status and
recreates on main with handles kept; keeping the old device until the new
one is ready makes a failed recreation safe to retry, and recovering at the
frame start keeps the tick sequence, so the readback can be compared frame
for frame.

## ADR-069 The image comparison judges each sprite; the GPU-object checks close their gaps (2026-11-09)

**Context.** The M6/M7 review found that ADR-065's drawn-pixel condition
still passed an image with one sprite erased or drawn in the fallback
magenta: one ship of the stress scenario is about 6% of the 4205 drawn
pixels at tick 600 against about 7 points of headroom (97.19% within), and
each of the 60 sprites of `sprite_grid.png`, 19 of the 21 of `rotated.png`
and all 47 of `sandbox_600.png` could be erased with the golden still
matching. The legacy and engine images differ where ships overlap in
another draw order (ADR-065), and that difference is not small: the
largest connected group of differing pixels is 114 pixels, more than the
smallest ship (56 pixels), so a cap on differing clumps cannot tell a
missing sprite from draw order. Measured on the real pairs instead: no
pixel is drawn in one image and background in the other together with all
8 of its neighbours, no pixel shows the fallback magenta, and every object
(an 8-connected group of one image's drawn pixels) has at least 75% of its
pixels within 8/255 (46.8% for an untested pair at tick 604). The review
also found that check_gpu_members.py missed a texture pointer kept through
an alias or a smart pointer, and that se::present asserted only the
engine's main thread, not SDL's.

**Decision.**
- The comparison (tests/render_gpu/support/image_compare.h and its Python
  twin tools/render_shot/compare_images.py) keeps the spec's three
  conditions and ADR-065's 90% of the drawn pixels, and adds three:
  - every object of either image with at least 8 pixels has at least 25%
    of its pixels within 8/255 (specks under 8 pixels are left to the
    image-wide conditions): an isolated sprite that is missing, added or
    drawn in other colours has none;
  - at most 8 solid mismatches, pixels drawn in one image and background
    in the other whose 8 neighbours are too: a sprite missing from a group
    of overlapping ones leaves such a block (19 to 24 for the smallest
    ships), draw order and rasterization leave thin edges (0 measured);
  - at most 8 pixels within 8/255 of opaque magenta (GpuResources' fallback
    texture) in the actual image where the expected one is not.
- The legacy image of the stress scenario at tick 600 is committed
  (tests/golden/render/legacy/stress_600.png, 89 KB, byte-identical in
  every preset's run). `render.legacy_image.*` and `legacy.image_compare.*`
  fail when legacy_replay's image differs from it (SE_UPDATE_GOLDENS=1
  rewrites it), so the unit test `render_gpu.The sandbox golden matches the
  legacy image, sprite by sprite` sweeps the real pair in every preset,
  WSL included: every object of the engine image erased, and drawn
  magenta, must fail, and one moved by 40 pixels too. `render_gpu.Every
  object of each golden, erased or magenta, fails` does the same for each
  golden against itself (every eighth object of the 1920x1080 sandbox,
  which the legacy sweep covers in full). Synthetic cases pin each
  condition and show overlapping sprites in another order and sprites
  shifted by one pixel still matching. With the old conditions these
  tests fail; the real pairs keep their margins.
- check_gpu_members.py reports any class, struct or union member that
  names SDL_GPUTexture (a pointer, a reference, a container, a smart
  pointer, any template argument), an SDL struct that holds one
  (SDL_GPUTextureLocation, SDL_GPUTextureRegion, SDL_GPUBlitRegion,
  SDL_GPUBlitInfo, SDL_GPUColorTargetInfo, SDL_GPUDepthStencilTargetInfo,
  SDL_GPUTextureSamplerBinding, SDL_GPUStorageTextureReadWriteBinding), or
  an alias of either (using or typedef, aliases of aliases, collected over
  every scanned file first). Member function declarations, function
  pointers and members initialized by a call are told apart. The planted
  fixture adds an alias, a typedef chain, a unique_ptr, an SDL struct and
  initialized and annotated members;
  `checks.planted.gpu_texture_member_allowed` holds the allowed forms and
  must report nothing.
- se_render asserts `SE_ASSERT_GPU_THREAD` (the engine's main thread and
  SDL_IsMainThread) in every function, present.cc included;
  check_banned_api.py's rule gpu_thread bans `SE_ASSERT_MAIN_THREAD` in
  se_render, with no opt-out (planted: `checks.planted.gpu_thread`).

**Why.** Each added condition answers one way a frame can be wrong while
most of it is right, with thresholds set from the real pairs' measured
margins; nothing the spec's tolerance passed before fails now except the
planted damage.

## ADR-070 Unattended runs show no dialog: SDL's assertions print and abort (2026-11-09)

**Context.** SE_NO_MESSAGE_BOX=1 suppressed the engine's and the CRT's
boxes, but a failed SDL assertion (SDL's own, or the presentation patch's
debug-mode checks) opened SDL's Abort/Retry/Ignore box, which hung
msvc-release once until the CTest timeout, on the owner's desktop. The M6
exit "rampage runs in Standard unpatched" (`pacing.unpatched_standard`)
also ran in no scripts/ci.ps1 path: the patched desktop presets skip it and
the unpatched test preset excludes the desktop label.

**Decision.**
- `se::SuppressFaultDialogs()` (se_platform/fatal.h) turns off the CRT and
  Windows Error Reporting dialogs as ConfigureFaultDialogs did and installs
  an SDL assertion handler that prints `SDL assertion failed: '<condition>'
  at <file>:<line> (<function>)` to stderr and aborts. It does not return
  SDL's ABORT, which calls SDL_Quit first and can block or assert again
  inside a GPU call. ConfigureFaultDialogs calls it under
  SE_NO_MESSAGE_BOX=1; every Catch2 executable that links se_platform
  (the platform, pacing, present and render_gpu tests) calls it
  unconditionally through tests/support/no_dialogs_listener.cc, and so does
  render_shot.
- scripts/ci.ps1 also sets SDL_ASSERT=abort (SDL's default handler honours
  it) for any process that installs no handler.
- Tests: `platform.Test executables never show SDL's assertion dialog`,
  `platform.SE_NO_MESSAGE_BOX=1 replaces SDL's assertion dialog`, and
  `platform.sdl_assertion_aborts`, which runs a hidden case that fails an
  SDL_assert_release through tests/platform/run_app.py (`--expect-abort`:
  the process must end in abort(), exit status 3 on Windows or SIGABRT
  elsewhere, having printed the line; a dialog would run into the
  timeout). The case restores SIGABRT's default action first, since
  Catch2's own report allocates inside the signal handler, which TSan
  flags.
- `scripts/ci.ps1 -Desktop` (and so -Milestone) also builds
  msvc-unpatched-dev and runs `ctest -L desktop -R
  pacing[.]unpatched_standard --no-tests=error` there.

**Why.** Tests run unattended on a desktop someone may be using; a
diagnosable abort is the only acceptable end of a failed assertion there.

## ADR-071 HalfRate does not flap; Standard's HalfRate holds each frame for two vblanks; the probe's present is paced (2026-11-09)

**Context.** The review found four pacing defects. HalfRate entered on
misses above 1% but left on cost margin alone, so misses that cost does not
explain made it toggle: on NVIDIA's Vulkan path in a window it went on and
off three times in 20 s (2 s at full rate, 4 s at half), against the
design's "HalfRate does not flap at a 1% miss load". The entry log printed
the miss fraction after HalfRatePolicy had cleared its window (always
0.0%). In Standard, HalfRate registered each frame's present one vblank
later than it can be taken, so a present one vblank late counted as a hit;
it also re-presented after every frame, so a frame slower than one vblank
(HalfRate entered by cost) held the screen for three vblanks and the frame
starts were three apart. And the Suspended probe's present (the frame that
leaves Suspended) was never reported to the pacer: not counted, its fence
not waited for N-L, its present not waited for.

**Decision.**
- HalfRatePolicy leaves after a dwell of margin in which the full-rate p95
  cost stays below 0.75 T *and* at most 1% of the presents resolved over
  the last 2 s at half rate missed. The dwell is 4 s (ADR-058) and doubles,
  up to 64 s, at each entry within 10 s of the previous exit; an entry
  later than that starts again at 4 s. Unit tests: a constant 3% miss load
  enters once in 30 s and stays (five entries before); 3% at full rate and
  none at half rate stays 4, 8 and 16 s, four entries in 60 s (ten
  before). Measured on Vulkan with the display awake: one entry in 20 s,
  kept (its misses continue at half rate, 4.1% overall).
- The entry log names the cause and the fraction over the 2 s that entered
  it, and the dwell (`EntryMissFraction`, `EnteredByCost`, `ExitDwellSec`).
- Standard: a frame starts just after a flip, and its own present targets
  the next vblank; in HalfRate it targets two vblanks after the previous
  frame's own present was taken, unless that is earlier. The composite is
  presented again only when the frame's present was taken at the first
  vblank after its start (a slower frame already holds the screen for two
  vblanks), and the re-present targets the vblank after that take. Off the
  vblank grid (no valid sampler) it re-presents after every frame,
  untargeted, as before. This replaces ADR-057's "Standard: the frame
  start plus one presentation interval" and the re-present target of
  ADR-066. Unit tests: in Standard HalfRate a present one vblank late is a
  miss, and 8 ms frames at 165 Hz start two vblanks apart without
  re-presents or misses (both fail with the old targets).
- The App reports the probe's frame to the pacer (OnFrameSubmitted) right
  after BeginFrame, like any other frame. Desktop test `platform.The
  Suspended probe's present reaches the pacer` pushes an
  SDL_EVENT_WINDOW_OCCLUDED for its own window (nothing on the desktop
  changes) and requires the pacer to count every present the App made (it
  fails without the change).

**Why.** Hysteresis must be on the quantity that entered the state; a
cause HalfRate does not cure should keep it rather than toggle it, and one
it cures but cost does not predict should back off. Standard has no sleep
to keep a phase, so the take of each frame's own present is its phase.

## ADR-072 Present statistics are checked on their own, after the measured composition delay (2026-11-09)

**Context.** The M6 exit counts Paced misses "by present statistics";
ADR-066 made the latency wait the pacer's source, because DXGI's
statistics for a window DWM composes report every present after the
vblank that took it. Counted against the targets they read 100% late
(3295 of 3295 in the review's run), so the exit passed on ADR-066's source
alone, which needs the owner's acceptance. Measured now with the display
awake: each present of the 20 s D3D12 run (3295) is shown exactly two
refreshes after its target and two after the vblank its latency wait saw
it taken at (ADR-066 recorded one; its count did not separate the two).

**Decision.**
- PresentTracker reports, for each present whose refresh the statistics
  tell, the refreshes it was shown after its target (0 or fewer, 1, 2, 3,
  4 or more), and for each named present whose take the latency wait saw
  (`OnPresentTaken`, from each blocked wait), the refreshes it was shown
  after that take. PacerFrame and PacerTotals carry both
  (`stats_lateness`, `stats_delay`); the shutdown log prints them.
- `CheckStatistics`: the composition delay is the most frequent
  shown-after-take value (without takes, the most frequent lateness; the
  smaller on a tie); the statistics' misses are the presents shown later
  than their target plus that delay. The delay is a property of the
  presentation path, measured per run rather than assumed, and the
  lateness is counted against the targets, so a pacer that took its
  presents late still shows them beyond it (a unit test: every present
  taken one late and shown after DWM's two refreshes is a miss by the
  statistics).
- `pacing.paced_misses` (D3D12, the exit's gate) requires, beside the
  latency wait's under 1%, when the statistics resolved at least 80% as
  many presents as the wait: takes known for at least 80% of them, and
  under 1% shown later than target plus delay. It prints both histograms.
  Measured: 0 of 3295 beyond a delay of 2. `paced_misses_vulkan` reports
  only (NVIDIA's Vulkan has no display timing).
- Whether ADR-066's source with this check satisfies the exit's "by
  present statistics" is the owner's decision (docs/gates.md).

**Why.** The spec asks for the statistics' verdict; this gives it with the
one correction the composed path needs, measured on the run itself, so
the exit no longer rests on the change of source alone.

## ADR-073 Input edges are latched by stamp per tick; the rule lives in se_core (2026-11-09)

**Context.** M8 replaces M5's BasicInputService, which counted every press
and release since the previous latch on the next tick, with timestamped
edges placed on ticks (docs/design/mt-design.md "Placing edges on ticks").
The spec asks to check StepPlan::tick_window_end_ns (M2) against the
design and fix it if needed.

**Decision.**
- The check: FixedStepper::Plan already computed the design's windows
  (Locked: k equal splits of [prev_start, start), a stall or the first
  frame spanning one period; Free: start - remainder - (steps - 1 - i) dt,
  the remainder including debt). Nothing changed numerically; the
  formulas moved into `se_core/edge_placement.h` (LockedTickWindows,
  FreeTickWindows), which Plan now calls, beside the placement rule
  itself: `PlaceEdge` puts a stamp on the first tick whose window end is
  after it (windows are half-open, so a stamp equal to a window end
  belongs to the next tick), stamps after the last window on the last
  tick, and returns kEdgeWaits in a frame without a tick. It is in
  se_core, not se_runtime, so that M17's InputFolder can apply the same
  rule against the sim thread's deadlines. tests/kernel/edge_placement_
  test.cc pins it at 30, 60, 144 and 165 Hz.
- `InputService` (se_runtime) keeps physical inputs that are down and the
  changes not yet latched. Arrival order is not trusted: each Latch takes
  the changes stamped before its window end (all of them on the frame's
  last tick), sorts them by (stamp, arrival) and applies them to per-action
  holder counts; an action whose count goes 0 to 1 or 1 to 0 is an edge.
  Key repeats and duplicate downs of one physical input are dropped on
  arrival. A change that arrives after its tick was latched goes to the
  next latch and is counted (`late_changes`); one that arrives in a frame
  with no tick waits for the next frame's first tick.
- `InputEdge::order` is the edge's rank among the frame's edges with the
  same stamp (one key holding two actions gives orders 0 and 1).
- Up to kMaxEdgesPerTick (32) edges are stored per frame; pressed,
  released and held count every edge (`edges_not_stored` counts the rest).
  While nothing latches (a paused game), more than 1024 waiting changes
  fold into the held state, oldest first, without edges.
- Clearing held input (focus loss, the modal loop) releases every physical
  input that is down with a change stamped at the clear and zeroes the
  axes. Those releases carry a new `EdgeSource::kCleared`, appended after
  kReplay (replay files store the value; ReplayReader accepts it,
  read_dump.py names it "cleared").
- Keys carry kRawKeyboard when SDL's raw keyboard stamps them (Windows,
  SDL_HINT_WINDOWS_RAW_KEYBOARD=1, the runtime default; `--no-raw-keyboard`
  turns it off for the editor, M10), else kWinMessage, which on Linux
  means the window system's event stamps. Mouse buttons are kMouse,
  gamepads kGamepadSampler.
- Axes: SDL gamepad axis i is InputFrame::axes[i] (sticks in [-1, 1],
  triggers [0, 1]), latched per tick by stamp like edges. A bound half
  axis ("-lefty") presses at 0.5 and releases below 0.35.
- `--record FILE` writes the session's replay at shutdown (frames with
  their edges and sources, per-tick hashes; rampage's start bytes are its
  scene file), and se_replay gains `--edges-out` to list a replay's edges.
  The test hook `--push-key FRAME:MS:KEY[:up]` pushes a key event at the
  end of an iterate, stamped MS after that frame's start.

**Why.** Sorting by stamp at latch time is what makes chord order and the
8 ms test independent of which thread pushed first; holder counts keep
several keys and pads on one action consistent; a source of its own for
cleared releases keeps replays honest about where every stamp came from.

## ADR-074 The ActionMap: legacy key names as keycodes, pad defaults per game, unknown names fail the boot (2026-11-09)

**Decision.**
- `ActionBinding` (se_sim/game_module.h) gains `config_key` (the user
  setting that rebinds the key) and `pad` (default gamepad inputs, comma
  separated); both default to empty, so existing bindings compile.
  ActionId i is binding i, as before.
- The runtime reads the game's settings file (`GameSetup::settings_file`,
  config/config.json for rampage) with a small JSON reader of its own
  (se_runtime/src/json_reader.h; glaze stays in the component units,
  ADR-010): control-settings / key-bindings and sound-settings. A missing
  section means defaults; a section of the wrong type is a boot Error.
- Key names resolve to SDL keycodes, as the spec says (the key that types
  the character in the current layout, not the physical position the
  legacy tree's scancodes named): one letter or digit is that character;
  the legacy config names esc/escape, enter/return, space, tab, backspace,
  up, down, left and right are accepted; anything else goes through
  SDL_GetKeyFromName. "mouse-left", "mouse-middle", "mouse-right",
  "mouse-x1" and "mouse-x2" bind mouse buttons.
- A key name SDL does not know, a pad name it does not know, and a user
  binding that no action declares are boot Errors naming the binding and
  the key (step 10 fails).
- Pad names are SDL's (a, b, x, y, back, start, dpup, leftshoulder, ...)
  plus south, east, west and north, and half axes "+leftx" / "-lefty".
  The default gamepad bindings are code in the game module. rampage's:
  d-pad and left stick move (up dpup and -lefty, left dpleft and -leftx,
  down dpdown and +lefty, right dpright and +leftx); jump south; exit back;
  enter start; back east; a1-a6 west, north, leftshoulder, rightshoulder,
  leftstick, rightstick.

**Why.** Keycodes are what the spec names and what a user who writes "w"
means; failing the boot on an unknown name is the spec's rule, and a
binding nobody declares is almost always a typo. Pad defaults belong to
the game because only the game knows its actions.

## ADR-075 The input sampler runs inside ADR-047's window; the App opens gamepads (2026-11-09)

**Decision.**
- `se::InputSampler` (se_platform/input_sampler.h) is one thread, role
  kInputSampler, HighQoS, started with StartThread. Each period it calls
  SDL_UpdateGamepads, then waits with SDL_DelayNS for the next point of a
  grid of whole periods, early by the timer's measured overshoot (an
  average of the last waits, at most half a period; measured here: a 1 ms
  SDL_DelayNS wakes about 0.5 ms late, so a plain 1 ms wait polled at
  about 670 Hz); it checks its stop token every period. A sampler that
  fell behind, or one that was parked, restarts the grid. With SDL_HINT_AUTO_UPDATE_JOYSTICKS=0 nothing else
  updates joysticks, so the sampler is also what makes SDL notice a
  gamepad being plugged in.
- Parking is an atomic doorbell: SetParked flips a flag and rings a
  sequence number that the thread waits on with atomic::wait; the stop
  callback rings it too, so Stop joins a parked thread at once. Main calls
  SetParked every iterate with Suspended or Idle (a new
  `Lifecycle::Idle()`), never blocking.
- The rate: `PowerWatch` on main reads SDL_GetPowerInfo at most every 5 s
  and publishes 1000 Hz on AC, 250 Hz on battery (SetRate, a relaxed
  store; the next period uses it). Both numbers stay the design's
  assumptions until the gates.md rows measure them.
- ADR-047: SDL_UpdateGamepads pushes SDL events, so the App starts the
  sampler in its first iterate, right after WakeEventScope::Open, and
  stops and joins it at the start of shutdown step 4, before
  WakeEventScope::Close and the filter's removal. The design's shutdown
  order puts the input sampler at step 9; ADR-047's rule moves it before
  step 4 (the spec says so too). It is an App member for now; the
  integration may move it into integration::HelperThreads. It takes a
  ThreadRegistry slot through StartThread, so EngineThreadsRunning counts
  it and dumps show its breadcrumb.
- SDL reports gamepad buttons and axes only for gamepads that are open, so
  the App opens each one on SDL_EVENT_GAMEPAD_ADDED and closes it on
  SDL_EVENT_GAMEPAD_REMOVED (se_platform/gamepads.h, main only), after
  the InputService released what it held; the rest close before shutdown
  step 15.
- Tests: the sampler's loop with a fake poll and wait (park, unpark, stop
  while parked or running, the rate, the role) runs everywhere, TSan
  included; a desktop test drives an SDL virtual gamepad from a test
  thread through the real sampler (gamepad_desktop_test.cc).

**Why.** Starting the thread later than boot step 9 is the only way to keep
its pushes inside the window ADR-047 found safe; parking with a doorbell
keeps main from ever waiting on the sampler.

## ADR-076 se_audio: one engine on main, voices in a fixed table, the end ring and the sweep (2026-11-09)

**Decision.**
- se_audio (engine/se_audio; STATIC; ALLOW_ONLY se_core and miniaudio)
  is the only module that links miniaudio (every other module's FORBID
  list keeps it out; se_runtime's no longer names it, since it reaches
  miniaudio through se_audio) and the only one that includes it:
  check_includes.py's new project-wide `miniaudio` ban, test modules
  included, with `checks.planted.miniaudio_include`. miniaudio's target is
  marked SYSTEM, so its header brings no warnings or lint findings.
- `AudioEngine::Create` on main: kDevice (the default playback device; on
  failure the null backend, logged with the reason), kNull (miniaudio's
  null backend: a thread mixes in real time and nothing is heard) or
  kNoDevice (ma_engine's noDevice: frames pulled with ReadFrames). The
  engine gets its own ma_resource_manager with no job threads
  (MA_RESOURCE_MANAGER_FLAG_NO_THREADING): nothing loads through it, so
  the only miniaudio thread is the device's. 48 kHz stereo f32.
- Groups Music and Effects are ma_sound_groups; their volumes are
  config/config.json "sound-settings" / 100. Voice caps are per group (32
  music, 224 effects; at most 256 together), and a group at its cap
  stops its oldest voice first.
- PCM is `std::shared_ptr<const Pcm>` (f32 at the engine's format,
  decoded by `DecodeSound` with miniaudio's decoder: WAV, FLAC and MP3 are
  built into miniaudio; no new dependency). DecodeSound touches no engine
  state, so M9's lane can call it. A voice is one of 256 fixed records
  (never moved): its ma_sound (MA_SOUND_FLAG_NO_SPATIALIZATION and
  NO_PITCH; miniaudio still resamples a sound of another rate), the
  ma_audio_buffer that reads the PCM in place, the PCM reference, group,
  scope, cosmetic flag and start serial. A voice id is slot and
  generation.
- Ends: the end callback (miniaudio's thread) pushes the voice id into an
  se_core SpscRing of 256 and never waits; a full ring is counted. Sweep()
  (main, every iterate) drains the ring, then uninitialises every voice
  whose ma_sound_at_end is true; a ring entry for a slot already reused
  has the wrong generation and is ignored. A voice's PCM reference is
  dropped only after ma_sound_uninit has returned.
- Scopes: Play takes a ScopeId; ReleaseScope stops and uninitialises its
  voices. SetSound replaces a sound's PCM for new voices only.
- `--audio device|null|none` and the `SE_AUDIO` environment variable
  choose the App's output; a headless App defaults to null. Nothing a test
  runs may be heard: scripts/ci.ps1 sets SE_AUDIO=null, so do
  tests/platform/run_app.py, the crash and fault drivers and RunTestApp
  (unless a test chose), and the one real-device test
  (`audio.The playback device opens at volume 0 for 1 s and closes`)
  opens the device at master and group volume 0.
- Shutdown step 7: the App destroys the engine right after the Worlds:
  every voice uninitialised, the groups, ma_engine_uninit (which joins the
  device thread), then the PCM.

**Why.** A fixed voice table gives miniaudio stable addresses and the end
callback something it can read without locks; draining a ring and sweeping
at_end together means a full ring or a callback that never ran cannot
leak a voice. The ma_sound_uninit-before-release order is the design's
fence for PCM lifetime.

## ADR-077 The AudioBridge schedules at anchor + D; rampage's sounds and input buffer leave the hashes alone (2026-11-09)

**Decision.**
- `AudioEvents` (se_sim/audio_events.h) is a World ctx resource: systems
  push {sound key (Fnv1a32 of the sound table's name), group, cosmetic,
  positional, position}; at most 256 per tick, more are counted. The
  `AudioSink::Drain` contract is now SE_REQUIRES(g_sim_owner) and
  NullAudioSink clears the events, so headless runs never accumulate
  them. The ctx is not hashed and nothing reads the events back, so audio
  never changes simulation state.
- `AudioBridge` (se_runtime) is the App's AudioSink. Each frame, before
  the ticks, it recalibrates a `PcmClock` (se_audio/pcm_clock.h) and takes
  D = B + T/2: B is the frame's StepPlan::budget_b, T the FramePacer's
  vblank period of that frame (PacerFrame::vblank_period_sec, else the
  frame clock's period over the present interval). So D already comes
  from the real FramePacer (the integration's item is done here). After
  each World's tick it starts each event at the PCM frame of
  min(anchor + D, now + D), the anchor being the tick's window end; the
  min caps a start that a synthetic frame clock puts in the future.
- The PCM clock maps SDL_GetTicksNS to engine frames with the largest
  offset (frames - t * rate) of the last 64 calibrations: the leading edge
  of the device's 10 ms mixing staircase. Spacing between mapped times is
  exact, which is what the 8.33 ms requirement needs.
- Late: a start already passed plays at once; a cosmetic sound more than
  100 ms late is dropped and counted. Pan is the event's x against the
  first running World's camera (center, half its virtual width over zoom).
- Each World gets a voice scope at its first drain; the scope's token
  lives in its AudioEvents, so destroying the World releases its voices
  (through a weak reference to the bridge, so a World that outlives the
  bridge releases nothing).
- Sounds load synchronously on main at boot from `GameSetup::sound_table`
  (config/sound.json, name to file); a missing or undecodable file is
  logged and its events fail (counted). M9's lane will decode them.
- rampage: tools/audio/make_sounds.py writes assets/sounds/hit.wav and
  death.wav with integer synthesis (byte-identical everywhere;
  `checks.sounds_fresh` regenerates and compares them). EffectSystem asks
  for "hit" when an effect's own application lowers HP; DeathRule asks for
  the cosmetic "death". The M3 goldens (determinism_rampage, ref.sandbox.*)
  are unchanged.
- rampage's IntentSystem buffers the discrete actions (jump 8 ticks; exit,
  enter, back 6; a1-a6 10) in an `ActionBuffer` component (se_core
  InputBuffer) added to a player at its first discrete press. It is
  serialized but not hashed: it is a function of the recorded input and
  nothing consumes it before M13, whose rules will make it hashed (and
  rebase the goldens) when they read it.

**Why.** Scheduling against the tick's window end keeps the ticks' own
spacing in the sound; the cap on early starts keeps synthetic clocks
(tests, --fixed-frame-dt) from delaying sounds by seconds. Keeping both
the audio events and the input buffer out of the hash is what lets the M3
goldens prove that this milestone changed no simulation result.

## ADR-078 Cooked atlas pages: one texture per page, cooked at build time, re-cooked on the lane in dev builds (2026-11-09)

**Decision.**
- `.seatlas` (se_assets/atlas_page.h): a 64-byte little-endian header
  (magic, version 1, size, RGBA8 straight alpha, the nominal 256 KB block
  size, block and region counts, the source's size and hash, a hash of the
  pixels), a region table (name and rectangle) and a block table (offset,
  compressed and raw bytes), then independent LZ4 HC blocks. The decoder
  checks every offset and size, decodes one block per lane step and checks
  the pixel hash at the end, so a damaged page is an Error, never a wrong
  image.
- tools/asset_cook writes one page per texture of config/texture.json, with
  one region (the texture's name) covering it, at the PNG's path relative
  to the content root with `.seatlas` for its extension. CMake runs it
  (`se_cooked_textures`, a dependency of rampage and of the render tests)
  into `<build>/bin/cooked`, next to the executables, where the runtime
  looks by default (`--cooked-root` overrides it). A page whose recorded
  source hash and size match its PNG is left alone.
- The runtime reads pages only in Release. Dev builds read the source PNG
  too (in 1 MB steps) and compare its hash with the page's: a page that is
  missing, damaged or stale is re-cooked on the lane, in memory (stb_image
  in se_assets' cook.cc, one unsplittable step, as the design allows for
  import); nothing is written back. Hot reload re-cooks a changed PNG the
  same way (Urgent).
- se_render's PNG decoding (image_io.h, LoadTextureTable) stays for tests
  and tools (render_shot, the golden images, the M7 tests); the App no
  longer calls it.

**Why.** Packing several textures into a page needs M10's atlas packer; one
texture per page keeps every sprite's texture key and region as M3 and M7
made them, so the golden images and the legacy image comparison do not
move, while the region table means packing changes no format. Re-cooking
stale pages in dev builds keeps any content root (tests copy theirs)
working without a build step, and keeps "stb_image only at import": a dev
build's re-cook is an import.

## ADR-079 The background lane's contract (2026-11-09)

**Decision.**
- The request and completion rings are se_core's SpscRing of owned
  pointers (a `Request*` with its job, a `LaneCompletion*`): the ring stays
  trivially copyable and the payload moves with the pointer; the
  release/acquire of the ring's index publishes it. A full request ring
  keeps the job in a main-side queue pushed at the next Submit or drain; a
  full completion ring keeps the completion on the lane, retried every
  1 ms; neither side waits.
- The binary_semaphore has at most one release outstanding: main (and the
  stop_callback) release only when an atomic flag goes from false to true,
  and the lane clears it with an exchange after a successful acquire, which
  also synchronizes with the release that set it. The wait's timeout is
  the next scan (500 ms) or the earliest retry.
- Scheduling: after every step the lane picks again: the oldest ready
  Urgent step, else the oldest ready Background step. Only a strand's
  oldest job is eligible, and it runs Urgent when an Urgent job of the
  strand waits behind it (first in, first out wins over class within a
  strand). The scan is a Background step. A job that asks to retry (a
  sharing violation) is not ready until its time; the strand behind it
  waits.
- QoS per step: `se::SetCurrentThreadQos` (se_os): Windows' power
  throttling (kHigh off, kEco on); on Linux SCHED_OTHER and SCHED_BATCH,
  which an unprivileged thread can switch between in both directions (a
  nice value it raised could not come back). The lane switches only when
  the class changes.
- Stop() (shutdown step 8): queued and running jobs that are not Durable
  end at the next step boundary (the running step finishes), Durable jobs
  run to the end, then the thread joins; a Durable job pushed just before
  the stop is still found (the stop synchronizes with the pull that
  follows it). Completions left on the lane at exit are dropped (main is
  shutting down); a Durable one's file is already in place.
- `DurablePending()` counts Durable jobs from Submit until the lane has
  finished them; `DurableFailures()` counts the ones that ended with an
  Error. se_runtime's LaneQuitProvider reports kPending while any is
  pending and kCancel when one failed since its polls began.
- The lane calls WakeMain after pushing completions; WakeMain pushes only
  inside the ADR-047 window, so the lane may call it at any time.
- ADR-006's list of `// se-fault-boundary` files gains
  engine/se_assets/src/lane_fault_boundary.cc: each step and each
  TakeResult runs inside one try; a throw (bad_alloc, glaze's) ends the job
  with an Error (kIo, "lane job '<kind>' threw: ...") and the lane runs on,
  as the design's failure handling says for background jobs.
- `--lane-step-trace FILE` (test builds) records every step (ticket, kind,
  job class, the class it ran at, Durable, strand, Urgent steps ready
  besides it, start, duration); the App writes it after the teardown.

**Why.** Pointer rings reuse the ring the engine already has and verified
under TSan, without a second, move-only ring type. The flag makes the
semaphore's precondition (count at most 1) hold by construction. Boosting
a strand's head keeps per-path order without letting a Background save
hold up an Urgent one.

## ADR-080 se::fs for the lane: shared-delete reads, POSIX replaces, the sharing-violation retry, the fault injector (2026-11-09)

**Decision.**
- se_platform/file_stream.h: `FileReader` (stepwise reads) opens with
  `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` through
  CreateFileW on Windows (SDL's IOStream shares reading only), an
  SDL_IOStream elsewhere; `FileWriter` writes in steps and flushes to the
  device (FlushFileBuffers; SDL_FlushIO).
- `TryReplace` renames with POSIX semantics on Windows
  (SetFileInformationByHandle, FileRenameInfoEx with REPLACE_IF_EXISTS |
  POSIX_SEMANTICS): it replaces a file that readers which allowed
  FILE_SHARE_DELETE have open (they keep the old contents), so an engine
  read never blocks a save. A volume without it falls back to MoveFileExW
  (REPLACE_EXISTING | WRITE_THROUGH). ERROR_SHARING_VIOLATION,
  ERROR_ACCESS_DENIED and ERROR_LOCK_VIOLATION are `kBusy` (another process
  holds the file without FILE_SHARE_DELETE); SDL_RenamePath elsewhere.
- The save job (SaveRequest{path, bytes, revision, durable}) writes a
  unique temporary file in the destination's directory (1 MB per step),
  flushes it, and renames; kBusy retries every 20 ms for up to 2 s, then
  the job fails, removes its temporary file and reports the Error naming
  the sharing violation (the document stays dirty).
- Revisions: the lane keeps, per strand, the newest revision renamed into
  place, for its lifetime; a save older than it removes its temporary file
  and completes with `SaveOutcome::superseded`. Revisions are not stored in
  the files (each document type would need a format of its own for it); a
  new process starts with an empty table.
- Flushes (replays, SessionLogs, flight recorder) are Background saves on
  the path's strand with a process-wide increasing revision
  (`SubmitFlush`).
- se_platform/fs_faults.h, in every build but Release (SE_FS_TEST_HOOKS):
  `SetFsFaults{write_step_delay_ms, read_step_delay_ms, rename_busy_ms}`
  (atomics, set from a test thread while the lane runs): a throttled disk,
  and a sharing violation on every replace until rename_busy_ms after the
  first one it refused.
- `fs::PathInfo` gains `modify_time_ns` (SDL_PathInfo's), which the scan
  compares with the size.

**Why.** The spec asks for FILE_SHARE_DELETE reads and a 2 s retry; without
POSIX semantics the engine's own saves would still fail over a file it is
reading. Keeping revisions in the lane is enough for the order the spec
tests (one lane writes every file) without changing any file format.

## ADR-081 AssetRegistry, scopes and the hold (2026-11-09)

**Decision.**
- `Handle<T>{index, generation}` moves to se_core/handle.h (with the
  `Texture` tag and `TextureHandle`), so se_render's GpuResources and the
  registry share it; se_assets cannot include GPU headers.
- The registry's stores: Texture (with its Atlas, the page's regions,
  which shares the texture's slot and generation), Sound (shared_ptr<const
  PcmPayload>), Prefab and SceneChunk (shared_ptr to the payloads). An
  entry is keyed by the texture's name or the file's normalised path, lives
  while a scope references it, and is removed (its GPU slot retired through
  GpuResources) when the last scope is released; its handles go stale.
- The GPU is behind `TextureUploadPort`; se_runtime implements it over
  GpuResources (AddTexture, QueueUpload, IsResident, ContentGeneration,
  Remove), so an upload becomes resident when its fence signals (M7) and a
  reload swaps at a frame start. Headless runs have no port: textures
  settle without a GPU texture.
- A load that fails settles (the sprite draws GpuResources' magenta, the
  Error with path, line and column is logged and kept): it does not hold
  the scope, as M7's boot did not fail on a missing texture. A reload that
  fails keeps the old asset.
- The AssetScope per GameState is se_runtime's (asset_runtime.h): game
  modules are deterministic and may not link se_assets, so a game declares
  what a state needs through `GameSetup::state_assets(state_id)`
  (StateAssets: every texture of the table by default, or named ones,
  prefabs, scene chunks, sounds). The runtime keeps one scope per state on
  the stack (GameStateStack::Entries, new) and releases it when the state
  leaves.
- The hold: no ticks are planned while the top state's scope is not
  resident (logged when it starts and ends), and SimDriver's new
  `TickGate` (se_sim) is asked before each tick of TickInline after the
  stack's pending changes are applied, so a state pushed by one tick of a
  frame stops the rest of that frame until its scope is resident. A held
  tick is not dropped: it runs in a later frame with the same InputFrame
  sequence, so the per-tick hashes are those of an ungated run
  (tests/kernel/tick_gate_test.cc). A state whose scope is not resident is
  not drawn (its textures are not there yet). Frames and ticks therefore
  drift by the hold: `--readback-tick TICK:FILE` (test builds) reads back
  the first frame after a tick ran, and M7's fault-injection test
  (tools/render_shot/fault_test.py) compares the frame after tick 130 of
  each run instead of frame 140 (a fault on the uploads for 100 frames now
  holds the sim that long).
- Sounds: the lane reads the file and decodes it through `SoundDecoder`
  (se_assets/sound_decoder.h), which M8's se_audio implements with
  miniaudio's decoder at the integration; miniaudio stays confined to
  se_audio. Without a decoder a sound fails with kUnsupported.
- Completions the registry did not submit (saves, flushes) go to a foreign
  handler; the App's logs failures.
- se_runtime's FORBID list drops lz4_static: se_runtime links se_assets,
  whose closure has it; se_assets alone includes lz4.h (every other module
  bans it).

**Why.** The port keeps the asset module headless (its tests and TSan run
without a GPU) and the GPU rules in se_render. Holding at the tick gate
rather than inside RunTick keeps RunTick, which replays and the M17
threaded driver call, unchanged.

## ADR-082 glaze parses prefabs and scene chunks on the lane, in one translation unit (2026-11-09)

**Decision.** se_assets' payloads_glaze.cc reads prefabs into glaze's
`generic_u64` (64-bit integers exact, object members in file order) and
writes each member back as compact JSON, the text the ComponentRegistry's
JSON hooks take on main. Scene chunks are checked whole by glaze's validator,
then read by reflection into a struct whose entities are `glz::raw_json`
(each entity's text kept as written; skipping a raw value checks nothing,
hence the validator): the generic value and its writing back cost more
than ten times as much (a 256-entity chunk: median 1.2 ms against about
0.1 ms in Release), which put the chunk's step near the 2 ms bound under
load. A scene chunk is
`{"format": "se-scene-chunk-v1", "chunk": N, "entities": [ {...} ]}`
(M10's importer splits scenes into chunks, one lane step each). A parse
error carries the line and column glaze's validator reports.
tools/checks/check_glaze.py allows glaze headers in a translation unit
named `*_glaze.cc` besides reflect_glaze.h.

**Why.** The design gives the lane glaze parsing; one named unit per module
keeps glaze's compile cost (ADR-010) contained and visible to the check.

## ADR-083 SimThread: sessions built and destroyed on the sim thread, M17a's commands, the tick as a fault boundary (2026-11-09)

**Context.** M17a's sim thread (docs/design/dedicated-driver.md "Threads",
"Lifecycle") owns every running World while it runs and calls
SimDriver::RunTick unchanged on absolute deadlines. The App does not use
it before M17b; M17a proves it headless.

**Decision.**
- `se_sim/sim_thread.h`: `SimThread`, one thread (role kSim, OS name
  "sim", HighQoS) started through se::StartThread. It holds g_sim_owner for
  its whole body through the new `SimOwnerScope` (se_core/thread_role.h:
  the constructor asserts the role kSim; `AssertSimThread()` gives the
  analysis the capability in code the thread calls back into).
- What it runs is a `SimSession` (the GameStateStack, SimDriver,
  SessionLog, ReplayRecorder and AudioSink of one session, plus two hooks:
  `RecordedFrame(tick)` for recorded input and `FillRender(slot)` for the
  snapshot's render part), made by a `SimSessionFactory` on the sim thread
  at StartGame and destroyed there (Quit, StartGame again, a fault, the
  thread's end): Worlds live and die on their owner thread. se_sim stays
  free of se_render_core: the render part of a slot is the session's.
- Commands: an SpscQueue of 256 move-only `SimCommand`s, numbered by
  `Send` (1, 2, ...), with the Doorbell rung after each push. They apply
  at tick boundaries in order: StartGame, Pause, Resume (the grid is
  re-based so the next tick is due now; the epoch changes), Step{n} (only
  while paused: exactly n ticks at once, then paused again), SetTimeScale,
  Suspend, Quit, ApplySessionEntry (M2's SessionLog::Enqueue against the
  next tick). Every snapshot header acks the last applied sequence, and
  `AckedSeq()` reads it atomically.
- Input: each tick's InputFrame is the session's recorded frame when it
  has one (replays: the inline-against-threaded hash checks), otherwise
  the InputFolder's latch of the InputEvent ring at the tick's own
  deadline L_k (ADR-084).
- Publishing: after each tick the reliable section gets a TickRecord
  (session, tick, hash, Worlds stepped); session events (started, paused,
  resumed, stepped, suspended, dilated, faulted, ended, start failed) are
  added as they happen. A burst of catch-up ticks publishes the render part
  only for its last tick. With no free slot the sim skips the publish
  (counted in the header's dropped publishes) and keeps the deltas; when
  it goes idle (paused, suspended, no session) with deltas pending it
  publishes a reliable-only slot first, looking again every 1 ms while no
  slot is free, so a paused session's last records and events still
  arrive. Waiting while idle is the Doorbell; while running, PacedClock's
  WaitForDeadline.
- Faults: `sim_fault_boundary.cc` is the one file of se_sim that catches
  (marked `// se-fault-boundary`; it joins ADR-006's list): RunTick runs
  inside its try, a throw ends the tick, the SimThread destroys the session
  on its own thread, posts the exception and a FaultRecord (role, the
  thread's breadcrumb, tick, session, what()) to its FaultBox, publishes
  the faulted event and waits for Quit.
- Tests (tests/sim/sim_thread_test.cc, label unit, TSan in WSL): the
  threaded driver's per-tick hashes equal the inline ones at W = 0 on the
  kernel golden (determinism_kernel, with its logged edits through the
  replay's SessionLog) and on rampage's sandbox golden
  (determinism_rampage) and the three reference scenarios (effects,
  patrol, stress) over 3600 ticks, on a ManualClock; Pause, Step{5} (five
  ticks exactly), Resume and Quit; a throwing tick is contained. W = 1 and
  W = 3 are checks for M14 (docs/gates.md).
- Not built yet (the M17a agent was cut off; the integration finished the
  thread, not the platform side): se_render_core's SnapshotRenderSource
  (the newest three slots, tau = F - D', the pair and alpha, per-entity
  interpolation, starvation and hard cuts), the extraction of a World into
  a slot's render part (`SimSession::FillRender` is its seam) and
  bench.snapshot_publish_10k. M17b needs all three before the App can use
  the threaded driver.

**Why.** A factory run on the sim thread is the simplest way to keep
"Worlds are created and destroyed by their owner" true without a handover
protocol; recorded frames make the threaded driver's equality with the
inline one a pure function of the tick, as the spec asks (live latching
differs by design).

## ADR-084 PacedClock and the InputFolder (2026-11-09)

**Decision.** (Written by the M17a agent's code, recorded at the
integration.)
- `se_sim/paced_clock.h`: deadlines L_k = T0 + (k - k0) * P + phi with
  P = 1 s / (rate * scale) in integer nanoseconds (120 ticks take exactly
  one second at scale 1). `WaitForDeadline(k)` sleeps on a WaitClock to
  L_k minus max(spin floor 0.3 ms, the measured timer overshoot's p99 over
  256 samples), then spins with the pause instruction; it never returns
  before L_k unless stopped. `DueTicks` returns at most 8 ticks per burst;
  more than 250 ms behind, it re-bases T0 so the next tick is due now and
  counts a dilation. phi moves towards the platform's target by at most
  0.1 ms per tick, and only when the display period is a whole multiple of
  dt. Rebase (resume, time scale, dilation) bumps the epoch the snapshot
  header carries.
- The system WaitClock is se_os's `PreciseTimer` (a high-resolution
  waitable timer on Windows; clock_nanosleep(TIMER_ABSTIME) on Linux) and
  `MonotonicNowNs`, in waits of at most 10 ms so a stop is seen; se_sim
  reads no clock itself. `ManualClock` drives the clock deterministically
  in tests (auto-advance, or blocking on another thread's Advance).
- `se_sim/input_folder.h`: the InputEvent ring (4096, an SpscQueue of
  trivially copyable events stamped on the engine's monotonic clock at
  pump time) is folded per tick: every event stamped at or before L_k
  (M8's placement rule with the sim's own deadlines as window ends),
  press and release counts saturating at 255 so a tap shorter than a tick
  is never lost, edges with their sources, axes (the latest wins),
  ReleaseAll and heartbeats.

**Why.** Absolute deadlines with a measured spin margin are the design's
answer to timer granularity; a fold against the sim's own deadlines keeps
the edge-placement rule identical to the inline driver's.

## ADR-085 Snapshot slots and the SnapshotChannel (2026-11-09)

**Decision.** (Written by the M17a agent's code, recorded at the
integration.)
- `se_core/snapshot.h`: a slot is a header (session, generation, epoch,
  tick, anchor L_k + dt, latch L_k, acked command sequence, dropped
  publishes, tick and build cost, the World hash, the run state, a render
  flag), trivially copyable 68-byte `RenderItem`s (full entity id and
  spawn serial, position, rotation, scale, texture key, region, origin,
  tint, layer, z, teleported/visible/flip bits; the region and origin are
  SpriteRenderer's floats until M10's atlases give a region index, so not
  the design's 56 bytes), a sparse entity-index-to-item table per view,
  up to 4 views (one per drawn World: camera with a cut flag, item and
  sparse ranges, a stepped flag) and the reliable section (audio cues,
  session events, scope releases, tick records, an ended session's
  replay). Everything is reused vectors: a warm slot allocates nothing.
- `se_core/snapshot_channel.h`: a pool of 8 slots and two SPSC rings of
  slot indices (published and returned, 16 each, so neither fills), with
  the design's memory orders: the sim writes a slot, then pushes its index
  (release); the platform pops it (acquire), reads it through a const
  `SlotLease` and returns the index after its last read (release); the sim
  reclaims returned indices (acquire) before reusing a slot.
  AcquireForWrite returns null when no slot is free; the sim never waits.
  Debug builds track each slot's owner and assert every transition.
- `se_core/spsc_queue.h` (move-only values, with a peek), `doorbell.h` (a
  monotonic sequence with atomic wait, rung by a stop_callback) and
  `fault_box.h` (exception_ptr, record and replay written, then a release
  flag; the first post wins) are the other channels.

**Why.** Index rings over a fixed pool give exactly one owner per slot at
any time with no read-modify-write on shared indices, which the property
tests (randomized schedules) check.

## ADR-087 Sounds decode on the background lane into one shared PCM type; the state's scope holds them (2026-11-09)

**Context.** M8 decoded the sound table on main at boot (`App::LoadSounds`,
ADR-077 said "M9's lane will decode them"); M9 gave the lane a SoundDecoder
port (se_assets/sound_decoder.h, ADR-081) that se_assets could not
implement, since miniaudio is confined to se_audio (ADR-076) and se_audio
may not see se_assets (ALLOW_ONLY se_core). The two modules also each had a
PCM struct (se_audio's `Pcm`, se_assets' `PcmPayload`), so handing a
decoded sound from the registry to a voice would have copied it.

**Decision.**
- `se_core/pcm.h` holds the one PCM type (`se::Pcm`: interleaved f32
  samples, channels, rate, frames). `se::PcmPayload` (se_assets) and
  `se::audio::Pcm` (se_audio) are aliases of it, so the lane's payload, the
  AssetRegistry's `std::shared_ptr<const Pcm>`, the engine's sound table
  and each voice share one object; no copy is made between the lane and
  the mixer.
- se_audio adds `PcmDecoder`: miniaudio's ma_decoder over the file's
  bytes (it owns them), decoding to the engine's rate and channel count in
  calls of at most N frames. It touches no engine state.
- se_runtime's `LaneSoundDecoder` (se_runtime/sound_assets.h) implements
  se_assets' SoundDecoder over it: each lane step decodes 4096-frame chunks
  until 1 ms has passed (the lane's steps stay near M9's 2 ms), and counts
  the steps, the longest one and the role of the thread that ran the last
  one (kBackground on the lane, which the integration tests check).
- Boot: the sound table is read at boot step 9 with the texture table
  (`LoadSoundTable`; a missing or bad table is logged and the game runs
  silent, as M8 did); its files are every state's default sounds
  (`StateAssets::all_sounds`, like `all_textures`). At step 10 the App
  creates the audio engine, then the LaneSoundDecoder at the engine's
  format (48 kHz stereo when the engine did not start: the sounds still
  load and are dropped) and gives it to the registry before the start
  state is pushed. With `--audio none` the sound table is not read: no
  sound is requested, decoded or waited for, so a headless run without
  audio ticks from its first frame as before.
- `AssetRuntime::SetSoundListener`: at iterate start, after the registry
  drained the lane's completions, every sound whose PCM landed (version
  changed and resident) is handed to the listener, and a sound whose
  entry went away (its last scope released) is handed over as null. The
  App's listener calls `AudioEngine::SetSound(key, pcm)` (null removes it:
  Play then fails and is counted). So a state's sounds are in the engine
  before its first tick (the hold of ADR-081 covers them), and a hot
  reload gives new voices the new PCM while playing voices keep the old
  object until their ma_sound_uninit has returned.
- `AudioEngine::VoicePcm(voice)` returns a voice's PCM (tests).
- Shutdown: the engine goes at step 7, the lane at step 8 (a sound job in
  flight finishes its step or is cancelled), the scopes at step 10; the
  decoder outlives them all.

**Why.** One type is what makes "the lane decodes, the voices play it in
place" true without a copy; the adapter in se_runtime is the only module
that may see both se_assets and se_audio. Feeding the engine from the
registry's version counter, at the same point where textures become
resident, keeps the hold's guarantee (asset timing never changes a tick)
for sounds too.

## ADR-088 The helper threads' order at the integration; the shutdown trace (2026-11-09)

**Decision.**
- Boot: the log worker (step 2), the vblank sampler and the watchdog
  (step 9, `StartHelperThreads`), the background lane right after them
  (step 9, `AssetRuntime::Start`), then at the first iterate, after
  `WakeEventScope::Open`, the input sampler (ADR-075). The lane pushes no
  SDL event itself: it calls WakeMain, which pushes only inside ADR-047's
  window, so it may start at step 9.
- Shutdown: the input sampler before step 4 (it calls SDL_UpdateGamepads,
  which pushes events; ADR-047, ADR-075) and the wake scope closes at step
  4; the Worlds (step 6); the audio engine (step 7); the lane (step 8,
  Durable jobs finish); the vblank sampler (step 9); the AssetScopes (step
  10); then the renderer, the window, SDL, the log worker (16) and the
  watchdog (17), as before. The design's step 9 joins the input sampler
  there; ADR-047 moves it before step 4 and this record keeps it there.
- Every one of them is a named role started through se::StartThread, so
  the ThreadRegistry has a live slot for each (`bg-lane`, role
  kBackground; `input-sampler`, role kInputSampler), the dump streams carry
  their breadcrumbs, and `integration::EngineThreadsRunning()` counts them;
  `crash.rampage_*` now require the lane's slot and OS thread name in the
  dumps as well as the input sampler's.
- App::Teardown records each step it runs in a fixed array (no
  allocation), and Shutdown logs it after the teardown: "shutdown order:
  input-sampler, event-filter (4), worlds (6), audio (7), bg-lane (8),
  vblank-sampler (9), asset-scopes (10), log-worker (16), watchdog (17)"
  (a step that had nothing to stop is left out). `rampage.frames_300`
  checks it, and the run reports read the order from it.

**Why.** The order is a contract between four milestones' threads; logging
it once makes it checkable without hooks in every step.

## ADR-089 Replays go through the lane; the quit's wait is logged; the Durable-save quit test (2026-11-09)

**Decision.**
- `--record`'s replay is written through the background lane (M9: "replay
  and SessionLog flushes go through the lane as Background jobs") as a
  Background-class SaveRequest marked Durable: it is submitted at the
  start of the shutdown, after the last tick, and shutdown step 8 runs it
  to the end before joining the lane (a non-Durable flush would be
  cancelled there). The temp-file-then-rename and the sharing-violation
  retry of ADR-080 apply. The App logs the lane's Durable failures after
  the teardown.
- The two-phase quit logs its wait: LaneQuitProvider says "quit waits for
  N Durable lane job(s)" when it first answers kPending and "the lane's
  Durable jobs are done" when it answers kReady; the App logs "quit ready
  after X ms" (from the request to the exit).
- Test builds add `--durable-save-test FRAME:KB:MS`: at iterate FRAME the
  App sets the se::fs injector to MS ms per write step, submits a Durable
  save of KB KiB of JSON to `<pref>/durable_save_test.json` and requests
  the quit. `platform.rampage_durable_quit` (label desktop) runs rampage
  with it (4 MiB at 100 ms per 1 MiB step) and checks the order of the
  log lines, that the quit waited at least the throttled steps, and that
  the file is complete, parses and left no temporary file.

**Why.** A replay written by main at shutdown bypassed the lane the design
gives every write; a Durable save at shutdown is the one form the lane's
stop is guaranteed to finish. The rampage test proves the quit's wait in
the real executable, with its window, its helpers and its shutdown order.

## ADR-090 The input sampler idles at 20 Hz while no gamepad is open (2026-11-09)

**Context.** M8's sampler calls SDL_UpdateGamepads at 1000 Hz on AC power
and 250 Hz on battery (ADR-075). The integration's desktop run found
`pacing.vrr_deadline` (M6: the self-paced 120 Hz deadline's p99 at most
0.5 ms) failing about one run in two: p99 0.49 to 0.52 ms. Measured on
this machine with nothing else changed (no gamepad connected, CPU load
13-26%): with the sampler at 1000 Hz p99 0.31 to 0.52 ms; at 250 Hz 0.26
to 0.28 ms; at 20 Hz 0.27 to 0.31 ms; without the sampler 0.26 to 0.31
ms. The 1 kHz updates stamp nothing while no pad is open.

**Decision.** The App gives the sampler `SamplerRateFor(on_battery,
open_gamepads)`: `kSamplerHzIdle` (20 Hz) while the App has no gamepad
open, otherwise 1000 Hz on AC and 250 Hz on battery as before. SDL finds
new pads through the same update, so a pad plugged in is opened within
about 50 ms, and the next iterate raises the rate (logged: "input sampler:
1000 Hz (on AC power, 1 gamepads)"). A virtual joystick is reported at
once, so the virtual-gamepad tests are unchanged.

**Why.** The spec's rates exist to stamp pad edges about a millisecond
apart; with no pad open they only cost main's pacing precision. With a pad
open the cost returns: the real-gamepad row of docs/gates.md now also asks
for `pacing.vrr_deadline` with a pad connected.
