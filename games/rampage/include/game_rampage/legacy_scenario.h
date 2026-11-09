// Copyright 2026 The simple-engine-2 Authors.
//
// The legacy reference's scenario files (se-legacy-scenario-v1, the format
// of app/headless/ScenarioLoader.hpp), loaded into the same World shape as
// a scene: the player of the legacy Sandbox::init with the scenario's
// overrides, then the scenario's entities in file order, so spawn serials
// equal the legacy SpawnIndex values.
//
// Colliders take the size of the texture named in config/texture.json, read
// from the PNG IHDR chunk (no image decoder before M7). Guids derive from
// the scenario name and the spawn index. Numbers are read as doubles and
// rounded to float, as the legacy loader does with rapidjson.

#ifndef GAME_RAMPAGE_LEGACY_SCENARIO_H_
#define GAME_RAMPAGE_LEGACY_SCENARIO_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "game_rampage/components.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"

namespace se::rampage {

inline constexpr std::string_view kLegacyScenarioFormat =
    "se-legacy-scenario-v1";

struct LegacyScenarioOptions {
  // The repository root: config/config.json, config/texture.json and the
  // textures are read relative to it, as the legacy tree reads them from
  // its working directory.
  std::string root;
  RulesVersion rules = RulesVersion::kV2;
};

struct LegacyScenarioSummary {
  std::string name;
  // Entities created, the player included.
  std::size_t entities = 0;
};

std::expected<LegacyScenarioSummary, Error> LoadLegacyScenario(
    World& world, const std::string& path, const LegacyScenarioOptions& options)
    SE_REQUIRES(g_sim_owner);

struct PngSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// The size in a PNG file's IHDR chunk (its first 24 bytes).
[[nodiscard]] std::expected<PngSize, Error> ReadPngSize(
    const std::string& path);

// The Guid of the entity with |spawn_index| in the scenario |name|.
[[nodiscard]] Guid LegacyGuid(std::string_view name,
                              std::uint32_t spawn_index) noexcept;

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_LEGACY_SCENARIO_H_
