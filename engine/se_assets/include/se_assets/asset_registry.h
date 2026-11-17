// Copyright 2026 The simple-engine-2 Authors.
//
// AssetRegistry: the asset tables on main (docs/milestone-specs.md M9;
// docs/design/mt-design.md "Audio And Assets"). Main thread only; the
// background lane does the I/O and decoding and never sees these tables.
//
// Typed stores: Texture (an atlas page on the GPU, through M7's
// GpuResources behind TextureUploadPort), Atlas (the page's regions), Sound
// (decoded PCM; M8 plays it), Prefab and SceneChunk (parsed payloads).
// Each entry is named by Handle<T>{index, generation}; a removed entry's
// handles go stale (its slot's generation changes).
//
// Scopes: an AssetScope per GameState (the runtime makes one for each
// state on the stack), later per EditDocument (M10), and the persistent UI
// scope. Requesting an asset in a scope references it once per scope; an
// entry lives while a scope references it, and releasing the last scope
// removes it (a texture's GPU slot is retired through the retire queue).
// Completions of a removed entry are dropped.
//
// Loading: Request*() submits a lane job (Urgent: someone waits on it).
// BeginFrame(), at iterate start, drains the completion ring: a page goes
// to the upload port (GpuResources creates the texture and queues the
// upload within its 8 MB per frame budget); the texture becomes resident
// when its fence signals (M7). An entry whose load failed is settled: its
// sprites draw the magenta fallback and the Error (code, message, path,
// line, column) is logged and kept. ScopeResident() is true once every
// entry of the scope is resident or failed; the runtime holds the scope's
// World until then.
//
// Hot reload (dev builds): every texture's source and page, and every
// sound, prefab and scene chunk file, is watched by the lane's 500 ms
// scan. A change submits an Urgent reload (a changed PNG is re-cooked on
// the lane); the old texture keeps drawing until the new one is resident,
// when GpuResources swaps them at frame start and retires the old one. A
// reload that fails keeps the old asset. Reloading a sound gives the new
// PCM to new voices only (the old shared_ptr lives on in its voices).

#ifndef SE_ASSETS_ASSET_REGISTRY_H_
#define SE_ASSETS_ASSET_REGISTRY_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_assets/sound_decoder.h"
#include "se_core/error.h"
#include "se_core/handle.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

// Hot-reload watch tags with this bit set are not the registry's: the
// scan's FileChange for them goes to the foreign handler (M11b: the
// editor's documents watch their files, ADR-120). The registry's own tags
// hold a store kind below 8 in their top four bits.
inline constexpr std::uint64_t kForeignWatchTag = std::uint64_t{1} << 63;

// Store tags.
struct TextureAsset {};
struct AtlasAsset {};
struct SoundAsset {};
struct PrefabAsset {};
struct SceneChunkAsset {};
struct AssetScopeTag {};
using AssetScope = Handle<AssetScopeTag>;

enum class AssetState : std::uint8_t {
  // A null, stale or unknown handle.
  kNone,
  // The lane is loading it.
  kLoading,
  // A texture whose upload is queued or in flight.
  kUploading,
  kResident,
  // The load failed: the placeholder shows; LastError() says why.
  kFailed,
};

[[nodiscard]] std::string_view AssetStateName(AssetState state) noexcept;

// The GPU side of textures. se_runtime implements it over GpuResources;
// headless tests use a fake. Main thread only.
class TextureUploadPort {
 public:
  virtual ~TextureUploadPort() = default;
  // The texture slot for |name| (the existing one when it has one).
  virtual TextureHandle Register(std::string_view name) = 0;
  // Creates the slot's next texture and queues |page|'s upload.
  [[nodiscard]] virtual std::expected<void, Error> Upload(TextureHandle slot,
                                                          AtlasPage page) = 0;
  [[nodiscard]] virtual bool IsResident(TextureHandle slot) const = 0;
  // Changes when the slot's resident texture changes.
  [[nodiscard]] virtual std::uint32_t ContentGeneration(
      TextureHandle slot) const = 0;
  // Retires the slot's textures; the handle goes stale.
  virtual void Remove(TextureHandle slot) = 0;
};

