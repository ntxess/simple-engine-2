// Copyright 2026 The simple-engine-2 Authors.
//
// The App's assets (docs/milestone-specs.md M9): the background lane, the
// AssetRegistry with GpuResources behind its upload port, an AssetScope
// per GameState on the stack, and the hold that keeps a state's Worlds
// from stepping (and from being drawn) until its scope is resident. Main
// thread only.
//
//   boot step 9     Start: the lane thread (role kBackground) with the
//                   other helpers; the registry
//   boot step 10    the start state is pushed: its scope requests its
//                   assets (Urgent); the SimDriver's TickGate is this
//   each iterate    BeginFrame at iterate start: the completion ring is
//                   drained (pages to the upload queue, the 8 MB budget is
//                   GpuResources'), changed files start reloads, scopes
//                   follow the stack; Hold() before the stepper plans: no
//                   ticks while the top state's scope is not resident
//                   (logged when it starts and ends), and the TickGate
//                   stops a frame's ticks when a tick pushes a state whose
//                   scope is not resident yet, so asset timing never
//                   changes what a tick computes
//   shutdown 8      StopLane: queued non-Durable jobs are cancelled, a
//                   running step finishes, Durable jobs run to the end
//   shutdown 10     ReleaseScopes: every scope, the UI scope too
//
// Textures come from cooked pages (tools/asset_cook, CMake, into
// <executable dir>/cooked or --cooked-root): in dev builds a page whose
// source changed or is missing is re-cooked on the lane, and the 500 ms
// scan hot-reloads changed sources and pages; Release builds read pages
// only. A state's assets are GameSetup::state_assets(id), by default every
// texture of the game's table.

#ifndef SE_RUNTIME_ASSET_RUNTIME_H_
#define SE_RUNTIME_ASSET_RUNTIME_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/asset_registry.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_runtime/lane_quit.h"
#include "se_sim/game_state.h"
#include "se_sim/sim_driver.h"

namespace se {

// What a GameState's AssetScope loads before its Worlds step.
struct StateAssets {
  // Every texture of the table.
  bool all_textures = true;
  // Names of the table, when all_textures is false.
  std::vector<std::string> textures;
  // Files relative to the content root.
  std::vector<std::string> prefabs;
  std::vector<std::string> scene_chunks;
  std::vector<std::string> sounds;
};

struct AssetRuntimeConfig {
  std::string content_root;
  // Where tools/asset_cook wrote the pages.
  std::string cooked_root;
  // The texture table, paths as written (relative to the content root).
  std::vector<TextureTableEntry> textures;
  AtlasRequest::Mode page_mode = AtlasRequest::Mode::kPageIfFresh;
  bool hot_reload = true;
  // Step trace records kept (--lane-step-trace).
  std::uint32_t trace_capacity = 0;
  // By state id; empty: every state loads every texture.
  std::function<StateAssets(std::string_view state_id)> state_assets;
};

class AssetRuntime final : public TickGate {
 public:
  // |renderer| may be null (headless): textures settle without the GPU.
  [[nodiscard]] static std::expected<std::unique_ptr<AssetRuntime>, Error>
  Start(const AssetRuntimeConfig& config, Renderer2D* renderer)
      SE_REQUIRES(g_main);

  AssetRuntime(const AssetRuntime&) = delete;
  AssetRuntime& operator=(const AssetRuntime&) = delete;
  // Stops the lane; the scopes are the owner's to release first
  // (ReleaseScopes, shutdown step 10).
  ~AssetRuntime() override;

  // Iterate start.
  void BeginFrame(const GameStateStack& states) SE_REQUIRES(g_main);
  // True while the top state's scope is not resident: plan no ticks.
  [[nodiscard]] bool Hold(const GameStateStack& states) SE_REQUIRES(g_main);
  // TickGate: a tick about to run on a state whose scope is not resident.
  bool HoldTicks(const GameStateStack& states, Tick next_tick) override
      SE_REQUIRES(g_sim_owner);
  // Before the frame's render: the scopes follow the stack (a tick may
  // have pushed or popped a state; a new state's assets are requested at
  // once).
  void BeforeRender(const GameStateStack& states) SE_REQUIRES(g_main);
  // False for the Worlds of a state whose scope is not resident (they are
  // not drawn: their textures are not there yet). Valid after
  // BeforeRender, for the stack it saw.
  [[nodiscard]] bool Drawable(const World* world) const;

  // --reload-texture: re-cooks |name| (from |file| instead when given) on
  // the lane. False when the table has no such texture.
  bool ReloadTexture(std::string_view name, std::string_view file)
      SE_REQUIRES(g_main);

  // Shutdown step 8. Logs nothing (the teardown is noexcept): the App
  // logs LogLaneStopped() afterwards.
  void StopLane() noexcept;
  // The lane's totals, for the shutdown log.
  static void LogLaneStopped(const LaneStats& stats);
  // Shutdown step 10.
  void ReleaseScopes() SE_REQUIRES(g_main);

  [[nodiscard]] AssetRegistry& Registry() noexcept { return *registry_; }
  [[nodiscard]] BackgroundLane& Lane() noexcept { return *lane_; }
  [[nodiscard]] QuitProvider& QuitWaiter() noexcept { return *quit_; }
  // The scope of the state on the stack with |id| (null when none).
  [[nodiscard]] AssetScope ScopeOf(std::string_view id) const;
  // Frames the top state was held for so far.
  [[nodiscard]] std::uint64_t HeldFrames() const noexcept {
    return held_frames_;
  }

 private:
  struct StateScope {
    const GameState* state = nullptr;
    std::string id;
    AssetScope scope;
    std::uint64_t created_ns = 0;
    bool resident_logged = false;
  };

  explicit AssetRuntime(AssetRuntimeConfig config);
  void SyncScopes(const GameStateStack& states) SE_REQUIRES(g_main);
  void RequestStateAssets(StateScope& entry) SE_REQUIRES(g_main);
  [[nodiscard]] TextureSource SourceOf(const TextureTableEntry& entry) const;
  bool TopResident(const GameStateStack& states, std::string_view reason)
      SE_REQUIRES(g_main);
  void OnForeign(LaneCompletion&& completion);

  AssetRuntimeConfig config_;
  std::unique_ptr<TextureUploadPort> port_;
  std::unique_ptr<BackgroundLane> lane_;
  std::unique_ptr<AssetRegistry> registry_;
  std::unique_ptr<LaneQuitProvider> quit_;
  std::vector<StateScope> scopes_;
  bool holding_ = false;
  std::uint64_t held_frames_ = 0;
};

}  // namespace se

#endif  // SE_RUNTIME_ASSET_RUNTIME_H_
