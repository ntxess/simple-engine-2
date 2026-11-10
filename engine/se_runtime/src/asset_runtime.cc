// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/asset_runtime.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "se_assets/asset_registry.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_runtime/lane_quit.h"
#include "se_sim/game_state.h"

namespace se {
namespace {

// The registry's GPU side: M7's GpuResources (handles survive reloads and
// device loss; an upload is resident when its fence signals).
class GpuTexturePort final : public TextureUploadPort {
 public:
  explicit GpuTexturePort(Renderer2D& renderer) : renderer_(&renderer) {}

  TextureHandle Register(std::string_view name) override {
    AssertMainThread();
    return renderer_->Resources().AddTexture(name);
  }
  std::expected<void, Error> Upload(TextureHandle slot,
                                    AtlasPage page) override {
    AssertMainThread();
    auto payload = std::make_shared<TexturePayload>();
    payload->width = page.width;
    payload->height = page.height;
    payload->pixels = std::move(page.pixels);
    return renderer_->Resources().QueueUpload(slot, std::move(payload));
  }
  bool IsResident(TextureHandle slot) const override {
    return renderer_->Resources().IsResident(slot);
  }
  std::uint32_t ContentGeneration(TextureHandle slot) const override {
    return renderer_->Resources().ContentGeneration(slot);
  }
  void Remove(TextureHandle slot) override {
    AssertMainThread();
    renderer_->Resources().Remove(slot);
  }

