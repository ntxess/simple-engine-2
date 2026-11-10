// Copyright 2026 The simple-engine-2 Authors.
//
// The AssetRegistry (docs/milestone-specs.md M9): handles and generations,
// scopes and refcounts, residency through the upload port, failures that
// settle, completions of released scopes dropped, hot reload, and the
// Prefab, SceneChunk and Sound stores. The GPU is a fake port here; the
// desktop test in tests/render_gpu drives the real one.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "assets/support/lane_test_support.h"
#include "assets/support/png.h"
#include "se_assets/asset_registry.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_assets/sound_decoder.h"
#include "se_core/handle.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::MainThreadScope;
using testing::ScratchDir;
using testing::TextBytes;
using testing::WriteBytes;

// GpuResources' contract, without a GPU: an upload becomes resident when
// the test says its fence signalled.
class FakeUploadPort final : public TextureUploadPort {
 public:
  struct Slot {
    std::string name;
    std::uint32_t generation = 1;
    bool alive = false;
    bool resident = false;
    std::uint32_t content = 0;
    std::uint32_t width = 0;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> pending;
    bool has_pending = false;
  };

  TextureHandle Register(std::string_view name) override {
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].alive && slots[i].name == name) return HandleOf(i);
    }
    Slot slot;
    slot.name = std::string(name);
    slot.alive = true;
    slots.push_back(std::move(slot));
    return HandleOf(slots.size() - 1);
  }
  std::expected<void, Error> Upload(TextureHandle handle,
                                    AtlasPage page) override {
    Slot* slot = Find(handle);
    if (slot == nullptr) return Fail(ErrorCode::kInvalidArgument, "stale");
    if (fail_uploads) return Fail(ErrorCode::kIo, "injected upload failure");
    slot->pending = std::move(page.pixels);
    slot->width = page.width;
    slot->has_pending = true;
    ++uploads;
    return {};
  }
  bool IsResident(TextureHandle handle) const override {
    const Slot* slot = Find(handle);
    return slot != nullptr && slot->resident;
  }
  std::uint32_t ContentGeneration(TextureHandle handle) const override {
    const Slot* slot = Find(handle);
    return slot != nullptr ? slot->content : 0;
  }
  void Remove(TextureHandle handle) override {
    if (Slot* slot = Find(handle)) {
      slot->alive = false;
      ++slot->generation;
      ++removed;
    }
  }

  // Every queued upload's fence signals: the slot swaps.
  void SignalFences() {
    for (Slot& slot : slots) {
      if (!slot.alive || !slot.has_pending) continue;
      slot.pixels = std::move(slot.pending);
      slot.has_pending = false;
      slot.resident = true;
      ++slot.content;
      ++retired;
    }
  }

  [[nodiscard]] const Slot* SlotOf(TextureHandle handle) const {
    return Find(handle);
  }

  std::vector<Slot> slots;
  int uploads = 0;
  int removed = 0;
  int retired = 0;
  bool fail_uploads = false;

 private:
  TextureHandle HandleOf(std::size_t i) const {
    TextureHandle handle;
    handle.index = static_cast<std::uint32_t>(i + 1);
    handle.generation = slots[i].generation;
    return handle;
  }
  Slot* Find(TextureHandle handle) {
    return const_cast<Slot*>(std::as_const(*this).Find(handle));
  }
  const Slot* Find(TextureHandle handle) const {
    if (handle.index == 0 || handle.index > slots.size()) return nullptr;
    const Slot& slot = slots[handle.index - 1];
    return slot.alive && slot.generation == handle.generation ? &slot : nullptr;
  }
};

void NoWake() noexcept {}

struct Fixture {
  explicit Fixture(bool hot_reload = false, std::uint64_t scan_ns = 0) {
    LaneConfig config;
    config.wake_main = &NoWake;
    config.scan_period_ns = scan_ns;
    auto started = BackgroundLane::Start(config);
    REQUIRE(started.has_value());
    lane = std::move(*started);
    AssetRegistryConfig registry_config;
    registry_config.hot_reload = hot_reload;
    registry = std::make_unique<AssetRegistry>(*lane, &port, registry_config);
  }

