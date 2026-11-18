// Copyright 2026 The simple-engine-2 Authors.

#include "se_assets/asset_registry.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_assets/sound_decoder.h"
#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"

namespace se {
namespace {

std::string DescribeError(const Error& error) {
  std::string text = error.message;
  if (!error.path.empty()) {
    text += " (";
    text += error.path;
    if (error.line > 0) {
      text += std::format(":{}:{}", error.line, error.column);
    }
    text += ")";
  }
  return text;
}

template <class H>
constexpr H MakeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
  H handle;
  handle.index = slot + 1;
  handle.generation = generation;
  return handle;
}

}  // namespace

std::string_view AssetStateName(AssetState state) noexcept {
  switch (state) {
    case AssetState::kNone:
      return "none";
    case AssetState::kLoading:
      return "loading";
    case AssetState::kUploading:
      return "uploading";
    case AssetState::kResident:
      return "resident";
    case AssetState::kFailed:
      return "failed";
  }
  return "?";
}

AssetRegistry::AssetRegistry(BackgroundLane& lane, TextureUploadPort* textures,
                             const AssetRegistryConfig& config)
    : lane_(&lane), textures_(textures), config_(config) {
  Scope ui;
  ui.name = "ui";
  ui.alive = true;
  scopes_.push_back(std::move(ui));
  ui_scope_ = MakeHandle<AssetScope>(0, scopes_[0].generation);
}

// --- Stores ---

template <class E>
const E* AssetRegistry::Lookup(const Store<E>& store, std::uint32_t index,
                               std::uint32_t generation) noexcept {
  if (index == 0 || index > store.slots.size()) return nullptr;
  const E& entry = store.slots[index - 1];
  if (!entry.common.alive || entry.common.generation != generation) {
    return nullptr;
  }
  return &entry;
}

template <class E>
std::uint32_t AssetRegistry::Acquire(Store<E>& store, const std::string& key,
                                     bool& created) {
  if (const auto found = store.by_key.find(key); found != store.by_key.end()) {
    created = false;
    return found->second;
  }
  created = true;
  std::uint32_t slot = 0;
  if (!store.free.empty()) {
    slot = store.free.back();
    store.free.pop_back();
  } else {
    slot = static_cast<std::uint32_t>(store.slots.size());
    store.slots.emplace_back();
  }
  E& entry = store.slots[slot];
  const std::uint32_t generation = entry.common.generation;
  entry = E{};
  entry.common.generation = generation;
  entry.common.alive = true;
  entry.common.key = key;
  entry.common.state = AssetState::kLoading;
  store.by_key.emplace(key, slot);
  return slot;
}

AssetRegistry::Common* AssetRegistry::CommonOf(const Ref& ref) noexcept {
  return const_cast<Common*>(std::as_const(*this).CommonOf(ref));
}

const AssetRegistry::Common* AssetRegistry::CommonOf(
    const Ref& ref) const noexcept {
  const std::uint32_t index = ref.slot + 1;
  switch (ref.kind) {
    case Kind::kTexture: {
      const auto* e = Lookup(textures_store_, index, ref.generation);
      return e != nullptr ? &e->common : nullptr;
    }
    case Kind::kSound: {
      const auto* e = Lookup(sounds_, index, ref.generation);
      return e != nullptr ? &e->common : nullptr;
    }
    case Kind::kPrefab: {
      const auto* e = Lookup(prefabs_, index, ref.generation);
      return e != nullptr ? &e->common : nullptr;
    }
    case Kind::kSceneChunk: {
      const auto* e = Lookup(chunks_, index, ref.generation);
      return e != nullptr ? &e->common : nullptr;
    }
    case Kind::kText: {
      const auto* e = Lookup(texts_, index, ref.generation);
      return e != nullptr ? &e->common : nullptr;
    }
  }
  return nullptr;
}

// --- Scopes ---

AssetScope AssetRegistry::CreateScope(std::string_view name) {
  SE_ASSERT_MAIN_THREAD();
  std::uint32_t slot = 0;
  if (!free_scopes_.empty()) {
    slot = free_scopes_.back();
    free_scopes_.pop_back();
  } else {
    slot = static_cast<std::uint32_t>(scopes_.size());
    scopes_.emplace_back();
  }
  Scope& scope = scopes_[slot];
  scope.name = std::string(name);
  scope.alive = true;
  scope.refs.clear();
  return MakeHandle<AssetScope>(slot, scope.generation);
}

