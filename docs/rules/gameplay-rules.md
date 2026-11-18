# Gameplay rules

Declared rules applied to the legacy reference (core/, app/) after
`legacy-ref-v0`, one commit each (docs/milestone-specs.md: R1 to R10 in M1b,
R11 to R13 in M3a). Each rule states the behaviour the new engine's gameplay
(M3, game_rampage) implements, what the legacy code did before, and which
golden projection hashes of the reference scenarios (tests/fixtures/reference/)
it re-baselined. The reference at `legacy-ref-v1` follows R1 to R10, the one
at `legacy-ref-v2` all thirteen, and M3 compares the two trees through the
projections of `legacy-ref-v2`: at the M3 integration game_rampage's
projections of the three scenarios are byte-identical to these goldens over
all 3600 ticks (`ref.sandbox.*`), and to the legacy-ref-v1 goldens with
R11 to R13 turned off (`ref.sandbox_v1.*`). R14 (M13b) is declared for the engine's game
flow only: it replaces a legacy scene change the reference keeps.

Conventions: time is counted in ticks of the 120 Hz simulation clock
(LegacySimTime). An authored duration of `ms` milliseconds is
`ticks(ms) = ceil(ms × 120 / 1000)` ticks. "Contact" is one overlapping
(source, receiver) pair in one tick; a source is an entity with a Sprite,
TeamTag, EffectsList and UpdateEntityEvent (moved this tick) or
UpdateEntityPolling (polled), and a receiver any other entity with a
SpawnIndex, Sprite and TeamTag (effects need its EntityStatus). Goldens are the
per-tick projection hashes (`NAME.sehash`), re-baselined by every rule; the
full projections (`NAME.seproj.gz`) were added at `legacy-ref-v1`.

## R1 TEMPTIMED always reverts

A TEMPTIMED effect adds its value to the receiver's stat once, on the tick its
event is first processed (the contact tick), and subtracts it exactly once when
the event ends: when `ticks(duration)` ticks have passed since the application,
or when the event watchdog ends it earlier (more than the watchdog's ticks since
the application). A receiver destroyed before then drops the event without a
revert.

Before: the watchdog path returned COMPLETE without the revert, so the
modification stayed (core/system/EventSystem.cpp:138-139; line numbers in this
file are those of the original code, as in docs/milestone-specs.md).

Goldens: `effects` changed from tick 302 (its TEMPTIMED of 40 s outlives the
2 s watchdog it sets); `stress` and `patrol` unchanged.

## R2 Cooldowns are per (source, receiver) pair, decided once per tick

A polled source (UpdateEntityPolling) keeps its cooldown separately for every
receiver. In a tick it affects an overlapping receiver if it has not already
affected that receiver in this tick and either
- it affected that receiver before, more than `ticks(cooldown)` ticks ago, or
- it never affected that receiver, and `readyOnStart` is set or more than
  `ticks(cooldown)` ticks have passed since the source was created.

The decision is made once per pair and tick, and a positive decision records
the contact. Self and same-team
pairs are skipped before the decision and do not use it. Event-driven sources
(UpdateEntityEvent only) have no cooldown.

Before: one cooldown per source, restarted by whichever receiver was checked
first in the tick (UpdateEntityPolling::isReady, called per candidate from
core/system/CollisionSystem.cpp:88; core/component/UpdateEntityPolling.cpp:18-24),
so a source affected at most one receiver per cooldown and the quadtree's
order chose which; `readyOnStart` was ignored (UpdateEntityPolling.cpp:8-11),
so every source first waited one cooldown after creation.

Goldens: all three changed (`stress` from tick 534, `effects` from tick 14,
`patrol` from tick 1, where the player starts on an enemy).

