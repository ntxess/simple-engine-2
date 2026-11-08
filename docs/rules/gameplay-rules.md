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
