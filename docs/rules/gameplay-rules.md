# Gameplay rules

Declared rules applied to the legacy reference (core/, app/) after
`legacy-ref-v0`, one commit each (docs/milestone-specs.md, M1b). Each rule
states the behaviour the new engine's gameplay (M3, game_rampage) implements,
what the legacy code did before, and which golden projection hashes of the
reference scenarios (tests/fixtures/reference/) it re-baselined. The reference
at `legacy-ref-v1` follows all of them, and M3 compares the two trees through
the projections of that state.

Conventions: time is counted in ticks of the 120 Hz simulation clock
(LegacySimTime). An authored duration of `ms` milliseconds is
`ticks(ms) = ceil(ms Ã— 120 / 1000)` ticks. "Contact" is one overlapping
(source, receiver) pair in one tick; a source is an entity with a Sprite,
TeamTag, EffectsList and UpdateEntityEvent or UpdateEntityPolling, a receiver
any other entity with a Sprite, TeamTag and EntityStatus.

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
affected that receiver in this tick and either more than `ticks(cooldown)`
ticks have passed since it last affected that receiver, or it has never
affected it and `readyOnStart` is set, or more than `ticks(cooldown)` ticks
have passed since the source was created. The decision is made once per pair
and tick, and a positive decision records the contact. Self and same-team
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
sources before polled ones (core/system/CollisionSystem.cpp:52-71), and the
pending events in the order of the event registry's storage, which entt
iterates newest first and reorders when an event is destroyed
(core/system/EventSystem.cpp:20-21). A source with both flags was handled
twice in a tick. The order is invisible for whole-number effects but not for
float sums near the stat's precision.

Goldens: `effects` changed from tick 133 for the 95 ticks where its escort and
charger both add to the player's Charge (1e7 + 1.5 + 0.75 rounds differently
in the two orders); `stress` and `patrol` unchanged.