Coverage: `readyOnStart` set shows in `patrol`, where the player starts on an
enemy and affects it at tick 1, before one cooldown has passed. Unset shows in
`effects` since the M1 review, which gave its turret (created at tick 0) a
cooldown of 5.5 s = 660 ticks that outlasts the first contact: the player
overlaps it from tick 613 and is first hit at tick 661; with the flag set the
hit would come at tick 613. That fixture change re-baselined `effects` from
tick 613 for 48 ticks (the player's HP).

## R3 Every overlap is found

Each tick, every source is tested against every candidate receiver, in
SpawnIndex order: all entities with a SpawnIndex, Sprite and TeamTag. A pair
overlaps when the two colliders intersect with positive area (touching edges do
not count).

Before: candidates came from QuadTree::queryRange (core/util/QuadTree.cpp:124),
which held only entities flagged as moved (UpdateEntityEvent) or polled
(UpdateEntityPolling) whose position lay inside the arena (QuadTree.cpp:12), and
visited only nodes whose bounds met the query range, so it missed stationary
receivers, entities outside the arena and colliders that reached into a node
from a neighbouring one. The quadtree is still rebuilt for the editor's
visualizer but no longer decides contacts.

Goldens: `effects` changed from tick 213 (the stationary receiver at x = 500 is
now hit), `patrol` from tick 1505 (111 ticks differ); `stress` unchanged.

## R4 The collider is the unrotated texture box at the sprite position

An entity's collider is the axis-aligned box of its texture rectangle's size,
centred on the sprite position. Rotation, scale and the sprite origin do not
change it.

Before: the collider was the sprite's global bounds
(core/system/CollisionSystem.cpp:73), the axis-aligned box around the rotated
and scaled sprite, so a path-following entity's collider grew by up to 41% per
side as its heading turned (67.9 px instead of 48 px at 45 degrees).

Goldens: all three changed, each for a short stretch of ticks around contacts
(`stress` from tick 438, 47 ticks; `effects` from tick 1303, 104 ticks;
`patrol` from tick 118, 59 ticks).

## R5 Seconds throughout

Rates are per second and systems receive the tick length in seconds
(1/120 s). The Speed stat that WayPointSystem follows is in px/s: the stress
generator's 0.5 px/ms becomes 500 px/s, and the scenario fixtures' Speed
values are multiplied by 1000. The player's movement command moves it at an
explicit 120 px/s (1 px per tick, as before). Durations of effects and
cooldowns stay authored in milliseconds and convert to ticks with `ticks(ms)`.

Before: Sandbox passed dt in milliseconds (the physics thread's 1000/frame-rate,
then 1000/120 since the single loop), Speed was in px/ms, and Movement moved
the player 1 px per input poll, a rate tied to the render loop
(core/command/Movement.cpp:12, app/editor/Editor.cpp:950).

Goldens: all three changed from tick 0, where the projection first prints the
Speed stat in its new unit (path-following positions are now speed × dt in
px/s × s, which may round differently in the last bit).

## R6 Zero-length path segments are skipped

A path segment whose two waypoints coincide (length 0) is skipped within the
same tick: the path advances to the next waypoint without moving or turning
the entity, and movement continues along the first segment of positive
length. A path that ends after skipped segments ends (or repeats) as usual.

Before: the direction of a segment was its vector divided by its length
(core/system/WayPointSystem.cpp:42-43), so a zero-length segment gave a NaN
direction and the entity's position and heading became NaN for good (it then
never collided).

Goldens: `effects` changed from tick 1 (its path with a duplicated first
waypoint now moves); `stress` and `patrol` unchanged.

## R7 OVERTIME applies one interval after contact, floor(duration / interval) times

An OVERTIME effect with interval `i` ms and duration `d` ms applies its value
`floor(d / i)` times: first exactly `ticks(i)` ticks after the contact, then
every `ticks(i)` ticks (the comparison is `>=` in ticks). It ends with its last
application, or earlier when the event watchdog ends it. An interval of 0
applies once per tick for `ticks(d)` ticks, starting one tick after the
contact; a duration shorter than the interval applies nothing.

Before: an application came on the first tick with *more* than `ticks(i)`
ticks since the previous one (`>`, core/system/EventSystem.cpp:98), and the
effect ended once the accumulated ticks reached `ticks(d)`, so a 250 ms
interval over 1100 ms applied 5 times, 31 ticks apart, instead of 4 times,
30 ticks apart; a 1000 ms interval over 5000 ms applied 5 times, 121 ticks
apart, instead of 120.

