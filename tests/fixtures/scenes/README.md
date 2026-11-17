# Scene fixtures (M11a)

se-scene-v1 scenes for `rampage.scene.*` (tests/rampage/prefab_test.cc).
Each covers one form of prefab instance against assets/prefabs (the asset
root is assets/):

| File | Covers |
|---|---|
| removed_component.scene.json | instances whose overrides remove prefab components (`null`) |
| added_component.scene.json | instances adding components the prefab lacks |
| nested_member.scene.json | overrides of nested members (a sprite region, a collider extent, a placed Transform) |
| stats_by_name.scene.json | stats added, changed and removed by name |
| no_prefab.scene.json | entities without a prefab, beside an instance |
| camera.scene.json | a Camera2D entity |
| sprite_tint.scene.json | SpriteRenderer tints, on an instance and on a plain entity |

`flatten/` holds what M3's SaveScene wrote for each of them and for
assets/scenes/sandbox.scene.json, produced once by the M3 writer before the
prefab mode existed. `rampage.scene.Flatten mode writes what M3's writer
wrote` compares kFlatten's output with these bytes (line ends aside), so
they are never regenerated: a difference is a change of the flattened
form, which the Play-start bytes, fault files and dumps depend on.
