# simple-engine-2 documentation

The engine is being redesigned on the `redesign/engine-v2` branch. The new
targets grow beside the legacy SFML tree, which is kept as a deterministic
behaviour reference until every rule and editor operation is ported.

| Document | What it holds |
|---|---|
| [design/mt-design.md](design/mt-design.md) | The specification being implemented: one owner thread with fork-join workers and helper threads, the 25 required changes, API sketch, build order |
| [design/dedicated-driver.md](design/dedicated-driver.md) | The dedicated 120 Hz sim-thread driver, built behind `--sim-driver=threaded` |
| [design/costfree-end-state.md](design/costfree-end-state.md) | The end state the design builds on: modules, World, renderer, editor, assets |
| [design/platform-constraints.md](design/platform-constraints.md) | Threading rules of SDL3, Dear ImGui, EnTT, miniaudio and Windows |
| [design/prototype-measurements.md](design/prototype-measurements.md) | The measurement rig's results on the 7700X |
| [decisions.md](decisions.md) | Architecture decision records |
| [style.md](style.md) | Google C++ style, linting and the exceptions policy |
| [deps.md](deps.md) | Pinned dependencies, hashes and why each is used |
| [gates.md](gates.md) | Verification gates that need hardware or setup not on the dev machine |
| [ledger.md](ledger.md) | Completeness ledger: every legacy feature ported, re-specified or deleted |
| [implementation-plan.md](implementation-plan.md) | The approved milestone plan (M0-M19) |
| [milestone-specs.md](milestone-specs.md) | Detailed specs for M1, M2 and M3 |
| [rules/gameplay-rules.md](rules/gameplay-rules.md) | Declared gameplay rules R1-R13 applied to the legacy reference (milestones M1b and M3a) |
| [../tests/fixtures/reference/README.md](../tests/fixtures/reference/README.md) | The legacy reference scenarios, inputs and golden projections (legacy_replay, tests/legacy) |

## Building

Windows, from the repository root (needs Visual Studio 18 with MSVC 14.51):

```bat
scripts\with-msvc.cmd cmake --workflow --preset msvc-dev
```

Headless runs of a game module, with per-tick hashes, replays and the
determinism cross-check:

```bat
out\build\msvc-dev\bin\se_replay --module kernel_test --ticks 600 --fixed-frame-dt 16.6667 --hash-out hashes.txt
```

`scripts/ci.ps1` runs the commit gate (msvc-dev, msvc-asan, clangcl-dev,
including the `lint` and `checks` test labels); `scripts/ci.ps1 -Milestone`
adds Debug, Release and the WSL Linux sanitizer presets. The legacy tree
builds with the `legacy-msvc-debug` and `legacy-msvc-release` presets.
