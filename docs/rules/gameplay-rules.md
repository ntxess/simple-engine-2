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
`ticks(ms) = ceil(ms × 120 / 1000)` ticks. "Contact" is one overlapping
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