  // Frames until |done| or about 10 s.
  template <class F>
  bool Frames(F done) SE_REQUIRES(g_main) {
    for (int i = 0; i < 5000; ++i) {
      registry->BeginFrame();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  FakeUploadPort port;
  std::unique_ptr<BackgroundLane> lane;
  std::unique_ptr<AssetRegistry> registry;
};

AtlasPage Solid(std::uint32_t size, std::uint8_t value) {
  AtlasPage page;
  page.width = size;
  page.height = size;
  page.pixels.assign(page.Bytes(), value);
  return page;
}

// A PNG and its cooked page in |dir|.
TextureSource WriteTexture(const std::string& dir, const std::string& name,
                           std::uint8_t value) {
  const AtlasPage page = Solid(8, value);
  const std::vector<std::byte> png = testing::EncodeTestPng(8, 8, page.pixels);
  TextureSource source;
  source.name = name;
  source.source_path = fs::Join(dir, name + ".png");
  source.page_path = fs::Join(dir, name + ".seatlas");
  REQUIRE(WriteBytes(source.source_path, png));
  auto cooked = CookPng(png, name, source.source_path);
  REQUIRE(cooked.has_value());
  auto encoded = EncodeAtlasPage(*cooked);
  REQUIRE(encoded.has_value());
  REQUIRE(WriteBytes(source.page_path, *encoded));
  return source;
}

TEST_CASE("Textures become resident when their upload's fence signals",
          "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_resident");
  Fixture f;
  const TextureSource player = WriteTexture(dir, "player", 10);
  const TextureSource coin = WriteTexture(dir, "coin", 20);
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const auto a = f.registry->RequestTexture(scope, player);
  const auto b = f.registry->RequestTexture(scope, coin);
  CHECK_FALSE(a.IsNull());
  CHECK(f.registry->State(a) == AssetState::kLoading);
  CHECK_FALSE(f.registry->ScopeResident(scope));
  // The pages arrive and are queued for upload; nothing is resident until
  // the fences signal.
  REQUIRE(f.Frames([&] { return f.port.uploads == 2; }));
  CHECK(f.registry->State(a) == AssetState::kUploading);
  CHECK_FALSE(f.registry->ScopeResident(scope));
  f.port.SignalFences();
  f.registry->BeginFrame();
  CHECK(f.registry->State(a) == AssetState::kResident);
  CHECK(f.registry->State(b) == AssetState::kResident);
  CHECK(f.registry->ScopeResident(scope));
  REQUIRE(f.registry->Version(a) == 1);
  const auto* slot = f.port.SlotOf(f.registry->GpuTexture(a));
  REQUIRE(slot != nullptr);
  REQUIRE(slot->name == "player");
  REQUIRE(slot->pixels.front() == 10);
  const AtlasInfo* atlas = f.registry->Atlas(f.registry->AtlasOf(a));
  REQUIRE(atlas != nullptr);
  REQUIRE(atlas->width == 8);
  REQUIRE(atlas->regions.size() == 1);
  REQUIRE(atlas->regions[0].name == "player");
  REQUIRE(f.registry->Stats().recooked == 0);
}

TEST_CASE("Handles, generations and scope refcounts", "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_scopes");
  Fixture f;
  const TextureSource player = WriteTexture(dir, "player", 10);
  const AssetScope title = f.registry->CreateScope("title");
  const AssetScope game = f.registry->CreateScope("game");
  const auto in_title = f.registry->RequestTexture(title, player);
  const auto in_game = f.registry->RequestTexture(game, player);
  // One entry, referenced by two scopes; asking twice in a scope counts
  // once.
  CHECK(in_title == in_game);
  CHECK(f.registry->RequestTexture(game, player) == in_game);
  REQUIRE(f.registry->TextureCount() == 1);
  REQUIRE(f.Frames([&] { return f.port.uploads == 1; }));
  f.port.SignalFences();
  f.registry->BeginFrame();
  REQUIRE(f.registry->ScopeResident(title));

