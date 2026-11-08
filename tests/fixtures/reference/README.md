# Legacy reference scenarios

Recorded behaviour of the legacy Sandbox (core/, app/), the reference the new
engine's gameplay port is compared against (docs/milestone-specs.md, M1;
docs/implementation-plan.md, M3). Each scenario runs 3600 ticks at 120 Hz.

| Scenario | Content |
|---|---|
| `stress` | 16 enemies from the editor's stress generator (seed 1, first press); the player sweeps the arena |
| `effects` | Hand-written row of test entities: effect timing, watchdog, cooldowns (including a polled turret that is not ready on start: the player overlaps it from tick 613 and is first hit at tick 661), stationary receiver, friendly source, death, rotated collider, zero-length path segment, arena edge, two kinds of source changing one stat by non-integer amounts in the same ticks (application order), and, out of every source's reach (SpawnIndex 13 to 15), a path that pushes its sprite into the bottom-left corner and ends beyond it, a repeating path with segments shorter than one tick's step whose last point is its first, and a repeating path whose points all coincide |
| `patrol` | 20 enemies from the stress generator (seed 2, first press); diagonal moves, pauses and reversals |

Files per scenario `NAME`:

- `NAME.scenario.json`: entities and player overrides (format in
  app/headless/ScenarioLoader.hpp). Generated scenarios were written by
  `legacy_replay --seed S --scenario BASE.json --generate N --scenario-out NAME.scenario.json`.
- `NAME.seinput`: the scripted input, sparse `tick held pressed released` lines
  in hex (core/util/LegacyInput.hpp), written by `tests/legacy/make_inputs.py`.
- `NAME.sehash`: the golden per-tick projection hashes (FNV-1a 64 of each
  tick's projection block, core/util/SemanticProjection.hpp) and the run hash.
  Every declared rule (docs/rules/gameplay-rules.md) re-baselined them.
- `NAME.seproj.gz`: the golden projection itself, gzip of the `.seproj` text
  (ticks 0 to 3600; per tick and entity, in SpawnIndex order: x, y, rotation,
  HP and the other stats, `%.9g`), first generated at `legacy-ref-v1` (after
  rules R1 to R10) and re-baselined by every rule since. M3's legacy
  comparison reads the state at `legacy-ref-v2`, after rules R1 to R13. Debug
  and Release builds produce identical projections.

Run one by hand from the repository root:

```sh
out/build/legacy-msvc-release/app/legacy_replay --scenario tests/fixtures/reference/stress.scenario.json \
    --inputs tests/fixtures/reference/stress.seinput --projection-out stress.seproj
```

Re-baseline after a declared rule: `SE_UPDATE_GOLDENS=1 ctest --preset legacy-msvc-debug -R legacy.reference`
(rewrites the goldens instead of checking them and prints a warning; an
unchanged golden keeps its file). `scripts/ci.ps1` clears `SE_UPDATE_GOLDENS`,
so the commit gate always checks.

## v1/

`v1/NAME.scenario.json`, `v1/NAME.seinput` and `v1/NAME.seproj.gz` are the
files of `legacy-ref-v1` (rules R1 to R10, before M3a's R11 to R13), kept
for the engine's comparison `ref.sandbox_v1.NAME` (tests/rampage,
`se_replay --legacy-rules v1`, docs/decisions.md ADR-025). They never
change; the files above move on with each declared rule.
