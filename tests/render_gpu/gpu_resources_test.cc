// Copyright 2026 The simple-engine-2 Authors.
//
// GpuResources on a device without a window (label gpu; once per driver):
// the magenta fallback, residency on the upload fence, the 8 MB budget,
// superseded uploads, the retire queue and the live-object counts, and the
// texture table's missing files.

#include "se_render/gpu_resources.h"

#include <SDL3/SDL_gpu.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render_gpu/support/gpu_fixture.h"
#include "render_gpu/support/image_compare.h"
#include "se_platform/fs.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_components.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::GpuFixture;

std::shared_ptr<const TexturePayload> Solid(std::uint32_t width,
                                            std::uint32_t height,
                                            std::uint8_t r) {
  auto payload = std::make_shared<TexturePayload>();
  payload->width = width;
  payload->height = height;
  payload->pixels.assign(payload->Bytes(), r);
  return payload;
}

// One upload-only submit, as a frame that does not render sends it.
void SubmitUploads(GpuResources& resources) SE_REQUIRES(g_main) {
  resources.PlanUploads();
  SDL_GPUCommandBuffer* commands =
      SDL_AcquireGPUCommandBuffer(resources.Native());
  REQUIRE(commands != nullptr);
  REQUIRE(resources.RecordUploads(commands, false));
  resources.UploadsSubmitted(
      SDL_SubmitGPUCommandBufferAndAcquireFence(commands));
  resources.RunRetireQueue();
}

TEST_CASE("Null, unknown and removed handles resolve to magenta and count",
          "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  GpuResources& resources = gpu.Resources();
  const GpuResourceStats before = resources.Stats();
  const ResolvedTexture null_handle = resources.Resolve(TextureHandle{});
  CHECK(null_handle.fallback);
  CHECK(null_handle.texture != nullptr);
  REQUIRE(null_handle.width == 1);
  CHECK(resources.Resolve(TextureHandle{999, 1}).fallback);
  CHECK(resources.Find(TextureKeyOf("never-added")).IsNull());

  const TextureHandle handle = resources.AddTexture("removed");
  CHECK(resources.Find(TextureKeyOf("removed")) == handle);
  // Not resident yet: magenta, counted apart from stale handles.
  CHECK(resources.Resolve(handle).fallback);
  resources.Remove(handle);
  CHECK(resources.Resolve(handle).fallback);
  CHECK(resources.Find(TextureKeyOf("removed")).IsNull());
  const GpuResourceStats& after = resources.Stats();
  REQUIRE(after.stale_resolves - before.stale_resolves == 3);
  REQUIRE(after.unresident_resolves - before.unresident_resolves == 1);
  // The slot is reused under a new generation; the old handle stays stale.
  const TextureHandle reused = resources.AddTexture("reused");
  CHECK(reused.index == handle.index);
  CHECK(reused.generation != handle.generation);
  CHECK(resources.NameOf(handle).empty());
  REQUIRE(resources.NameOf(reused) == "reused");
}

TEST_CASE(
    "An upload becomes resident when its fence signals; the handle "
    "stays valid",
    "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  GpuResources& resources = gpu.Resources();
  const TextureHandle handle = resources.AddTexture("upload");
  REQUIRE(resources.QueueUpload(handle, Solid(8, 4, 10)));
  CHECK(resources.UploadsPending());
  CHECK_FALSE(resources.IsResident(handle));
  const std::uint32_t generation = resources.ContentGeneration(handle);
  SubmitUploads(resources);
  CHECK_FALSE(resources.UploadsPending());
  CHECK(resources.UploadsInFlight());
  // Resident only once CollectUploads sees the fence.
  CHECK_FALSE(resources.IsResident(handle));
  gpu.Device().WaitForIdle();
  resources.CollectUploads();
  REQUIRE(resources.IsResident(handle));
  CHECK(resources.ContentGeneration(handle) == generation + 1);
  const ResolvedTexture first = resources.Resolve(handle);
  CHECK_FALSE(first.fallback);
  REQUIRE(first.width == 8);
  REQUIRE(first.height == 4);

  // A reload at another size: the same handle, the old texture retired.
  const std::uint64_t retired = resources.Stats().retired;
  REQUIRE(resources.QueueUpload(handle, Solid(16, 16, 20)));
  SubmitUploads(resources);
  REQUIRE(resources.Resolve(handle).width == 8);
  gpu.Device().WaitForIdle();
  resources.CollectUploads();
  REQUIRE(resources.Resolve(handle).width == 16);
  CHECK(resources.ContentGeneration(handle) == generation + 2);
  CHECK(resources.Stats().retired == retired + 1);
  REQUIRE(resources.RetireQueueSize() == 1);
  resources.RunRetireQueue();
}