  f.registry->ReleaseScope(title);
  CHECK_FALSE(f.registry->ScopeAlive(title));
  CHECK(f.registry->State(in_game) == AssetState::kResident);
  REQUIRE(f.port.removed == 0);
  // A stale scope handle does nothing.
  f.registry->ReleaseScope(title);
  CHECK(f.registry->RequestTexture(title, player).IsNull());

  f.registry->ReleaseScope(game);
  CHECK(f.registry->State(in_game) == AssetState::kNone);
  CHECK(f.registry->GpuTexture(in_game).IsNull());
  REQUIRE(f.port.removed == 1);
  REQUIRE(f.registry->TextureCount() == 0);

  // The slot is reused with a new generation: the old handle stays stale.
  const AssetScope again = f.registry->CreateScope("again");
  const auto reloaded = f.registry->RequestTexture(again, player);
  CHECK(reloaded.index == in_game.index);
  CHECK(reloaded.generation != in_game.generation);
  CHECK(f.registry->State(in_game) == AssetState::kNone);
  CHECK(f.registry->State(reloaded) == AssetState::kLoading);
  // A scope handle reused by a new scope differs too.
  CHECK(again.index == game.index);
  CHECK(again.generation != game.generation);
  CHECK_FALSE(f.registry->ScopeAlive(game));
  CHECK(f.registry->ScopeAlive(again));
}

TEST_CASE("Completions for a released scope are dropped", "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_drop");
  Fixture f;
  const TextureSource player = WriteTexture(dir, "player", 10);
  // A slow disk: the load is still on the lane when the scope goes.
  fs::SetFsFaults(fs::FsFaults{.read_step_delay_ms = 50});
  const AssetScope scope = f.registry->CreateScope("short-lived");
  f.registry->RequestTexture(scope, player);
  f.registry->ReleaseScope(scope);
  REQUIRE(f.Frames([&] { return f.registry->Stats().dropped == 1; }));
  fs::ClearFsFaults();
  REQUIRE(f.port.uploads == 0);
  REQUIRE(f.registry->TextureCount() == 0);
}

TEST_CASE("A load that fails settles with its Error", "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_fail");
  Fixture f;
  TextureSource missing;
  missing.name = "missing";
  missing.page_path = fs::Join(dir, "missing.seatlas");
  missing.source_path = fs::Join(dir, "missing.png");
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const auto handle = f.registry->RequestTexture(scope, missing);
  REQUIRE(f.Frames(
      [&] { return f.registry->State(handle) == AssetState::kFailed; }));
  // A failed asset shows its placeholder; it does not hold the scope.
  CHECK(f.registry->ScopeResident(scope));
  REQUIRE(f.registry->Progress(scope).failed == 1);
  const Error* error = f.registry->LastError(handle);
  REQUIRE(error != nullptr);
  CHECK(error->code == ErrorCode::kNotFound);
  CHECK_FALSE(error->path.empty());
}

TEST_CASE("A stale page is re-cooked from its source on the lane",
          "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_recook");
  Fixture f;
  TextureSource source = WriteTexture(dir, "player", 10);
  // The PNG changed after the cook.
  REQUIRE(WriteBytes(source.source_path,
                     testing::EncodeTestPng(8, 8, Solid(8, 77).pixels)));
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const auto handle = f.registry->RequestTexture(scope, source);
  REQUIRE(f.Frames([&] { return f.port.uploads == 1; }));
  f.port.SignalFences();
  f.registry->BeginFrame();
  CHECK(f.registry->State(handle) == AssetState::kResident);
  REQUIRE(f.port.SlotOf(f.registry->GpuTexture(handle))->pixels.front() == 77);
  REQUIRE(f.registry->Stats().recooked == 1);
  CHECK(f.registry->Atlas(f.registry->AtlasOf(handle))->recooked);
}

