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
//                   follow the stack, and every sound whose PCM landed or
//                   changed goes to the sound listener (the audio engine;
//                   ADR-087); Hold() before the stepper plans: no
//                   ticks while the top state's scope is not resident
//                   (logged when it starts and ends), and the TickGate
//                   stops a frame's ticks when a tick pushes a state whose
//                   scope is not resident yet, so asset timing never
//                   changes what a tick computes
//   shutdown 8      StopLane: queued non-Durable jobs are cancelled, a
//                   running step finishes, Durable jobs run to the end
//   shutdown 10     ReleaseScopes: every scope, the UI scope too
//
// M13 (ADR-123): the AssetRuntime is the stack's StateContentGate and the
// states' ContentSource. A push or replace waits at the gate until its
// state's AssetScope, created the first time the gate is asked (textures,
// sounds, texts and the texts' dependencies, found as they land), is
// resident; Hold and HoldTicks hold while a state waits, so no tick runs
// before it is created. Text(path) serves the texts the scopes on the
// stack (and the waiting one) pinned when their state was created: a text
// changed on disk never changes under a running World; the next state that
// loads it gets the new version, reported to the text swap listener (the
// App logs a kAssetSwap entry). A text that fails to load is the state's
// error (StateError): the state is not created. LoadedTexts() is every
// version served, in order: the session's content bundle.
//
// Textures come from cooked pages (tools/asset_cook, CMake, into
// <executable dir>/cooked or --cooked-root): in dev builds a page whose
// source changed or is missing is re-cooked on the lane, and the 500 ms
// scan hot-reloads changed sources and pages; Release builds read pages
// only. A state's assets are GameSetup::state_assets(id), by default every
// texture of the game's table and every sound of its sound table. Sounds
// are decoded on the lane by the SoundDecoder the App gives the registry
// (se_runtime/sound_assets.h).

#ifndef SE_RUNTIME_ASSET_RUNTIME_H_
#define SE_RUNTIME_ASSET_RUNTIME_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/asset_registry.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_core/error.h"
#include "se_core/pcm.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_runtime/lane_quit.h"
#include "se_sim/content_bundle.h"
#include "se_sim/content_source.h"
#include "se_sim/game_state.h"
#include "se_sim/sim_driver.h"