TEST_CASE(
    "A texture larger than the budget fills over several submits and "
    "is never resident half-uploaded",
    "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  GpuResources& resources = gpu.Resources();
  // 2048 x 2048 RGBA8 is 16 MB: two 8 MB submits at least.
  const TextureHandle handle = resources.AddTexture("large");
  REQUIRE(resources.QueueUpload(handle, Solid(2048, 2048, 30)));
  const std::uint64_t batches = resources.Stats().upload_batches;
  CHECK(resources.PlanUploads() <= GpuResources::kUploadBudgetBytes);
  resources.DropUploadPlan();
  int submits = 0;
  while (resources.UploadsPending() && submits < 8) {
    SubmitUploads(resources);
    ++submits;
    gpu.Device().WaitForIdle();
    resources.CollectUploads();
    if (resources.UploadsPending()) CHECK_FALSE(resources.IsResident(handle));
  }
  REQUIRE(submits >= 2);
  CHECK(resources.Stats().upload_batches - batches ==
        static_cast<std::uint64_t>(submits));
  CHECK(resources.IsResident(handle));
  CHECK(resources.Stats().upload_bytes >= std::uint64_t{2048} * 2048 * 4);
}

TEST_CASE("A newer upload supersedes an older one of the same slot", "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  GpuResources& resources = gpu.Resources();
  const TextureHandle handle = resources.AddTexture("twice");
  REQUIRE(resources.QueueUpload(handle, Solid(4, 4, 1)));
  REQUIRE(resources.QueueUpload(handle, Solid(2, 2, 2)));
  REQUIRE(resources.Stats().uploads_superseded >= 1);
  REQUIRE(resources.FlushUploads());
  REQUIRE(resources.Resolve(handle).width == 2);
}

TEST_CASE("Retired objects live until the retire queue runs after a submit",
          "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  GpuResources& resources = gpu.Resources();
  const std::uint64_t textures = resources.LiveObjects(GpuObjectKind::kTexture);
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = GpuResources::kTextureFormat;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = 4;
  info.height = 4;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  SDL_GPUTexture* texture = resources.CreateGpuTexture(info);
  REQUIRE(texture != nullptr);
  CHECK(resources.LiveObjects(GpuObjectKind::kTexture) == textures + 1);
  resources.Retire(texture);
  CHECK(resources.LiveObjects(GpuObjectKind::kTexture) == textures + 1);
  REQUIRE(resources.RetireQueueSize() == 1);
  resources.RunRetireQueue();
  CHECK(resources.LiveObjects(GpuObjectKind::kTexture) == textures);
  REQUIRE(resources.RetireQueueSize() == 0);
}

TEST_CASE(
    "A texture whose file is missing is an Error naming the path and "
    "draws magenta",
    "[gpu]") {
  const testing::MainThreadScope main;
  GpuFixture gpu;
  if (!gpu.Available()) SKIP(gpu.SkipReason());
  const std::string dir = testing::ImageOutDir() + "/table";
  REQUIRE(fs::CreateDirectories(dir));
  const std::string table = R"({"player": "player.png", "gone": "gone.png"})";
  REQUIRE(fs::WriteFile(dir + "/texture.json",
                        std::as_bytes(std::span(table.data(), table.size()))));
  // A real PNG for "player".
  auto player = fs::ReadFile(testing::RepoRoot() + "/assets/player/player.png");
  REQUIRE(player);
  REQUIRE(fs::WriteFile(dir + "/player.png", *player));
  GpuResources& resources = gpu.Resources();
  auto report = LoadTextureTable(resources, dir, "texture.json");
  REQUIRE(report);
  REQUIRE(report->loaded == 1);
  REQUIRE(report->errors.size() == 1);
  CHECK(report->errors[0].code == ErrorCode::kNotFound);
  CHECK(report->errors[0].path == dir + "/gone.png");
  CHECK(report->errors[0].message.find("'gone'") != std::string::npos);
  REQUIRE(resources.FlushUploads());
  CHECK_FALSE(
      resources.Resolve(resources.Find(TextureKeyOf("player"))).fallback);
  CHECK(resources.Resolve(resources.Find(TextureKeyOf("gone"))).fallback);
  // An unreadable table fails the call.
  CHECK_FALSE(LoadTextureTable(resources, dir, "missing.json"));
}

}  // namespace
}  // namespace se