TEST_CASE("Hot reload swaps a changed texture once it is resident",
          "[registry][hot_reload]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_hot");
  Fixture f(/*hot_reload=*/true, /*scan_ns=*/50'000'000);
  const TextureSource source = WriteTexture(dir, "player", 10);
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const auto handle = f.registry->RequestTexture(scope, source);
  REQUIRE(f.Frames([&] { return f.port.uploads == 1; }));
  f.port.SignalFences();
  f.registry->BeginFrame();
  REQUIRE(f.registry->State(handle) == AssetState::kResident);
  // Let the lane take its first look at the watched files.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  f.registry->BeginFrame();

  REQUIRE(WriteBytes(source.source_path,
                     testing::EncodeTestPng(8, 8, Solid(8, 200).pixels)));
  REQUIRE(f.Frames([&] { return f.port.uploads == 2; }));
  // Until the new upload is resident, the old texture draws.
  CHECK(f.registry->State(handle) == AssetState::kResident);
  REQUIRE(f.registry->Version(handle) == 1);
  REQUIRE(f.port.SlotOf(f.registry->GpuTexture(handle))->pixels.front() == 10);
  f.port.SignalFences();
  f.registry->BeginFrame();
  REQUIRE(f.registry->Version(handle) == 2);
  REQUIRE(f.port.SlotOf(f.registry->GpuTexture(handle))->pixels.front() == 200);
  REQUIRE(f.registry->Stats().file_changes >= 1);
  REQUIRE(f.registry->Stats().reloads_resident == 1);

  // A reload that does not decode keeps the loaded texture.
  REQUIRE(WriteBytes(source.source_path, TextBytes("not a png")));
  REQUIRE(f.Frames([&] { return f.registry->Stats().reload_failures == 1; }));
  CHECK(f.registry->State(handle) == AssetState::kResident);
  REQUIRE(f.registry->Version(handle) == 2);
  CHECK(f.registry->LastError(handle) != nullptr);
}

TEST_CASE("Prefabs and scene chunks are parsed on the lane", "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_json");
  const std::string prefab_path = fs::Join(dir, "enemy.prefab.json");
  const std::string chunk_path = fs::Join(dir, "scene.0.chunk.json");
  const std::string bad_path = fs::Join(dir, "bad.chunk.json");
  REQUIRE(WriteBytes(prefab_path, TextBytes(R"({
    "name": "enemy",
    "Transform": {"x": 1.5, "y": 2},
    "Stats": {"HP": 100, "guid": 18446744073709551615}
  })")));
  auto chunk = WriteSceneChunk(3, {R"({"guid":1,"x":2})", R"({"guid":2})"});
  REQUIRE(chunk.has_value());
  REQUIRE(WriteBytes(chunk_path, TextBytes(*chunk)));
  REQUIRE(WriteBytes(bad_path,
                     TextBytes("{\n  \"format\": \"se-scene-chunk-v1\",\n  "
                               "\"entities\": [ {\"a\": } ]\n}")));
  Fixture f;
  const AssetScope scope = f.registry->CreateScope("level");
  const auto prefab = f.registry->RequestPrefab(scope, prefab_path);
  const auto scene = f.registry->RequestSceneChunk(scope, chunk_path);
  const auto bad = f.registry->RequestSceneChunk(scope, bad_path);
  REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
  const auto parsed = f.registry->Prefab(prefab);
  REQUIRE(parsed != nullptr);
  REQUIRE(parsed->members.size() == 3);
  // File order, compact text, 64-bit integers exact.
  CHECK(parsed->members[0] == JsonMember{"name", "\"enemy\""});
  REQUIRE(parsed->members[1].key == "Transform");
  CHECK(parsed->members[2].json.find("18446744073709551615") !=
        std::string::npos);
  const auto entities = f.registry->SceneChunk(scene);
  REQUIRE(entities != nullptr);
  REQUIRE(entities->chunk == 3);
  REQUIRE(entities->entities.size() == 2);
  REQUIRE(entities->entities[0] == R"({"guid":1,"x":2})");
  CHECK(f.registry->State(bad) == AssetState::kFailed);
  REQUIRE(f.registry->Progress(scope).failed == 1);
}

