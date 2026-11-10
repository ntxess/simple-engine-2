// Copyright 2026 The simple-engine-2 Authors.
//
// A document World for the editor's tests: rampage's registry, the sandbox
// scene loaded under a kLoad gate into a kEditDocument World (whose gate is
// closed otherwise), and commands applied under a kEditorCommand gate.
// Construct it inside a MainThreadScope and an InlineOwnerScope.

#ifndef TESTS_EDITOR_SUPPORT_DOCUMENT_WORLD_H_
#define TESTS_EDITOR_SUPPORT_DOCUMENT_WORLD_H_

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "game_rampage/module.h"
#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_sim/game_module.h"

namespace se::testing {

// The sandbox scene's player and its first two enemies.
inline constexpr Guid kPlayer{0x5a4db0c500004000ULL, 0x8000000000000000ULL};
inline constexpr Guid kEnemy1{0x5a4db0c500004000ULL, 0x8000000000000001ULL};
inline constexpr Guid kEnemy2{0x5a4db0c500004000ULL, 0x8000000000000002ULL};

[[nodiscard]] inline std::string SandboxScenePath() {
  return std::string(SE_SOURCE_DIR) + "/assets/scenes/sandbox.scene.json";
}

[[nodiscard]] inline rampage::SceneRequest SandboxRequest() {
  rampage::SceneRequest request;
  request.format = rampage::SceneRequest::Format::kScene;
  request.path = SandboxScenePath();
  request.asset_root = std::string(SE_SOURCE_DIR) + "/assets";
  request.legacy_root = SE_SOURCE_DIR;
  return request;
}

class DocumentWorld {
 public:
  DocumentWorld() SE_REQUIRES(g_sim_owner)
      : module_(rampage::RampageModule()),
        world_(std::make_unique<World>(
            module_.Registry(),
            WorldDesc{.name = "document", .kind = WorldKind::kEditDocument})) {
    const MutationGate gate = world_->OpenGate(GateReason::kLoad);
    const auto loaded = rampage::LoadRequestedScene(*world_, SandboxRequest());
    REQUIRE(loaded.has_value());
  }

  [[nodiscard]] World& Get() noexcept { return *world_; }
  [[nodiscard]] const ComponentRegistry& Registry() const noexcept {
    return module_.Registry();
  }
  [[nodiscard]] std::uint64_t Hash() const { return world_->Hash().Folded(); }

  [[nodiscard]] std::expected<void, Error> Apply(editor::EditorCommand& command)
      SE_REQUIRES(g_sim_owner) {
    const MutationGate gate = world_->OpenGate(GateReason::kEditorCommand);
    return command.Apply(*world_);
  }
  [[nodiscard]] std::expected<void, Error> Revert(
      editor::EditorCommand& command) SE_REQUIRES(g_sim_owner) {
    const MutationGate gate = world_->OpenGate(GateReason::kEditorCommand);
    return command.Revert(*world_);
  }

  [[nodiscard]] bool Has(Guid guid, std::string_view component) const {
    const std::optional<entt::entity> entity = world_->Find(guid);
    const ComponentInfo* info = module_.Registry().FindByName(component);
    if (!entity || info == nullptr) return false;
    const entt::sparse_set* storage = world_->CReg().storage(info->id);
    return storage != nullptr && storage->contains(*entity);
  }

  // The component's JSON, or empty.
  [[nodiscard]] std::string ComponentJson(Guid guid,
                                          std::string_view component) const {
    if (!Has(guid, component)) return {};
    const ComponentInfo* info = module_.Registry().FindByName(component);
    auto json = info->write_json(world_->CReg(), *world_->Find(guid));
    return json ? *json : std::string();
  }

 private:
  ModuleInstance module_;
  std::unique_ptr<World> world_;
};

}  // namespace se::testing

#endif  // TESTS_EDITOR_SUPPORT_DOCUMENT_WORLD_H_
