# Legacy reference scenarios

Recorded behaviour of the legacy Sandbox (core/, app/), the reference the new
engine's gameplay port is compared against (docs/milestone-specs.md, M1;
docs/implementation-plan.md, M3). Each scenario runs 3600 ticks at 120 Hz.

| Scenario | Content |
|---|---|
| `stress` | 16 enemies from the editor's stress generator (seed 1, first press); the player sweeps the arena |
| `effects` | Hand-written row of test entities: effect timing, watchdog, cooldowns, stationary receiver, friendly source, death, rotated collider, zero-length path segment, arena edge, and two kinds of source changing one stat by non-integer amounts in the same ticks (application order) |
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
  HP and the other stats, `%.9g`), generated at `legacy-ref-v1`, after rules
  R1 to R10. This is what M3's legacy comparison reads. Debug and Release
  builds produce identical projections.

Run one by hand from the repository root:

```sh
out/build/legacy-msvc-release/app/legacy_replay --scenario tests/fixtures/reference/stress.scenario.json \
    --inputs tests/fixtures/reference/stress.seinput --projection-out stress.seproj
```

Re-baseline after a declared rule: `SE_UPDATE_GOLDENS=1 ctest --preset legacy-msvc-debug -R legacy.reference`
(rewrites both goldens; an unchanged projection keeps its file).