// A decoder standing in for M8's (se_audio's miniaudio decoder): bytes are
// 8-bit samples, decoded 1000 frames per step.
class ByteDecoder final : public SoundDecoder {
 public:
  class Stream final : public SoundDecodeStream {
   public:
    explicit Stream(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
    std::expected<bool, Error> DecodeStep(PcmPayload& pcm) override {
      pcm.channels = 1;
      pcm.sample_rate = 48000;
      const std::size_t end = std::min(bytes_.size(), next_ + 1000);
      for (; next_ < end; ++next_) {
        pcm.samples.push_back(
            static_cast<float>(std::to_integer<int>(bytes_[next_])) / 255.0F);
      }
      pcm.frames = pcm.samples.size();
      return next_ == bytes_.size();
    }

   private:
    std::vector<std::byte> bytes_;
    std::size_t next_ = 0;
  };

  std::expected<std::unique_ptr<SoundDecodeStream>, Error> Open(
      std::vector<std::byte> encoded, std::string_view /*path*/) override {
    return std::make_unique<Stream>(std::move(encoded));
  }
};

TEST_CASE("Sounds decode on the lane through the integration's decoder",
          "[registry][sound]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_sound");
  const std::string path = fs::Join(dir, "hit.wav");
  REQUIRE(WriteBytes(path, testing::PatternBytes(4500, 5)));
  Fixture f(/*hot_reload=*/true, /*scan_ns=*/50'000'000);
  const AssetScope scope = f.registry->CreateScope("level");

  SECTION("without a decoder the sound fails, naming the integration") {
    const auto sound = f.registry->RequestSound(scope, path);
    REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
    CHECK(f.registry->State(sound) == AssetState::kFailed);
    REQUIRE(f.registry->LastError(sound) != nullptr);
    CHECK(f.registry->LastError(sound)->code == ErrorCode::kUnsupported);
  }
  SECTION("with one, new PCM goes to new voices only") {
    ByteDecoder decoder;
    f.registry->SetSoundDecoder(&decoder);
    const auto sound = f.registry->RequestSound(scope, path);
    REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
    const std::shared_ptr<const PcmPayload> voice = f.registry->Sound(sound);
    REQUIRE(voice != nullptr);
    REQUIRE(voice->frames == 4500);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    f.registry->BeginFrame();
    REQUIRE(WriteBytes(path, testing::PatternBytes(900, 6)));
    REQUIRE(f.Frames([&] { return f.registry->Version(sound) == 2; }));
    // A playing voice keeps its PCM; new voices get the new one.
    REQUIRE(voice->frames == 4500);
    REQUIRE(f.registry->Sound(sound)->frames == 900);
    // Releasing the scope frees the registry's reference only.
    f.registry->ReleaseScope(scope);
    CHECK(f.registry->Sound(sound) == nullptr);
    REQUIRE(voice->frames == 4500);
  }
}

TEST_CASE("Foreign completions reach their handler", "[registry]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("registry_foreign");
  Fixture f;
  std::vector<Ticket> foreign;
  f.registry->SetForeignHandler([&foreign](LaneCompletion&& completion) {
    const LaneCompletion done = std::move(completion);
    foreign.push_back(done.ticket);
  });
  SaveRequest save;
  save.path = fs::Join(dir, "settings.json");
  save.bytes = TextBytes("{}");
  const Ticket ticket = SubmitSave(*f.lane, std::move(save));
  REQUIRE(f.Frames([&] { return !foreign.empty(); }));
  CHECK(foreign.front() == ticket);
  REQUIRE(f.registry->Stats().dropped == 0);
}

}  // namespace
}  // namespace se