struct TextureSource {
  // The texture table's name: the GPU slot's name and the key sprites use.
  std::string name;
  // The cooked page; empty: cook from the source.
  std::string page_path;
  // The PNG the page is cooked from; empty: none.
  std::string source_path;
};

struct AtlasInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<AtlasRegion> regions;
  bool recooked = false;
};

struct ScopeProgress {
  std::uint32_t assets = 0;
  std::uint32_t loading = 0;
  std::uint32_t failed = 0;
};

struct RegistryStats {
  std::uint64_t requests = 0;
  std::uint64_t loads = 0;
  std::uint64_t failures = 0;
  std::uint64_t uploads = 0;
  std::uint64_t resident = 0;
  std::uint64_t file_changes = 0;
  std::uint64_t reloads = 0;
  std::uint64_t reloads_resident = 0;
  std::uint64_t reload_failures = 0;
  // Completions whose entry was removed (its scopes released) first.
  std::uint64_t dropped = 0;
  // Pages cooked on the lane instead of read from a .seatlas file.
  std::uint64_t recooked = 0;
};

struct AssetRegistryConfig {
  // How texture pages load: kPageIfFresh in dev builds (a stale or missing
  // page re-cooks from its source), kPage in Release.
  AtlasRequest::Mode page_mode = AtlasRequest::Mode::kPageIfFresh;
  // Watch every loaded file with the lane's scan (dev builds).
  bool hot_reload = true;
};

class AssetRegistry {
 public:
  // |textures| may be null (headless): texture loads then settle as
  // resident without a GPU texture.
  AssetRegistry(BackgroundLane& lane, TextureUploadPort* textures,
                const AssetRegistryConfig& config);
  AssetRegistry(const AssetRegistry&) = delete;
  AssetRegistry& operator=(const AssetRegistry&) = delete;
  // Does not release the scopes: the owner calls ReleaseAll() (shutdown
  // step 10), which may allocate; the GPU slots then go with the renderer.
  ~AssetRegistry() = default;

  // --- Scopes ---
  AssetScope CreateScope(std::string_view name) SE_REQUIRES(g_main);
  // The persistent UI scope (released only by ReleaseAll).
  [[nodiscard]] AssetScope UiScope() const noexcept { return ui_scope_; }
  void ReleaseScope(AssetScope scope) SE_REQUIRES(g_main);
  [[nodiscard]] bool ScopeAlive(AssetScope scope) const noexcept;
  // Every asset of |scope| is resident or failed (an empty scope is).
  [[nodiscard]] bool ScopeResident(AssetScope scope) const;
  [[nodiscard]] ScopeProgress Progress(AssetScope scope) const;
  [[nodiscard]] std::string_view ScopeName(AssetScope scope) const;

  // --- Requests (Urgent lane jobs) ---
  Handle<TextureAsset> RequestTexture(AssetScope scope,
                                      const TextureSource& source)
      SE_REQUIRES(g_main);
  Handle<SoundAsset> RequestSound(AssetScope scope, std::string_view path)
      SE_REQUIRES(g_main);
  Handle<PrefabAsset> RequestPrefab(AssetScope scope, std::string_view path)
      SE_REQUIRES(g_main);
  Handle<SceneChunkAsset> RequestSceneChunk(AssetScope scope,
                                            std::string_view path)
      SE_REQUIRES(g_main);