AssetRegistry::Scope* AssetRegistry::ScopeOf(AssetScope scope) noexcept {
  return const_cast<Scope*>(std::as_const(*this).ScopeOf(scope));
}

const AssetRegistry::Scope* AssetRegistry::ScopeOf(
    AssetScope scope) const noexcept {
  if (scope.index == 0 || scope.index > scopes_.size()) return nullptr;
  const Scope& found = scopes_[scope.index - 1];
  if (!found.alive || found.generation != scope.generation) return nullptr;
  return &found;
}

bool AssetRegistry::ScopeAlive(AssetScope scope) const noexcept {
  return ScopeOf(scope) != nullptr;
}

std::string_view AssetRegistry::ScopeName(AssetScope scope) const {
  const Scope* found = ScopeOf(scope);
  return found != nullptr ? std::string_view(found->name) : std::string_view();
}

void AssetRegistry::ReleaseScope(AssetScope scope) {
  SE_ASSERT_MAIN_THREAD();
  Scope* found = ScopeOf(scope);
  if (found == nullptr) return;
  const std::vector<Ref> refs = std::move(found->refs);
  found->refs.clear();
  found->alive = false;
  ++found->generation;
  if (scope.index - 1 != ui_scope_.index - 1) {
    free_scopes_.push_back(scope.index - 1);
  }
  for (const Ref& ref : refs) Unref(ref);
}

ScopeProgress AssetRegistry::Progress(AssetScope scope) const {
  ScopeProgress progress;
  const Scope* found = ScopeOf(scope);
  if (found == nullptr) return progress;
  for (const Ref& ref : found->refs) {
    const Common* common = CommonOf(ref);
    if (common == nullptr) continue;
    ++progress.assets;
    if (common->state == AssetState::kFailed) {
      ++progress.failed;
    } else if (common->state != AssetState::kResident) {
      ++progress.loading;
    }
  }
  return progress;
}

bool AssetRegistry::ScopeResident(AssetScope scope) const {
  if (ScopeOf(scope) == nullptr) return false;
  return Progress(scope).loading == 0;
}

bool AssetRegistry::AddRef(Scope& scope, const Ref& ref) {
  if (std::ranges::find(scope.refs, ref) != scope.refs.end()) return false;
  scope.refs.push_back(ref);
  if (Common* common = CommonOf(ref)) ++common->refs;
  return true;
}

void AssetRegistry::Unref(const Ref& ref) {
  Common* common = CommonOf(ref);
  if (common == nullptr) return;
  SE_ASSERT(common->refs > 0);
  if (--common->refs == 0) Remove(ref);
}

void AssetRegistry::Remove(const Ref& ref) {
  Common* common = CommonOf(ref);
  if (common == nullptr) return;
  // Its completions still in flight are dropped when they arrive.
  std::erase_if(pending_,
                [&ref](const auto& item) { return item.second.ref == ref; });
  for (const std::string& path : common->watched) {
    lane_->Submit(JobDesc{}, MakeWatchJob(path, TagOf(ref), /*watch=*/false));
  }
  const std::string key = common->key;
  common->alive = false;
  ++common->generation;
  common->watched.clear();
  switch (ref.kind) {
    case Kind::kTexture: {
      TextureEntry& entry = textures_store_.slots[ref.slot];
      if (textures_ != nullptr && !entry.gpu.IsNull()) {
        textures_->Remove(entry.gpu);
      }
      entry.gpu = {};
      entry.atlas = {};
      textures_store_.by_key.erase(key);
      textures_store_.free.push_back(ref.slot);
      break;
    }
    case Kind::kSound:
      sounds_.slots[ref.slot].pcm.reset();
      sounds_.by_key.erase(key);
      sounds_.free.push_back(ref.slot);
      break;
    case Kind::kPrefab:
      prefabs_.slots[ref.slot].payload.reset();
      prefabs_.by_key.erase(key);
      prefabs_.free.push_back(ref.slot);
      break;
    case Kind::kSceneChunk:
      chunks_.slots[ref.slot].payload.reset();
      chunks_.by_key.erase(key);
      chunks_.free.push_back(ref.slot);
      break;
    case Kind::kText:
      texts_.slots[ref.slot].text.reset();
      texts_.by_key.erase(key);
      texts_.free.push_back(ref.slot);
      break;
  }
}

