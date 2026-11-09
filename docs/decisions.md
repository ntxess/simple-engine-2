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