Goldens: all three changed (`stress` from tick 560, 75 ticks; `effects` from
tick 44; `patrol` from tick 121, 61 ticks).

## R8 Contacts are sorted before their effects apply

Each tick, the contacts are the distinct overlapping (source, receiver) pairs
over all sources (moved and polled). They are sorted by source SpawnIndex,
then receiver SpawnIndex, and handled in that order: the self, same-team and
cooldown (R2) checks, then one event per effect of the source's EffectsList, in
list order. Pending events are processed in the order they were created, so
effects of earlier ticks come first and, within a tick, effects follow the
sorted contacts.

Before: contacts were handled source by source in entt's view order, moved
sources before polled ones (core/system/CollisionSystem.cpp:53-67), and the
pending events in the order of the event registry's storage, which entt
iterates newest first and reorders when an event is destroyed
(core/system/EventSystem.cpp:20-21). A source with both flags was handled
twice in a tick. The order is invisible for whole-number effects but not for
float sums near the stat's precision.

Goldens: `effects` changed from tick 133 for the 95 ticks where its escort and
charger both add to the player's Charge (1e7 + 1.5 + 0.75 rounds differently
in the two orders); `stress` and `patrol` unchanged.

## R9 System order: Input, WayPoint, Boundary, Collision, Event, GameRules, Cleanup

Every tick runs, in this order: Input (the player's movement commands from the
tick's input frame), WayPoint (path following), Boundary (the clamp of every
sprite to the scene bounds, part of movement), Collision (contacts from the
positions after all movement), Event (pending effects, including the ones the
contacts just created), GameRules (HP <= 0) and Cleanup (destruction). So an
entity collides where it is at the end of the tick's movement, and a moved
entity's dirty flag is consumed in the tick that set it.

Before: Sandbox registered Collision, Event, WayPoint in that order
(app/scene/Sandbox.cpp:90-92), so contacts used the previous tick's path
positions and the current tick's player position, and the clamp ran last, at
the end of update(), after the HP check (since M1a; in render() before that).

Goldens: all three changed around contacts (`stress` from tick 439, 19 ticks;
`effects` from tick 13; `patrol` from tick 119, 14 ticks).

## R10 HP <= 0 destroys are deferred to the end of the tick