void AssetRegistry::ReleaseAll() {
  SE_ASSERT_MAIN_THREAD();
  for (std::size_t i = 0; i < scopes_.size(); ++i) {
    if (!scopes_[i].alive) continue;
    ReleaseScope(MakeHandle<AssetScope>(static_cast<std::uint32_t>(i),
                                        scopes_[i].generation));
  }
}

// --- Requests ---

std::uint64_t AssetRegistry::TagOf(const Ref& ref) noexcept {
  return (std::uint64_t{static_cast<std::uint8_t>(ref.kind)} << 60) |
         (std::uint64_t{ref.slot & 0x0FFFFFFFU} << 32) | ref.generation;
}

AssetRegistry::Ref AssetRegistry::RefOfTag(std::uint64_t tag) noexcept {
  Ref ref;
  ref.kind = static_cast<Kind>(tag >> 60);
  ref.slot = static_cast<std::uint32_t>((tag >> 32) & 0x0FFFFFFFU);
  ref.generation = static_cast<std::uint32_t>(tag & 0xFFFFFFFFU);
  return ref;
}

Ticket AssetRegistry::Submit(const Ref& ref, bool reload,
                             std::unique_ptr<LaneJob> job) {
  JobDesc desc;
  desc.job_class = JobClass::kUrgent;
  const Ticket ticket = lane_->Submit(desc, std::move(job));
  pending_[ticket] = Pending{ref, reload};
  owned_.insert(ticket);
  if (Common* common = CommonOf(ref)) common->ticket = ticket;
  if (reload) ++stats_.reloads;
  return ticket;
}

void AssetRegistry::WatchPath(const Ref& ref, const std::string& path) {
  if (!config_.hot_reload || path.empty()) return;
  Common* common = CommonOf(ref);
  if (common == nullptr ||
      std::ranges::find(common->watched, path) != common->watched.end()) {
    return;
  }
  common->watched.push_back(path);
  lane_->Submit(JobDesc{}, MakeWatchJob(path, TagOf(ref), /*watch=*/true));
}

void AssetRegistry::SubmitTextureLoad(std::uint32_t slot,
                                      AtlasRequest::Mode mode,
                                      std::string_view source_override,
                                      bool reload) {
  TextureEntry& entry = textures_store_.slots[slot];
  AtlasRequest request;
  request.name = entry.source.name;
  request.page_path = entry.source.page_path;
  request.source_path = source_override.empty() ? entry.source.source_path
                                                : std::string(source_override);
  request.mode = mode;
  if (!source_override.empty()) request.mode = AtlasRequest::Mode::kCook;
  const Ref ref{Kind::kTexture, slot, entry.common.generation};
  Submit(ref, reload, MakeAtlasJob(std::move(request)));
}

Handle<TextureAsset> AssetRegistry::RequestTexture(
    AssetScope scope, const TextureSource& source) {
  SE_ASSERT_MAIN_THREAD();
  Scope* target = ScopeOf(scope);
  if (target == nullptr || source.name.empty()) return {};
  ++stats_.requests;
  bool created = false;
  const std::uint32_t slot = Acquire(textures_store_, source.name, created);
  TextureEntry& entry = textures_store_.slots[slot];
  const Ref ref{Kind::kTexture, slot, entry.common.generation};
  AddRef(*target, ref);
  if (created) {
    entry.source = source;
    if (textures_ != nullptr) entry.gpu = textures_->Register(source.name);
    SubmitTextureLoad(slot, config_.page_mode, {}, /*reload=*/false);
    WatchPath(ref, source.source_path);
    WatchPath(ref, source.page_path);
  }
  return MakeHandle<Handle<TextureAsset>>(slot, entry.common.generation);
}

void AssetRegistry::SubmitFileLoad(Kind kind, std::uint32_t slot, bool reload) {
  switch (kind) {
    case Kind::kSound: {
      SoundEntry& entry = sounds_.slots[slot];
      Submit(Ref{kind, slot, entry.common.generation}, reload,
             MakeSoundJob(entry.common.key, decoder_));
      break;
    }
    case Kind::kPrefab: {
      PrefabEntry& entry = prefabs_.slots[slot];
      Submit(Ref{kind, slot, entry.common.generation}, reload,
             MakeJsonJob(entry.common.key, JsonKind::kPrefab));
      break;
    }
    case Kind::kSceneChunk: {
      ChunkEntry& entry = chunks_.slots[slot];
      Submit(Ref{kind, slot, entry.common.generation}, reload,
             MakeJsonJob(entry.common.key, JsonKind::kSceneChunk));
      break;
    }
    case Kind::kText: {
      TextEntry& entry = texts_.slots[slot];
      Submit(Ref{kind, slot, entry.common.generation}, reload,
             MakeTextJob(entry.common.key));
      break;
    }
    case Kind::kTexture:
      break;
  }
}

