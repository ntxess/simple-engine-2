// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/asset_runtime.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
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
#include "se_sim/content_bundle.h"
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

std::string Describe(const Error& error) {
  std::string text = error.message;
  if (!error.path.empty()) {
    text += std::format(" ({}:{}:{})", error.path, error.line, error.column);
  }
  return text;
}

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
  if (foreign_listener_) foreign_listener_(done);
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

void AssetRuntime::RetainDefaults(AssetScope scope) {
  AssertMainThread();
  for (const TextureTableEntry& entry : config_.textures) {
    static_cast<void>(registry_->RequestTexture(scope, SourceOf(entry)));
  }
  for (const std::string& sound : config_.sounds) {
    static_cast<void>(
        registry_->RequestSound(scope, fs::Join(config_.content_root, sound)));
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
  // The sound table's sounds, then the state's own (ADR-087).
  if (wanted.all_sounds) {
    for (const std::string& path : config_.sounds) RequestSound(entry, path);
  }
  for (const std::string& path : wanted.sounds) RequestSound(entry, path);
  // M13: the state's texts (their dependencies follow as they land).
  for (const std::string& path : wanted.texts) RequestText(entry, path);
}

std::string AssetRuntime::ContentKey(std::string_view path) const {
  return fs::Normalize(path);
}

void AssetRuntime::RequestText(StateScope& entry, std::string_view path) {
  const std::string key = ContentKey(path);
  const bool known = std::ranges::any_of(
      entry.texts, [&key](const ScopeText& text) { return text.path == key; });
  if (known) return;
  ScopeText text;
  text.path = key;
  text.handle =
      registry_->RequestText(entry.scope, fs::Join(config_.content_root, key));
  entry.texts.push_back(std::move(text));
}

bool AssetRuntime::ExpandTexts(StateScope& entry) {
  // By index: a dependency appends to entry.texts.
  for (std::size_t i = 0; i < entry.texts.size(); ++i) {
    const AssetState state = registry_->State(entry.texts[i].handle);
    if (state == AssetState::kFailed) {
      if (!state_error_) {
        const Error* error = registry_->LastError(entry.texts[i].handle);
        state_error_ = error != nullptr
                           ? *error
                           : Error{ErrorCode::kIo, "the text did not load",
                                   entry.texts[i].path, 0, 0};
        state_error_state_ = entry.id;
        SE_LOG_ERROR(
            "state '{}' cannot be created: its text {} did not load: {}",
            entry.id, entry.texts[i].path, Describe(*state_error_));
      }
      return false;
    }
    if (entry.texts[i].expanded || state != AssetState::kResident) continue;
    entry.texts[i].expanded = true;
    if (!config_.text_dependencies) continue;
    const std::shared_ptr<const std::string> text =
        registry_->Text(entry.texts[i].handle);
    if (text == nullptr) continue;
    const std::string path = entry.texts[i].path;
    for (const std::string& dependency :
         config_.text_dependencies(path, *text)) {
      RequestText(entry, dependency);
    }
  }
  return true;
}

void AssetRuntime::PinTexts(StateScope& entry) {
  for (ScopeText& text : entry.texts) {
    text.pinned = registry_->Text(text.handle);
    if (text.pinned == nullptr) continue;
    const std::uint64_t hash = ContentTextHash(*text.pinned);
    const ContentText* before = loaded_texts_.Latest(text.path);
    const bool changed = before != nullptr && before->hash != hash;
    loaded_texts_.Add(text.path, *text.pinned);
    if (changed) {
      SE_LOG_INFO(
          "state '{}' loads {} as changed on disk (hash {:016x}): a changed "
          "text applies to the next state that loads it",
          entry.id, text.path, hash);
      if (text_swap_listener_) text_swap_listener_(text.path, hash);
    }
  }
}

bool AssetRuntime::Ready(std::string_view state_id) {
  AssertMainThread();
  auto found = std::ranges::find_if(scopes_, [state_id](const StateScope& s) {
    return s.pending && s.id == state_id;
  });
  if (found == scopes_.end()) {
    StateScope scope;
    scope.id = std::string(state_id);
    scope.scope = registry_->CreateScope(state_id);
    scope.created_ns = SDL_GetTicksNS();
    scope.pending = true;
    scopes_.push_back(std::move(scope));
    RequestStateAssets(scopes_.back());
    found = scopes_.end() - 1;
  }
  StateScope& entry = *found;
  if (entry.ready) return true;
  if (!ExpandTexts(entry)) return false;
  const bool texts_landed = std::ranges::all_of(
      entry.texts, [](const ScopeText& text) { return text.expanded; });
  if (!texts_landed || !registry_->ScopeResident(entry.scope)) return false;
  PinTexts(entry);
  entry.ready = true;
  entry.resident_logged = true;
  const ScopeProgress progress = registry_->Progress(entry.scope);
  SE_LOG_INFO(
      "state '{}': its {} assets are resident after {:.1f} ms ({} failed: "
      "placeholders; {} texts); its Worlds step",
      entry.id, progress.assets,
      static_cast<double>(SDL_GetTicksNS() - entry.created_ns) * kNsToMs,
      progress.failed, entry.texts.size());
  return true;
}

std::expected<std::string_view, Error> AssetRuntime::Text(
    std::string_view path) const {
  const std::string key = ContentKey(path);
  // The newest scopes first: a state being created reads its own versions.
  for (const StateScope& scope : scopes_ | std::views::reverse) {
    for (const ScopeText& text : scope.texts) {
      if (text.path == key && text.pinned != nullptr) {
        return std::string_view(*text.pinned);
      }
    }
  }
  return std::unexpected(
      Error{ErrorCode::kNotFound,
            "not a text of any state on the stack (name it in the state's "
            "StateAssets::texts, or in GameSetup::text_dependencies)",
            key, 0, 0});
}

void AssetRuntime::RequestSound(const StateScope& entry,
                                const std::string& path) {
  const std::string joined = fs::Join(config_.content_root, path);
  const Handle<SoundAsset> handle =
      registry_->RequestSound(entry.scope, joined);
  const bool tracked = std::ranges::any_of(
      sounds_,
      [&handle](const TrackedSound& sound) { return sound.handle == handle; });
  if (!tracked) sounds_.push_back({joined, handle, 0});
}

void AssetRuntime::NotifySounds() {
  // Sounds whose last scope released them: the listener drops them.
  std::erase_if(sounds_, [this](const TrackedSound& sound) {
    if (registry_->State(sound.handle) != AssetState::kNone) return false;
    if (sound_listener_) sound_listener_(sound.path, nullptr);
    return true;
  });
  for (TrackedSound& sound : sounds_) {
    if (registry_->State(sound.handle) != AssetState::kResident) continue;
    const std::uint32_t version = registry_->Version(sound.handle);
    if (version == sound.version) continue;
    sound.version = version;
    if (sound_listener_) {
      sound_listener_(sound.path, registry_->Sound(sound.handle));
    }
  }
}

void AssetRuntime::SyncScopes(const GameStateStack& states) {
  const std::vector<StackEntry> entries = states.Entries();
  // States that left the stack release their scopes.
  std::erase_if(scopes_, [&](const StateScope& scope) {
    if (scope.pending) return false;
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
    // M13: a state the gate let through takes the scope Ready() made.
    const auto pending =
        std::ranges::find_if(scopes_, [&entry](const StateScope& scope) {
          return scope.pending && scope.ready && scope.id == entry.id;
        });
    if (pending != scopes_.end()) {
      pending->state = entry.state;
      pending->pending = false;
      continue;
    }
    StateScope scope;
    scope.state = entry.state;
    scope.id = std::string(entry.id);
    scope.scope = registry_->CreateScope(entry.id);
    scope.created_ns = SDL_GetTicksNS();
    scopes_.push_back(std::move(scope));
    RequestStateAssets(scopes_.back());
  }
  // A scope made for a state that no longer waits (its command was
  // dropped, the stack cleared) is released.
  const std::string_view waiting = states.Waiting();
  std::erase_if(scopes_, [&](const StateScope& scope) {
    if (!scope.pending || scope.id == waiting) return false;
    AssertMainThread();
    registry_->ReleaseScope(scope.scope);
    return true;
  });
}

AssetScope AssetRuntime::ScopeOf(std::string_view id) const {
  for (const StateScope& scope : scopes_) {
    if (!scope.pending && scope.id == id) return scope.scope;
  }
  // M13: the scope of a state that waits at the content gate.
  for (const StateScope& scope : scopes_) {
    if (scope.id == id) return scope.scope;
  }
  return {};
}

void AssetRuntime::BeginFrame(const GameStateStack& states) {
  SE_ASSERT_MAIN_THREAD();
  SyncScopes(states);
  registry_->BeginFrame();
  NotifySounds();
}

bool AssetRuntime::TopResident(const GameStateStack& states,
                               std::string_view reason) {
  SyncScopes(states);
  const std::vector<StackEntry> entries = states.Entries();
  if (entries.empty()) return true;
  const StackEntry& top = entries.back();
  for (StateScope& scope : scopes_) {
    if (scope.pending || scope.state != top.state || scope.id != top.id) {
      continue;
    }
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
  if (const std::string_view waiting = states.Waiting(); !waiting.empty()) {
    // M13: a state waits at the content gate; no tick runs before it is
    // created (logged when the hold starts).
    ++held_frames_;
    if (!waiting_logged_) {
      waiting_logged_ = true;
      const AssetScope scope = [&] {
        for (const StateScope& s : scopes_) {
          if (s.pending && s.id == waiting) return s.scope;
        }
        return AssetScope{};
      }();
      const ScopeProgress progress = registry_->Progress(scope);
      SE_LOG_INFO(
          "sim held (no ticks planned): state '{}' waits at the content gate "
          "for {} of its {} assets",
          waiting, progress.loading, progress.assets);
    }
    return true;
  }
  waiting_logged_ = false;
  const bool held = !TopResident(states, "no ticks planned");
  if (held) ++held_frames_;
  return held;
}

bool AssetRuntime::HoldTicks(const GameStateStack& states, Tick next_tick) {
  AssertMainThread();
  const bool held = !states.Waiting().empty() ||
                    !TopResident(states, "a tick pushed a state");
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
    if (scope.state == nullptr || registry_->ScopeResident(scope.scope)) {
      continue;
    }
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
  sounds_.clear();
  registry_->ReleaseAll();
}

}  // namespace se