 private:
  Renderer2D* renderer_;
};

constexpr double kNsToMs = 1e-6;

}  // namespace

AssetRuntime::AssetRuntime(AssetRuntimeConfig config)
    : config_(std::move(config)) {}

std::expected<std::unique_ptr<AssetRuntime>, Error> AssetRuntime::Start(
    const AssetRuntimeConfig& config, Renderer2D* renderer) {
  std::unique_ptr<AssetRuntime> runtime(new AssetRuntime(config));
  LaneConfig lane_config;
  lane_config.trace_capacity = config.trace_capacity;
  // The scan only matters with hot reload.
  if (!config.hot_reload) lane_config.scan_period_ns = 0;
  auto lane = BackgroundLane::Start(lane_config);
  if (!lane) return std::unexpected(std::move(lane.error()));
  runtime->lane_ = std::move(*lane);
  if (renderer != nullptr) {
    runtime->port_ = std::make_unique<GpuTexturePort>(*renderer);
  }
  AssetRegistryConfig registry_config;
  registry_config.page_mode = config.page_mode;
  registry_config.hot_reload = config.hot_reload;
  runtime->registry_ = std::make_unique<AssetRegistry>(
      *runtime->lane_, runtime->port_.get(), registry_config);
  AssetRuntime* raw = runtime.get();
  runtime->registry_->SetForeignHandler([raw](LaneCompletion&& completion) {
    raw->OnForeign(std::move(completion));
  });
  runtime->quit_ = std::make_unique<LaneQuitProvider>(*runtime->lane_);
  SE_LOG_INFO("background lane started: pages from {} ({}), hot reload {}",
              config.cooked_root,
              config.page_mode == AtlasRequest::Mode::kPage
                  ? "cooked pages only"
                  : "re-cooked on the lane when stale",
              config.hot_reload ? "on" : "off");
  return runtime;
}

AssetRuntime::~AssetRuntime() { StopLane(); }

void AssetRuntime::OnForeign(LaneCompletion&& completion) {
  const LaneCompletion done = std::move(completion);
  // Saves and flushes submitted through the lane by the App's users.
  if (!done.result) {
    const Error& error = done.result.error();
    SE_LOG_ERROR("lane job '{}' failed: {} ({})", done.kind, error.message,
                 error.path);
    return;
  }
  if (const auto* saved = std::get_if<SaveOutcome>(&*done.result)) {
    SE_LOG_DEBUG("saved {} (revision {}, {} bytes{})", saved->path,
                 saved->revision, saved->bytes,
                 saved->superseded ? ", superseded" : "");
  }
}

TextureSource AssetRuntime::SourceOf(const TextureTableEntry& entry) const {
  TextureSource source;
  source.name = entry.name;
  source.source_path = fs::Join(config_.content_root, entry.path);
  source.page_path = CookedPagePath(config_.cooked_root, entry.path);
  return source;
}

void AssetRuntime::RequestStateAssets(StateScope& entry) {
  StateAssets wanted;
  if (config_.state_assets) wanted = config_.state_assets(entry.id);
  for (const TextureTableEntry& texture : config_.textures) {
    const bool named = std::ranges::find(wanted.textures, texture.name) !=
                       wanted.textures.end();
    if (!wanted.all_textures && !named) continue;
    registry_->RequestTexture(entry.scope, SourceOf(texture));
  }
  for (const std::string& name : wanted.textures) {
    const bool known = std::ranges::any_of(
        config_.textures,
        [&name](const TextureTableEntry& t) { return t.name == name; });
    if (!known) {
      SE_LOG_ERROR("state '{}' needs texture '{}', which the table lacks",
                   entry.id, name);
    }
  }
  for (const std::string& path : wanted.prefabs) {
    registry_->RequestPrefab(entry.scope, fs::Join(config_.content_root, path));
  }
  for (const std::string& path : wanted.scene_chunks) {
    registry_->RequestSceneChunk(entry.scope,
                                 fs::Join(config_.content_root, path));
  }
  for (const std::string& path : wanted.sounds) {
    registry_->RequestSound(entry.scope, fs::Join(config_.content_root, path));
  }
}

void AssetRuntime::SyncScopes(const GameStateStack& states) {
  const std::vector<StackEntry> entries = states.Entries();
  // States that left the stack release their scopes.
  std::erase_if(scopes_, [&](const StateScope& scope) {
    const bool on_stack =
        std::ranges::any_of(entries, [&scope](const StackEntry& entry) {
          return entry.state == scope.state && entry.id == scope.id;
        });
    if (!on_stack) {
      AssertMainThread();
      registry_->ReleaseScope(scope.scope);
    }
    return !on_stack;
  });
  for (const StackEntry& entry : entries) {
    const bool known =
        std::ranges::any_of(scopes_, [&entry](const StateScope& scope) {
          return entry.state == scope.state && entry.id == scope.id;
        });
    if (known) continue;
    StateScope scope;
    scope.state = entry.state;
    scope.id = std::string(entry.id);
    scope.scope = registry_->CreateScope(entry.id);
    scope.created_ns = SDL_GetTicksNS();
    scopes_.push_back(std::move(scope));
    RequestStateAssets(scopes_.back());
  }
}

AssetScope AssetRuntime::ScopeOf(std::string_view id) const {
  for (const StateScope& scope : scopes_) {
    if (scope.id == id) return scope.scope;
  }
  return {};
}

void AssetRuntime::BeginFrame(const GameStateStack& states) {
  SE_ASSERT_MAIN_THREAD();
  SyncScopes(states);
  registry_->BeginFrame();
}

bool AssetRuntime::TopResident(const GameStateStack& states,
                               std::string_view reason) {
  SyncScopes(states);
  const std::vector<StackEntry> entries = states.Entries();
  if (entries.empty()) return true;
  const StackEntry& top = entries.back();
  for (StateScope& scope : scopes_) {
    if (scope.state != top.state || scope.id != top.id) continue;
    if (registry_->ScopeResident(scope.scope)) {
      if (!scope.resident_logged) {
        scope.resident_logged = true;
        const ScopeProgress progress = registry_->Progress(scope.scope);
        SE_LOG_INFO(
            "state '{}': its {} assets are resident after {:.1f} ms ({} "
            "failed: placeholders); its Worlds step",
            scope.id, progress.assets,
            static_cast<double>(SDL_GetTicksNS() - scope.created_ns) * kNsToMs,
            progress.failed);
      }
      holding_ = false;
      return true;
    }
    if (!holding_) {
      holding_ = true;
      const ScopeProgress progress = registry_->Progress(scope.scope);
      SE_LOG_INFO(
          "sim held ({}): state '{}' waits for {} of its {} assets to be "
          "resident",
          reason, scope.id, progress.loading, progress.assets);
    }
    return false;
  }
  return true;
}

bool AssetRuntime::Hold(const GameStateStack& states) {
  SE_ASSERT_MAIN_THREAD();
  const bool held = !TopResident(states, "no ticks planned");
  if (held) ++held_frames_;
  return held;
}

bool AssetRuntime::HoldTicks(const GameStateStack& states, Tick next_tick) {
  AssertMainThread();
  const bool held = !TopResident(states, "a tick pushed a state");
  if (held) {
    SE_LOG_DEBUG("tick {} and the rest of the frame held for assets",
                 next_tick);
  }
  return held;
}

void AssetRuntime::BeforeRender(const GameStateStack& states) {
  SE_ASSERT_MAIN_THREAD();
  SyncScopes(states);
}

bool AssetRuntime::Drawable(const World* world) const {
  for (const StateScope& scope : scopes_) {
    if (registry_->ScopeResident(scope.scope)) continue;
    const auto worlds = scope.state->Worlds();
    if (std::ranges::find(worlds, world) != worlds.end()) return false;
  }
  return true;
}

bool AssetRuntime::ReloadTexture(std::string_view name, std::string_view file) {
  SE_ASSERT_MAIN_THREAD();
  const Handle<TextureAsset> handle = registry_->FindTexture(name);
  if (handle.IsNull()) return false;
  const std::string from =
      file.empty() ? std::string() : fs::Join(config_.content_root, file);
  registry_->ReloadTexture(handle, from);
  return true;
}

void AssetRuntime::StopLane() noexcept {
  if (lane_) lane_->Stop();
}

void AssetRuntime::LogLaneStopped(const LaneStats& stats) {
  SE_LOG_INFO(
      "background lane stopped: {} jobs ({} failed, {} cancelled), {} steps "
      "({} Urgent), longest step {:.2f} ms, {} scans, {} changes",
      stats.completed + stats.failed, stats.failed, stats.cancelled,
      stats.steps, stats.urgent_steps,
      static_cast<double>(stats.max_step_ns) * kNsToMs, stats.scans,
      stats.changes);
}

void AssetRuntime::ReleaseScopes() {
  SE_ASSERT_MAIN_THREAD();
  if (!registry_) return;
  scopes_.clear();
  registry_->ReleaseAll();
}

}  // namespace se