Handle<SoundAsset> AssetRegistry::RequestSound(AssetScope scope,
                                               std::string_view path) {
  SE_ASSERT_MAIN_THREAD();
  Scope* target = ScopeOf(scope);
  if (target == nullptr || path.empty()) return {};
  ++stats_.requests;
  bool created = false;
  const std::string key = fs::Normalize(path);
  const std::uint32_t slot = Acquire(sounds_, key, created);
  const std::uint32_t generation = sounds_.slots[slot].common.generation;
  const Ref ref{Kind::kSound, slot, generation};
  AddRef(*target, ref);
  if (created) {
    SubmitFileLoad(Kind::kSound, slot, /*reload=*/false);
    WatchPath(ref, key);
  }
  return MakeHandle<Handle<SoundAsset>>(slot, generation);
}

Handle<PrefabAsset> AssetRegistry::RequestPrefab(AssetScope scope,
                                                 std::string_view path) {
  SE_ASSERT_MAIN_THREAD();
  Scope* target = ScopeOf(scope);
  if (target == nullptr || path.empty()) return {};
  ++stats_.requests;
  bool created = false;
  const std::string key = fs::Normalize(path);
  const std::uint32_t slot = Acquire(prefabs_, key, created);
  const std::uint32_t generation = prefabs_.slots[slot].common.generation;
  const Ref ref{Kind::kPrefab, slot, generation};
  AddRef(*target, ref);
  if (created) {
    SubmitFileLoad(Kind::kPrefab, slot, /*reload=*/false);
    WatchPath(ref, key);
  }
  return MakeHandle<Handle<PrefabAsset>>(slot, generation);
}

Handle<SceneChunkAsset> AssetRegistry::RequestSceneChunk(
    AssetScope scope, std::string_view path) {
  SE_ASSERT_MAIN_THREAD();
  Scope* target = ScopeOf(scope);
  if (target == nullptr || path.empty()) return {};
  ++stats_.requests;
  bool created = false;
  const std::string key = fs::Normalize(path);
  const std::uint32_t slot = Acquire(chunks_, key, created);
  const std::uint32_t generation = chunks_.slots[slot].common.generation;
  const Ref ref{Kind::kSceneChunk, slot, generation};
  AddRef(*target, ref);
  if (created) {
    SubmitFileLoad(Kind::kSceneChunk, slot, /*reload=*/false);
    WatchPath(ref, key);
  }
  return MakeHandle<Handle<SceneChunkAsset>>(slot, generation);
}

Handle<TextAsset> AssetRegistry::RequestText(AssetScope scope,
                                             std::string_view path) {
  SE_ASSERT_MAIN_THREAD();
  Scope* target = ScopeOf(scope);
  if (target == nullptr || path.empty()) return {};
  ++stats_.requests;
  bool created = false;
  const std::string key = fs::Normalize(path);
  const std::uint32_t slot = Acquire(texts_, key, created);
  const std::uint32_t generation = texts_.slots[slot].common.generation;
  const Ref ref{Kind::kText, slot, generation};
  AddRef(*target, ref);
  if (created) {
    SubmitFileLoad(Kind::kText, slot, /*reload=*/false);
    WatchPath(ref, key);
  }
  return MakeHandle<Handle<TextAsset>>(slot, generation);
}

void AssetRegistry::ReloadTexture(Handle<TextureAsset> handle,
                                  std::string_view file) {
  SE_ASSERT_MAIN_THREAD();
  const TextureEntry* entry =
      Lookup(textures_store_, handle.index, handle.generation);
  if (entry == nullptr) return;
  const std::uint32_t slot = handle.index - 1;
  SubmitTextureLoad(slot, AtlasRequest::Mode::kCook, file, /*reload=*/true);
}

// --- Lookups ---

AssetState AssetRegistry::State(Handle<TextureAsset> handle) const {
  const auto* e = Lookup(textures_store_, handle.index, handle.generation);
  return e != nullptr ? e->common.state : AssetState::kNone;
}

