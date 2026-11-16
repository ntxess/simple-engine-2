// Copyright 2026 The simple-engine-2 Authors.

#include "src/play_states.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_sim/game_state.h"

namespace se::editor {
namespace {

// The hand-over slots: written on main right before a push, read by the
// creator during it.
struct PendingStates {
  std::unique_ptr<World> play_world;
  const bool* play_running = nullptr;
  World* document = nullptr;
};

PendingStates& Pending() {
  static auto* const kPending = new PendingStates();
  return *kPending;
}

// A Play session's World: owned, stepping while its flag says so.
class PlayWorldState final : public GameState {
 public:
  PlayWorldState(std::unique_ptr<World> world, const bool* running)
      : world_(std::move(world)), running_(running) {
    worlds_[0] = world_.get();
  }

  void OnEnter() SE_REQUIRES(g_sim_owner) override {}
  void OnExit() SE_REQUIRES(g_sim_owner) override {
    worlds_[0] = nullptr;
    world_.reset();
  }
  [[nodiscard]] std::span<World* const> Worlds() const override {
    return world_ ? std::span<World* const>(worlds_)
                  : std::span<World* const>();
  }
  [[nodiscard]] bool SimRunning() const override {
    return running_ == nullptr || *running_;
  }

 private:
  std::unique_ptr<World> world_;
  const bool* running_;
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakePlayWorldState(const StateEnv& /*env*/) {
  AssertMainThread();
  PendingStates& pending = Pending();
  SE_ASSERT_MSG(pending.play_world != nullptr,
                "a play-World state without a pending World");
  auto state = std::make_unique<PlayWorldState>(std::move(pending.play_world),
                                                pending.play_running);
  pending.play_running = nullptr;
  return state;
}

// The document: shown, never stepped.
class DocumentState final : public GameState {
 public:
  explicit DocumentState(World* world) { worlds_[0] = world; }

  void OnEnter() SE_REQUIRES(g_sim_owner) override {}
  [[nodiscard]] std::span<World* const> Worlds() const override {
    return worlds_[0] != nullptr ? std::span<World* const>(worlds_)
                                 : std::span<World* const>();
  }
  [[nodiscard]] bool SimRunning() const override { return false; }

 private:
  std::array<World*, 1> worlds_{};
};

std::unique_ptr<GameState> MakeDocumentState(const StateEnv& /*env*/) {
  AssertMainThread();
  World* world = std::exchange(Pending().document, nullptr);
  return std::make_unique<DocumentState>(world);
}

}  // namespace

void RegisterPlayWorldState(GameStateFactory& factory, std::string_view id) {
  factory.Register(id, &MakePlayWorldState);
}

void SetPendingPlayWorld(std::unique_ptr<World> world, const bool* running) {
  AssertMainThread();
  Pending().play_world = std::move(world);
  Pending().play_running = running;
}

void RegisterDocumentState(GameStateFactory& factory) {
  factory.Register(kDocumentState, &MakeDocumentState);
}

void SetPendingDocumentWorld(World* world) {
  AssertMainThread();
  Pending().document = world;
}

}  // namespace se::editor