GameRules marks every entity whose HP stat is 0 or less after the tick's
effects (an EntityStatus without HP counts as 0) with a DestroyRequest.
Cleanup, the last step of the tick, destroys all requested entities in
SpawnIndex order. No system sees an entity destroyed in the middle of the
tick, and the destroy order (which decides entt's id recycling) is fixed.

Before: Sandbox::update destroyed each such entity on the spot while iterating
the EntityStatus view (app/scene/Sandbox.cpp:126-136).

Goldens: unchanged. Nothing ran after that loop within the tick, and
destroying the current entity of an entt view is safe, so the deferred point
produces the same projections; the rule fixes the single destroy point and
its order, which the engine's Cleanup phase (World::DrainDestroyRequests)
reproduces.

## R11 The clamp keeps the collider inside the scene bounds

Boundary (R9) keeps every sprite's collider, the unrotated texture box centred
on its position (R4), inside the scene bounds `W` × `H`, the configured
resolution (1920 × 1080). With the half extents `hw = width / 2` and
`hh = height / 2` of the sprite's texture rectangle, the new position is

    x' = W < 2·hw ? W / 2 : clamp(x, hw, W - hw)
    y' = H < 2·hh ? H / 2 : clamp(y, hh, H - hh)

evaluated in float as written (`W < 2.f * hw ? W * 0.5f :
std::clamp(x, hw, W - hw)`, `hw = texture width / 2.f`, `W` the bound as a
float). Both axes are computed from the position before the clamp and written
once. A sprite larger than the bounds on an axis is centred on that axis. The
clamp does not mark an entity as moved.

Before: the clamp (app/scene/Sandbox.cpp:202-218, Sandbox::checkBoundary;
core/system/BoundarySystem.cpp since R9) compared the centred position with 0
on the left and top, but the position plus the size of the rotated global
bounds with the bounds on the right and bottom, so half a sprite could leave on
the left and top while a whole sprite's margin was kept on the right and
bottom (48 px, or 67.9 px for a 48 px sprite turned 45 degrees). Each
correction's `setPosition` took the other axis from before the clamp, so when
both axes needed one the second undid the first, and a sprite pushed into a
corner kept leaving the arena along the first axis.

Goldens: all three changed from tick 1 on every tick (3600 of 3600):
`stress` (SpawnIndex 10 starts at x = 1902; it was kept at 1852.1, now at
1896), `effects` (SpawnIndex 9 on its path along the right edge at x = 1890,
which the old clamp moved to 1872) and `patrol` (SpawnIndex 9 starts at
y = 3, now 24). Path-following entities soon differ everywhere, because a
repeating path drifted from its points (R12, R13) until the clamp held it.

Coverage: `effects` SpawnIndex 13 is pushed diagonally into the bottom-left
corner by its path, corrected on both axes in the same ticks from tick 43, and
stops beyond the corner, held at (24, 1056); before, it left the arena to
x = -50.3 with y held at 1012.1. In `patrol` the player walks along the left
edge from tick 2877. No fixture has a sprite larger than the bounds (no
texture is larger than the arena); the engine's boundary_test covers it.

## R12 Path steps follow the polyline

A path's progress is its current segment `i`, from waypoint `i` to the next,
and the distance `u` travelled along it (a float). An entity starts on its
first point with `u = 0` on segment 0. Each tick, an entity with the Speed
stat (one without it does not move) takes the step `rem = speed × dt`
(`dt = 1.f / 120.f`, which is the float of the tick length 1/120 s) and loops:

1. a segment of length 0 is skipped (R6);
2. with no segment left (the end of a non-repeating path), the entity is placed
   exactly on the last point and stops moving for good;
3. if `u + rem < len`, then `u = u + rem` and the loop ends;
4. otherwise `rem = (u + rem) - len` and `u = 0`, and the loop goes on with the
   next segment.

So one tick's step can pass several waypoints. The position is taken from the
progress: `a + (b - a) × (u / len)` per axis, with the segment's points `a`
and `b` and its length `len = sqrt(dx × dx + dy × dy)` (`dx = b.x - a.x`,
`dy = b.y - a.y`; WayPoint::distanceToNext, computed once per segment). The
heading is the segment's, `atan2(dy, dx) × (180 / π) + 90` degrees, of the
last segment of positive length the entity moved along in the tick (so the
segment it ends the tick on, or the last one of a path it finished). All of
these are float expressions, evaluated as written and in this order. An
entity is moved (UpdateEntityEvent, the dirty flag of R2 and R8) on every tick
its position changes, so never after it finished its path. Boundary (R11)
clamps the sprite after this, without changing the progress, so a path that
leaves the arena does not shift the rest of the path. A step that a whole
round of a repeating path no longer shortens (a speed so large, or NaN or
infinite, that the round vanishes in float rounding) would loop for ever; it
ends at the start of the path instead. Until R13, a repeating path went on
from its end with segment 0, so the entity jumped back to its first point.

Before: MovementPattern kept the waypoint the entity was heading from and a
running total `distance` (core/system/WayPointSystem.cpp:37-51). Each tick the
whole step `speed × dt` was taken along the current segment's direction, even
past its end, and the next segment began only once `distance` exceeded the
next waypoint's running total (`distanceTotal`), so corners were overshot by
up to one step and at most one waypoint was passed per tick. The position was
integrated from these per-tick increments (`sprite.move`), so it drifted from
the path, and every clamp at the arena edge shifted the rest of the path; the
heading came from the step's vector, `atan2(velocity.y, velocity.x)`.

Goldens: all three changed from tick 1 on every tick (3600 of 3600; at tick 1
only in the last digits of the headings in `stress` and `patrol`, and in
`effects` SpawnIndex 14, which passes three waypoints in its first step).

Coverage: `effects` SpawnIndex 14 has segments of 1 px and 1.41 px, shorter
than its 4.17 px step, and passes three waypoints in tick 1 and again in tick
162, where its step crosses its last point (which is its first) and the next
two.
The non-repeating paths of `effects` end exactly on their last points:
SpawnIndex 10 at (400, 700) from tick 1393 and SpawnIndex 11 at (1840, 540)
from tick 1600, where the old code took one more step to x = 1841 and stayed
a moved source for that tick (the player's Charge differs from tick 1601).
SpawnIndex 13 ends beyond the arena corner and stays clamped there (R11).

## R13 Repeating paths close the loop

A repeating path with two or more distinct points has one more segment, the
closing segment from its last point back to its first, and after it starts
over at segment 0. The entity therefore cycles the closed polygon, and a
two-point path goes back and forth. The closing segment is a segment like any
other (R12): its length is `sqrt(dx × dx + dy × dy)` with `dx = first.x -
last.x` and `dy = first.y - last.y`, the heading along it is its own, and when
the last point is the first its length is 0 and it is skipped (R6). A path
whose points all coincide, a single point included, never moves.

Before: at the last waypoint `repeat` reset the progress (`currentPath` and
`distance`) but not the position, without moving in that tick
(core/system/WayPointSystem.cpp:27-34), so the path replayed from where the
entity stood, offset by the vector from the first point to the last, and
repeating paths wandered off until the clamp held them at the arena edge.
Since R12, which takes the position from the progress, the entity jumped back
from its last point to its first.

Goldens: all three changed from the first time a repeating path reached its
end: `stress` from tick 104 (3497 ticks), `effects` from tick 48 (3097 ticks;
SpawnIndex 6 turns back along its closing segment and skips its zero-length
first segment) and `patrol` from tick 94 (3479 ticks).

Coverage: two-point paths go back and forth in `effects` (SpawnIndex 2, 5 and
9) and in both generated scenarios; the other generated paths cycle their
polygons. `effects` SpawnIndex 14, whose last point is its first, has a
zero-length closing segment and is unchanged by this rule; SpawnIndex 15
(three coincident points) and the one-point paths in `stress` (SpawnIndex 11)
and `patrol` (SpawnIndex 10 and 17) never move.

## R14 Escape opens Pause; Pause's Jump returns to the Title

Declared for the engine only (M13b, game_rampage's FlowSystem); the legacy
reference is not changed and its goldens are not affected. A session starts
at the Title (the legacy MainMenu: its wallpaper, the "bg" texture over the
arena). In each state the first matching rule applies, at most one
transition per tick, and it takes effect at the next tick boundary (the
GameStateStack's deferred commands), so the tick in which a key is pressed
still runs in the old state and the next tick runs in the new one:

- Title: Enter replaces it with Gameplay; Exit (Escape) pops it, which ends
  the game (no state is left).
- Gameplay: when no PlayerControl entity is left alive (destroyed, or
  carrying a DestroyRequest, which R10's HP <= 0 rule adds in the same
  tick's GameRules), GameOver replaces it; otherwise Exit pushes Pause. The
  death wins over a pause pressed in the same tick.
- Pause (an overlay: its World steps, the paused Gameplay below it does not,
  and both draw): Exit pops it and Gameplay resumes; otherwise Jump (Space,
  the pad's south button) pops it and replaces Gameplay with the Title, two
  commands applied in order at one boundary, so the Gameplay never ticks
  again.
- GameOver: Enter replaces it with a fresh Gameplay (the scene loaded
  again); otherwise Exit replaces it with the Title.

A key counts in the tick whose InputFrame has its press; holding it does not
repeat it. The "sandbox" state (the editor's Play, se_replay, the goldens)
carries no flow role, and FlowSystem leaves its World alone.

Before: Escape in the Sandbox replaced it with the MainMenu, read from the
key events and only when the Sandbox was the root scene
(app/scene/Sandbox.cpp:97-100); the MainMenu had no way back, and nothing
paused or ended a game. Until M19's menus there is no text or UI: Jump in
the Pause stands for "return to the title" (decision 3 of the M11/M13/M17b
specs).

Goldens: none of the reference's change. The engine's flow has its own
golden, `determinism_rampage_flow` (tests/golden/*/), over a recorded
session through every state (tests/fixtures/flow/flow.seinput); every
earlier engine golden is unchanged.