AssetState AssetRegistry::State(Handle<SoundAsset> handle) const {
  const auto* e = Lookup(sounds_, handle.index, handle.generation);
  return e != nullptr ? e->common.state : AssetState::kNone;
}

AssetState AssetRegistry::State(Handle<PrefabAsset> handle) const {
  const auto* e = Lookup(prefabs_, handle.index, handle.generation);
  return e != nullptr ? e->common.state : AssetState::kNone;
}

AssetState AssetRegistry::State(Handle<SceneChunkAsset> handle) const {
  const auto* e = Lookup(chunks_, handle.index, handle.generation);
  return e != nullptr ? e->common.state : AssetState::kNone;
}

AssetState AssetRegistry::State(Handle<TextAsset> handle) const {
  const auto* e = Lookup(texts_, handle.index, handle.generation);
  return e != nullptr ? e->common.state : AssetState::kNone;
}

Handle<TextureAsset> AssetRegistry::FindTexture(std::string_view name) const {
  const auto found = textures_store_.by_key.find(std::string(name));
  if (found == textures_store_.by_key.end()) return {};
  return MakeHandle<Handle<TextureAsset>>(
      found->second, textures_store_.slots[found->second].common.generation);
}

TextureHandle AssetRegistry::GpuTexture(Handle<TextureAsset> handle) const {
  const auto* e = Lookup(textures_store_, handle.index, handle.generation);
  return e != nullptr ? e->gpu : TextureHandle{};
}

Handle<AtlasAsset> AssetRegistry::AtlasOf(Handle<TextureAsset> handle) const {
  // A texture's atlas shares its slot and generation.
  if (Lookup(textures_store_, handle.index, handle.generation) == nullptr) {
    return {};
  }
  Handle<AtlasAsset> atlas;
  atlas.index = handle.index;
  atlas.generation = handle.generation;
  return atlas;
}

const AtlasInfo* AssetRegistry::Atlas(Handle<AtlasAsset> handle) const {
  const auto* e = Lookup(textures_store_, handle.index, handle.generation);
  if (e == nullptr || e->common.version == 0) return nullptr;
  return &e->atlas;
}

std::shared_ptr<const PcmPayload> AssetRegistry::Sound(
    Handle<SoundAsset> handle) const {
  const auto* e = Lookup(sounds_, handle.index, handle.generation);
  return e != nullptr ? e->pcm : nullptr;
}

std::shared_ptr<const PrefabPayload> AssetRegistry::Prefab(
    Handle<PrefabAsset> handle) const {
  const auto* e = Lookup(prefabs_, handle.index, handle.generation);
  return e != nullptr ? e->payload : nullptr;
}

std::shared_ptr<const SceneChunkPayload> AssetRegistry::SceneChunk(
    Handle<SceneChunkAsset> handle) const {
  const auto* e = Lookup(chunks_, handle.index, handle.generation);
  return e != nullptr ? e->payload : nullptr;
}

std::shared_ptr<const std::string> AssetRegistry::Text(
    Handle<TextAsset> handle) const {
  const auto* e = Lookup(texts_, handle.index, handle.generation);
  return e != nullptr ? e->text : nullptr;
}

std::string_view AssetRegistry::TextPath(Handle<TextAsset> handle) const {
  const auto* e = Lookup(texts_, handle.index, handle.generation);
  return e != nullptr ? std::string_view(e->common.key) : std::string_view();
}

std::uint32_t AssetRegistry::Version(Handle<TextAsset> handle) const {
  const auto* e = Lookup(texts_, handle.index, handle.generation);
  return e != nullptr ? e->common.version : 0;
}

const Error* AssetRegistry::LastError(Handle<TextAsset> handle) const {
  const auto* e = Lookup(texts_, handle.index, handle.generation);
  return e != nullptr && e->common.error ? &*e->common.error : nullptr;
}

std::uint32_t AssetRegistry::Version(Handle<TextureAsset> handle) const {
  const auto* e = Lookup(textures_store_, handle.index, handle.generation);
  return e != nullptr ? e->common.version : 0;
}

std::uint32_t AssetRegistry::Version(Handle<SoundAsset> handle) const {
  const auto* e = Lookup(sounds_, handle.index, handle.generation);
  return e != nullptr ? e->common.version : 0;
}