  // --- Lookups ---
  [[nodiscard]] AssetState State(Handle<TextureAsset> handle) const;
  [[nodiscard]] AssetState State(Handle<SoundAsset> handle) const;
  [[nodiscard]] AssetState State(Handle<PrefabAsset> handle) const;
  [[nodiscard]] AssetState State(Handle<SceneChunkAsset> handle) const;
  [[nodiscard]] Handle<TextureAsset> FindTexture(std::string_view name) const;
  [[nodiscard]] TextureHandle GpuTexture(Handle<TextureAsset> handle) const;
  [[nodiscard]] Handle<AtlasAsset> AtlasOf(Handle<TextureAsset> handle) const;
  [[nodiscard]] const AtlasInfo* Atlas(Handle<AtlasAsset> handle) const;
  [[nodiscard]] std::shared_ptr<const PcmPayload> Sound(
      Handle<SoundAsset> handle) const;
  [[nodiscard]] std::shared_ptr<const PrefabPayload> Prefab(
      Handle<PrefabAsset> handle) const;
  [[nodiscard]] std::shared_ptr<const SceneChunkPayload> SceneChunk(
      Handle<SceneChunkAsset> handle) const;
  // Counts the loads and reloads that landed (1 after the first).
  [[nodiscard]] std::uint32_t Version(Handle<TextureAsset> handle) const;
  [[nodiscard]] std::uint32_t Version(Handle<SoundAsset> handle) const;
  // The last failure of the entry's load or reload, or null.
  [[nodiscard]] const Error* LastError(Handle<TextureAsset> handle) const;
  [[nodiscard]] const Error* LastError(Handle<SoundAsset> handle) const;
  [[nodiscard]] std::size_t TextureCount() const noexcept;

  // Iterate start: drains the lane's completions into the stores (and
  // passes the ones the registry did not submit to the foreign handler),
  // starts reloads for changed files, and promotes uploaded textures to
  // resident.
  void BeginFrame() SE_REQUIRES(g_main);
  // Reloads |handle| now (Urgent): re-cooks its source, or cooks |file|
  // instead (the --reload-texture hook).
  void ReloadTexture(Handle<TextureAsset> handle, std::string_view file = {})
      SE_REQUIRES(g_main);

  // The decoder sound loads use (M8's, at the integration).
  void SetSoundDecoder(SoundDecoder* decoder) noexcept { decoder_ = decoder; }
  // Completions the registry did not submit (saves, flushes).
  // M10a: editor Play freezes gameplay assets (prefabs, scene chunks): a
  // change of one is kept and reloaded when the freeze ends. Visual assets
  // (textures, sounds) still reload; while frozen each such change is
  // reported to the swap listener (key, a hash of the file's size and
  // time), which logs it in the play session's SessionLog.
  void SetGameplayFrozen(bool frozen) SE_REQUIRES(g_main);
  [[nodiscard]] bool GameplayFrozen() const noexcept { return frozen_; }
  [[nodiscard]] std::size_t DeferredChanges() const noexcept {
    return deferred_.size();
  }
  using SwapListener =
      std::function<void(std::string_view key, std::uint64_t content_hash)>;
  void SetSwapListener(SwapListener listener) {
    swap_listener_ = std::move(listener);
  }
  // Also takes the FileChanges of foreign watch tags (kForeignWatchTag).
  void SetForeignHandler(std::function<void(LaneCompletion&&)> handler) {
    foreign_ = std::move(handler);
  }
  [[nodiscard]] const RegistryStats& Stats() const noexcept { return stats_; }
  [[nodiscard]] BackgroundLane& Lane() const noexcept { return *lane_; }

  // Releases every scope, the UI scope too (shutdown step 10).
  void ReleaseAll() SE_REQUIRES(g_main);

 private:
  enum class Kind : std::uint8_t { kTexture, kSound, kPrefab, kSceneChunk };

  struct Common {
    std::uint32_t generation = 1;
    bool alive = false;
    std::string key;
    std::uint32_t refs = 0;
    AssetState state = AssetState::kNone;
    // The newest load or reload in flight (0: none).
    Ticket ticket = 0;
    std::uint32_t version = 0;
    std::optional<Error> error;
    std::vector<std::string> watched;
  };

  struct TextureEntry {
    Common common;
    TextureSource source;
    TextureHandle gpu;
    std::uint32_t generation_at_upload = 0;
    bool reload_uploading = false;
    AtlasInfo atlas;
  };