namespace se {

// What a GameState's AssetScope loads before its Worlds step.
struct StateAssets {
  // Every texture of the table.
  bool all_textures = true;
  // Names of the table, when all_textures is false.
  std::vector<std::string> textures;
  // Every sound of the sound table (AssetRuntimeConfig::sounds), besides
  // |sounds|.
  // Files relative to the content root.
  std::vector<std::string> prefabs;
  std::vector<std::string> scene_chunks;
  std::vector<std::string> sounds;
  bool all_sounds = true;
  // Files loaded as text through the lane before the state is created,
  // relative to the content root, and every file text_dependencies names
  // for them.
  std::vector<std::string> texts;
};

struct AssetRuntimeConfig {
  std::string content_root;
  // Where tools/asset_cook wrote the pages.
  std::string cooked_root;
  // The texture table, paths as written (relative to the content root).
  std::vector<TextureTableEntry> textures;
  // The sound table's files, relative to the content root (ADR-087).
  std::vector<std::string> sounds;
  AtlasRequest::Mode page_mode = AtlasRequest::Mode::kPageIfFresh;
  bool hot_reload = true;
  // Step trace records kept (--lane-step-trace).
  std::uint32_t trace_capacity = 0;
  // By state id; empty: every state loads every texture.
  std::function<StateAssets(std::string_view state_id)> state_assets;
  // The files a loaded text needs too (GameSetup::text_dependencies).
  std::function<std::vector<std::string>(std::string_view path,
                                         std::string_view text)>
      text_dependencies;
};

class AssetRuntime final : public TickGate,
                           public StateContentGate,
                           public ContentSource {
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
  // TickGate: a tick about to run on a state whose scope is not resident,
  // or while a state waits at the content gate.
  bool HoldTicks(const GameStateStack& states, Tick next_tick) override
      SE_REQUIRES(g_sim_owner);
  // StateContentGate (M13): creates |state_id|'s scope the first time it is
  // asked; true once every asset and text of it is resident or failed and
  // no text failed. Main thread (inline: the owner).
  [[nodiscard]] bool Ready(std::string_view state_id) override
      SE_REQUIRES(g_sim_owner);
  // ContentSource (M13): a text a state on the stack, or the one being
  // created, loaded; |path| is content-relative.
  [[nodiscard]] std::expected<std::string_view, Error> Text(
      std::string_view path) const override;
  // A state whose text did not load (it is never created): the App takes
  // the fault path. Null while none.
  [[nodiscard]] const Error* StateError() const noexcept {
    return state_error_ ? &*state_error_ : nullptr;
  }
  // The state StateError() is about.
  [[nodiscard]] std::string_view StateErrorState() const noexcept {
    return state_error_state_;
  }
  // Forgets the state error once its owner handled it (the editor's 'Play
  // game' faults its session and goes on; M13 integration).
  void ClearStateError() noexcept {
    state_error_.reset();
    state_error_state_.clear();
  }
  // Every text version served to a state so far, in the order first served
  // (the session's content bundle; its start state is not set).
  [[nodiscard]] const ContentBundle& LoadedTexts() const noexcept {
    return loaded_texts_;
  }
  // Called when a state is created with a version of a text that differs
  // from the one the path served before (path content-relative, the
  // version's ContentTextHash); the App logs a kAssetSwap entry.
  using TextSwapListener =
      std::function<void(std::string_view path, std::uint64_t hash)>;
  void SetTextSwapListener(TextSwapListener listener) {
    text_swap_listener_ = std::move(listener);
  }
  // Before the frame's render: the scopes follow the stack (a tick may
  // have pushed or popped a state; a new state's assets are requested at
  // once).
  void BeforeRender(const GameStateStack& states) SE_REQUIRES(g_main);
  // False for the Worlds of a state whose scope is not resident (they are
  // not drawn: their textures are not there yet). Valid after
  // BeforeRender, for the stack it saw.
  [[nodiscard]] bool Drawable(const World* world) const;

  // Called at iterate start (BeginFrame) for every sound whose PCM landed
  // or was reloaded, with the file's path (joined to the content root),
  // and with null once the sound's last scope released it (ADR-087).
  using SoundListener =
      std::function<void(std::string_view path, std::shared_ptr<const Pcm>)>;
  void SetSoundListener(SoundListener listener) {
    sound_listener_ = std::move(listener);
  }

  // M10a: requests into |scope| what a state with the default StateAssets
  // loads (every texture and sound of the tables), so an editor scope
  // keeps them while its sessions' stacks come and go.
  void RetainDefaults(AssetScope scope) SE_REQUIRES(g_main);
  // M10a: every lane completion the registry did not submit (saves,
  // flushes) is also handed to |listener| on main, after it is logged.
  using ForeignListener = std::function<void(const LaneCompletion&)>;
  void SetForeignListener(ForeignListener listener) {
    foreign_listener_ = std::move(listener);
  }

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
  [[nodiscard]] LaneQuitProvider& LaneQuit() noexcept { return *quit_; }
  // The scope of the state on the stack with |id| (null when none).
  [[nodiscard]] AssetScope ScopeOf(std::string_view id) const;
  // Frames the top state was held for so far.
  [[nodiscard]] std::uint64_t HeldFrames() const noexcept {
    return held_frames_;
  }

 private:
  // A text a state's scope loads (M13).
  struct ScopeText {
    // Content-relative, normalised.
    std::string path;
    Handle<TextAsset> handle;
    // The version the state was created with.
    std::shared_ptr<const std::string> pinned;
    bool expanded = false;
  };

  struct StateScope {
    const GameState* state = nullptr;
    std::string id;
    AssetScope scope;
    std::uint64_t created_ns = 0;
    bool resident_logged = false;
    // M13: made by Ready() before its state exists (state is null until
    // SyncScopes adopts it); ready once Ready() said yes.
    bool pending = false;
    bool ready = false;
    std::vector<ScopeText> texts;
  };

  // A sound a scope requested, and the version the listener saw.
  struct TrackedSound {
    std::string path;
    Handle<SoundAsset> handle;
    std::uint32_t version = 0;
  };

  explicit AssetRuntime(AssetRuntimeConfig config);
  void RequestSound(const StateScope& entry, const std::string& path)
      SE_REQUIRES(g_main);
  void NotifySounds() SE_REQUIRES(g_main);
  void SyncScopes(const GameStateStack& states) SE_REQUIRES(g_main);
  void RequestStateAssets(StateScope& entry) SE_REQUIRES(g_main);
  void RequestText(StateScope& entry, std::string_view path)
      SE_REQUIRES(g_main);
  // Requests the dependencies of every text of |entry| that landed; false
  // when a text failed (state_error_ is set).
  bool ExpandTexts(StateScope& entry) SE_REQUIRES(g_main);
  void PinTexts(StateScope& entry) SE_REQUIRES(g_main);
  [[nodiscard]] std::string ContentKey(std::string_view path) const;
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
  std::vector<TrackedSound> sounds_;
  SoundListener sound_listener_;
  ForeignListener foreign_listener_;
  bool holding_ = false;
  std::uint64_t held_frames_ = 0;
  // M13.
  ContentBundle loaded_texts_;
  TextSwapListener text_swap_listener_;
  std::optional<Error> state_error_;
  std::string state_error_state_;
  bool waiting_logged_ = false;
};

}  // namespace se

#endif  // SE_RUNTIME_ASSET_RUNTIME_H_