const Error* AssetRegistry::LastError(Handle<TextureAsset> handle) const {
  const auto* e = Lookup(textures_store_, handle.index, handle.generation);
  return e != nullptr && e->common.error ? &*e->common.error : nullptr;
}

const Error* AssetRegistry::LastError(Handle<SoundAsset> handle) const {
  const auto* e = Lookup(sounds_, handle.index, handle.generation);
  return e != nullptr && e->common.error ? &*e->common.error : nullptr;
}

std::size_t AssetRegistry::TextureCount() const noexcept {
  return textures_store_.by_key.size();
}

// --- Completions ---

void AssetRegistry::BeginFrame() {
  SE_ASSERT_MAIN_THREAD();
  lane_->DrainCompletions([this](LaneCompletion&& completion) {
    AssertMainThread();
    OnCompletion(std::move(completion));
  });
  UpdateResidency();
}

void AssetRegistry::Failed(Common& common, bool reload, Error error) {
  if (reload) {
    // The old asset stays.
    ++stats_.reload_failures;
    SE_LOG_ERROR("asset '{}': reload failed, keeping the loaded one: {}",
                 common.key, DescribeError(error));
  } else {
    ++stats_.failures;
    common.state = AssetState::kFailed;
    SE_LOG_ERROR("asset '{}' did not load: {}", common.key,
                 DescribeError(error));
  }
  common.error = std::move(error);
}

void AssetRegistry::OnCompletion(LaneCompletion&& completion) {
  if (completion.result &&
      std::holds_alternative<FileChange>(*completion.result)) {
    if ((std::get<FileChange>(*completion.result).tag & kForeignWatchTag) !=
        0) {
      // A watch of the registry's user (the editor's documents).
      if (foreign_) foreign_(std::move(completion));
      return;
    }
    OnFileChange(std::get<FileChange>(*completion.result));
    return;
  }
  // The registry's own watch list changes.
  if (completion.kind == "watch" || completion.kind == "unwatch") return;
  const auto found = pending_.find(completion.ticket);
  if (found == pending_.end()) {
    if (owned_.contains(completion.ticket) || !foreign_) {
      // A load whose entry was removed before it completed.
      owned_.erase(completion.ticket);
      ++stats_.dropped;
    } else {
      foreign_(std::move(completion));
    }
    return;
  }
  owned_.erase(completion.ticket);
  const Pending pending = found->second;
  pending_.erase(found);
  Common* common = CommonOf(pending.ref);
  if (common == nullptr) {
    ++stats_.dropped;
    return;
  }
  // An older load of the same entry was superseded by a newer one.
  if (common->ticket != completion.ticket) {
    ++stats_.dropped;
    return;
  }
  common->ticket = 0;
  if (!completion.result) {
    Failed(*common, pending.reload && common->version > 0,
           std::move(completion.result.error()));
    return;
  }
  LanePayload& payload = *completion.result;
  switch (pending.ref.kind) {
    case Kind::kTexture:
      OnTexture(pending.ref.slot, pending.reload, payload);
      return;
    case Kind::kSound:
      if (auto* pcm = std::get_if<PcmPayload>(&payload)) {
        sounds_.slots[pending.ref.slot].pcm =
            std::make_shared<const PcmPayload>(std::move(*pcm));
      }
      break;
    case Kind::kPrefab:
      if (auto* prefab = std::get_if<PrefabPayload>(&payload)) {
        prefabs_.slots[pending.ref.slot].payload =
            std::make_shared<const PrefabPayload>(std::move(*prefab));
      }
      break;
    case Kind::kSceneChunk:
      if (auto* chunk = std::get_if<SceneChunkPayload>(&payload)) {
        chunks_.slots[pending.ref.slot].payload =
            std::make_shared<const SceneChunkPayload>(std::move(*chunk));
      }
      break;
    case Kind::kText:
      if (auto* text = std::get_if<TextPayload>(&payload)) {
        texts_.slots[pending.ref.slot].text =
            std::make_shared<const std::string>(std::move(text->text));
      }
      break;
  }
  ++stats_.loads;
  if (pending.reload) ++stats_.reloads_resident;
  common->state = AssetState::kResident;
  common->error.reset();
  ++common->version;
}