  struct SoundEntry {
    Common common;
    std::shared_ptr<const PcmPayload> pcm;
  };

  struct PrefabEntry {
    Common common;
    std::shared_ptr<const PrefabPayload> payload;
  };

  struct ChunkEntry {
    Common common;
    std::shared_ptr<const SceneChunkPayload> payload;
  };

  struct Ref {
    Kind kind = Kind::kTexture;
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    friend bool operator==(const Ref&, const Ref&) = default;
  };

  struct Scope {
    std::string name;
    std::uint32_t generation = 1;
    bool alive = false;
    std::vector<Ref> refs;
  };

  struct Pending {
    Ref ref;
    bool reload = false;
  };

  template <class E>
  struct Store {
    std::vector<E> slots;
    std::vector<std::uint32_t> free;
    std::unordered_map<std::string, std::uint32_t> by_key;
  };

  [[nodiscard]] Scope* ScopeOf(AssetScope scope) noexcept;
  [[nodiscard]] const Scope* ScopeOf(AssetScope scope) const noexcept;
  [[nodiscard]] Common* CommonOf(const Ref& ref) noexcept;
  [[nodiscard]] const Common* CommonOf(const Ref& ref) const noexcept;
  template <class E>
  [[nodiscard]] static const E* Lookup(const Store<E>& store,
                                       std::uint32_t index,
                                       std::uint32_t generation) noexcept;
  template <class E>
  static std::uint32_t Acquire(Store<E>& store, const std::string& key,
                               bool& created);
  // Adds |ref| to |scope| once; true when it was new there.
  bool AddRef(Scope& scope, const Ref& ref);
  void Unref(const Ref& ref) SE_REQUIRES(g_main);
  void Remove(const Ref& ref) SE_REQUIRES(g_main);

  Ticket Submit(const Ref& ref, bool reload, std::unique_ptr<LaneJob> job)
      SE_REQUIRES(g_main);
  void SubmitTextureLoad(std::uint32_t slot, AtlasRequest::Mode mode,
                         std::string_view source_override, bool reload)
      SE_REQUIRES(g_main);
  void SubmitFileLoad(Kind kind, std::uint32_t slot, bool reload)
      SE_REQUIRES(g_main);
  void WatchPath(const Ref& ref, const std::string& path) SE_REQUIRES(g_main);
  [[nodiscard]] static std::uint64_t TagOf(const Ref& ref) noexcept;
  [[nodiscard]] static Ref RefOfTag(std::uint64_t tag) noexcept;

  void OnCompletion(LaneCompletion&& completion) SE_REQUIRES(g_main);
  void OnFileChange(const FileChange& change) SE_REQUIRES(g_main);
  void OnTexture(std::uint32_t slot, bool reload, LanePayload& payload)
      SE_REQUIRES(g_main);
  void UpdateResidency() SE_REQUIRES(g_main);
  void Failed(Common& common, bool reload, Error error);

  BackgroundLane* lane_;
  TextureUploadPort* textures_;
  AssetRegistryConfig config_;
  SoundDecoder* decoder_ = nullptr;
  std::function<void(LaneCompletion&&)> foreign_;
  bool frozen_ = false;
  std::vector<FileChange> deferred_;
  SwapListener swap_listener_;

  Store<TextureEntry> textures_store_;
  Store<SoundEntry> sounds_;
  Store<PrefabEntry> prefabs_;
  Store<ChunkEntry> chunks_;
  std::vector<Scope> scopes_;
  std::vector<std::uint32_t> free_scopes_;
  AssetScope ui_scope_;
  std::unordered_map<Ticket, Pending> pending_;
  // Every load the registry submitted and has not seen complete (pending_
  // forgets those of removed entries).
  std::unordered_set<Ticket> owned_;
  RegistryStats stats_;
};

}  // namespace se

#endif  // SE_ASSETS_ASSET_REGISTRY_H_