void AssetRegistry::OnTexture(std::uint32_t slot, bool reload,
                              LanePayload& payload) {
  TextureEntry& entry = textures_store_.slots[slot];
  auto* page = std::get_if<AtlasPage>(&payload);
  if (page == nullptr) {
    Failed(entry.common, reload && entry.common.version > 0,
           Error{ErrorCode::kCorrupt, "the lane returned no atlas page",
                 entry.source.page_path, 0, 0});
    return;
  }
  ++stats_.loads;
  if (page->recooked) ++stats_.recooked;
  AtlasInfo atlas;
  atlas.width = page->width;
  atlas.height = page->height;
  atlas.regions = page->regions;
  atlas.recooked = page->recooked;
  if (textures_ == nullptr || entry.gpu.IsNull()) {
    // Headless: nothing to upload.
    entry.atlas = std::move(atlas);
    entry.common.state = AssetState::kResident;
    entry.common.error.reset();
    ++entry.common.version;
    ++stats_.resident;
    if (reload) ++stats_.reloads_resident;
    return;
  }
  const std::uint32_t before = textures_->ContentGeneration(entry.gpu);
  if (auto uploaded = textures_->Upload(entry.gpu, std::move(*page));
      !uploaded) {
    Failed(entry.common, reload && entry.common.version > 0,
           std::move(uploaded.error()));
    return;
  }
  ++stats_.uploads;
  entry.atlas = std::move(atlas);
  entry.generation_at_upload = before;
  entry.common.error.reset();
  if (reload && entry.common.version > 0) {
    // The old texture draws until the new one is resident.
    entry.reload_uploading = true;
  } else {
    entry.common.state = AssetState::kUploading;
  }
}

void AssetRegistry::UpdateResidency() {
  if (textures_ == nullptr) return;
  for (TextureEntry& entry : textures_store_.slots) {
    if (!entry.common.alive || entry.gpu.IsNull()) continue;
    const bool waiting =
        entry.common.state == AssetState::kUploading || entry.reload_uploading;
    if (!waiting) continue;
    if (!textures_->IsResident(entry.gpu) ||
        textures_->ContentGeneration(entry.gpu) == entry.generation_at_upload) {
      continue;
    }
    if (entry.reload_uploading) {
      ++stats_.reloads_resident;
      SE_LOG_INFO("texture '{}' reloaded (version {})", entry.common.key,
                  entry.common.version + 1);
    }
    entry.reload_uploading = false;
    entry.common.state = AssetState::kResident;
    ++entry.common.version;
    ++stats_.resident;
  }
}

void AssetRegistry::SetGameplayFrozen(bool frozen) {
  AssertMainThread();
  if (frozen_ == frozen) return;
  frozen_ = frozen;
  if (frozen_) return;
  std::vector<FileChange> deferred = std::move(deferred_);
  deferred_.clear();
  for (const FileChange& change : deferred) OnFileChange(change);
}

void AssetRegistry::OnFileChange(const FileChange& change) {
  ++stats_.file_changes;
  const Ref ref = RefOfTag(change.tag);
  Common* common = CommonOf(ref);
  if (common == nullptr) return;
  if (!change.exists) {
    SE_LOG_WARN("asset '{}': {} was removed; keeping the loaded one",
                common->key, change.path);
    return;
  }
  if (frozen_) {
    if (ref.kind == Kind::kPrefab || ref.kind == Kind::kSceneChunk ||
        ref.kind == Kind::kText) {
      // Editor Play: gameplay assets wait for Stop (M10a).
      SE_LOG_INFO("asset '{}': {} changed during Play; reloaded at Stop",
                  common->key, change.path);
      deferred_.push_back(change);
      return;
    }
    if (swap_listener_) {
      Hasher hash(0x5A9A55E7);
      hash.AddString(change.path);
      hash.AddU64(change.size);
      hash.AddU64(static_cast<std::uint64_t>(change.modify_time_ns));
      swap_listener_(common->key, hash.Finish());
    }
  }
  SE_LOG_INFO("asset '{}': {} changed; reloading", common->key, change.path);
  if (ref.kind == Kind::kTexture) {
    TextureEntry& entry = textures_store_.slots[ref.slot];
    const bool source_changed =
        fs::Normalize(change.path) == fs::Normalize(entry.source.source_path);
    // A changed source is re-cooked; a re-cooked page is read again
    // (checked against its source).
    SubmitTextureLoad(
        ref.slot,
        source_changed ? AtlasRequest::Mode::kCook : config_.page_mode, {},
        /*reload=*/true);
    return;
  }
  SubmitFileLoad(ref.kind, ref.slot, /*reload=*/true);
}

}  // namespace se
