// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/gpu_resources.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_platform/driver_wait.h"
#include "se_render/gpu_device.h"
#include "se_render_core/render_components.h"
#include "src/gpu_assert.h"

namespace se {
namespace {

// D3D12 places texture copies at 512-byte offsets; aligned chunks spare
// SDL a staging copy.
constexpr std::uint32_t kChunkAlignment = 512;
constexpr std::string_view kMagentaName = "<magenta>";
// Rounds of FlushUploads before it gives up (a 64 MB texture needs 8).
constexpr int kMaxFlushRounds = 1024;

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

std::string SdlError(std::string_view what) {
  std::string message(what);
  message += ": ";
  message += SDL_GetError();
  return message;
}

}  // namespace

std::expected<std::unique_ptr<GpuResources>, Error> GpuResources::Create(
    GpuDevice& device) {
  SE_ASSERT_GPU_THREAD();
  std::unique_ptr<GpuResources> resources(new GpuResources(device));
  if (auto made = resources->CreateDeviceObjects(); !made) {
    return std::unexpected(std::move(made.error()));
  }
  return resources;
}

std::expected<void, Error> GpuResources::CreateDeviceObjects() {
  SDL_GPUTransferBufferCreateInfo upload{};
  upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  upload.size = static_cast<std::uint32_t>(kUploadBudgetBytes);
  upload_buffer_ = CreateTransferBuffer(upload);
  if (upload_buffer_ == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                SdlError("SDL_CreateGPUTransferBuffer(uploads)"));
  }
  if (magenta_.IsNull()) {
    magenta_ = AddTexture(kMagentaName);
    auto payload = std::make_shared<TexturePayload>();
    payload->width = 1;
    payload->height = 1;
    payload->pixels = {255, 0, 255, 255};
    if (auto queued = QueueUpload(magenta_, std::move(payload)); !queued) {
      return queued;
    }
  }
  return FlushUploads();
}

GpuResources::~GpuResources() {
  SE_ASSERT_GPU_THREAD();
  ReleaseRemaining();
}

std::uint64_t GpuResources::ReleaseRemaining() noexcept {
  SE_ASSERT_GPU_THREAD();
  RunRetireQueue();
  for (Slot& slot : slots_) {
    if (slot.resident != nullptr) {
      ReleaseNow(GpuObjectKind::kTexture,
                 std::exchange(slot.resident, nullptr));
    }
  }
  for (PendingUpload& entry : pending_) {
    ReleaseNow(GpuObjectKind::kTexture, std::exchange(entry.texture, nullptr));
  }
  for (std::size_t i = 0; i < in_flight_count_; ++i) {
    UploadBatch& batch = in_flight_[i];
    for (Completion& completion : batch.completions) {
      ReleaseNow(GpuObjectKind::kTexture,
                 std::exchange(completion.texture, nullptr));
    }
    ReleaseFence(std::exchange(batch.fence, nullptr));
  }
  in_flight_count_ = 0;
  if (upload_buffer_ != nullptr) {
    ReleaseNow(GpuObjectKind::kTransferBuffer,
               std::exchange(upload_buffer_, nullptr));
  }
  return LiveObjects();
}

// --- Slots ---

GpuResources::Slot* GpuResources::SlotOf(TextureHandle handle) noexcept {
  if (handle.index == 0 || handle.index > slots_.size()) return nullptr;
  Slot& slot = slots_[handle.index - 1];
  if (!slot.alive || slot.generation != handle.generation) return nullptr;
  return &slot;
}

const GpuResources::Slot* GpuResources::SlotOf(
    TextureHandle handle) const noexcept {
  if (handle.index == 0 || handle.index > slots_.size()) return nullptr;
  const Slot& slot = slots_[handle.index - 1];
  if (!slot.alive || slot.generation != handle.generation) return nullptr;
  return &slot;
}

std::uint32_t GpuResources::NewSlot() {
  if (!free_slots_.empty()) {
    const std::uint32_t index = free_slots_.back();
    free_slots_.pop_back();
    return index;
  }
  slots_.emplace_back();
  return static_cast<std::uint32_t>(slots_.size() - 1);
}

TextureHandle GpuResources::AddTexture(std::string_view name) {
  SE_ASSERT_GPU_THREAD();
  const std::uint32_t key = TextureKeyOf(name);
  if (const auto found = by_key_.find(key); found != by_key_.end()) {
    const Slot& existing = slots_[found->second];
    if (existing.name != name) {
      SE_LOG_WARN("texture '{}' has the key of '{}' ({:#010x}); both name one",
                  name, existing.name, key);
    }
    return TextureHandle{found->second + 1, existing.generation};
  }
  const std::uint32_t index = NewSlot();
  Slot& slot = slots_[index];
  slot.name = std::string(name);
  slot.key = key;
  slot.alive = true;
  slot.kind = SlotKind::kUploaded;
  slot.format = kTextureFormat;
  by_key_.emplace(key, index);
  return TextureHandle{index + 1, slot.generation};
}

TextureHandle GpuResources::Find(std::uint32_t key) const noexcept {
  const auto found = by_key_.find(key);
  if (found == by_key_.end()) return {};
  return TextureHandle{found->second + 1, slots_[found->second].generation};
}

SDL_GPUTexture* GpuResources::NewSlotTexture(const Slot& slot,
                                             std::uint32_t width,
                                             std::uint32_t height) noexcept {
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = slot.format;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  if (slot.kind == SlotKind::kTarget) {
    info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  }
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  return CreateGpuTexture(info);
}

std::expected<TextureHandle, Error> GpuResources::AddTarget(
    std::string_view name, std::uint32_t width, std::uint32_t height,
    SDL_GPUTextureFormat format) {
  SE_ASSERT_GPU_THREAD();
  if (width == 0 || height == 0) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("render target '{}' has no size", name));
  }
  const std::uint32_t index = NewSlot();
  Slot& slot = slots_[index];
  slot.name = std::string(name);
  slot.key = 0;
  slot.alive = true;
  slot.kind = SlotKind::kTarget;
  slot.format = format;
  slot.resident = NewSlotTexture(slot, width, height);
  if (slot.resident == nullptr) {
    slot.alive = false;
    free_slots_.push_back(index);
    return Fail(ErrorCode::kUnsupported,
                SdlError(std::format("SDL_CreateGPUTexture(target '{}' {}x{})",
                                     name, width, height)));
  }
  slot.width = width;
  slot.height = height;
  ++slot.content_generation;
  return TextureHandle{index + 1, slot.generation};
}

std::expected<void, Error> GpuResources::ResizeTarget(TextureHandle handle,
                                                      std::uint32_t width,
                                                      std::uint32_t height) {
  SE_ASSERT_GPU_THREAD();
  Slot* slot = SlotOf(handle);
  if (slot == nullptr || slot->kind != SlotKind::kTarget) {
    return Fail(ErrorCode::kInvalidArgument,
                "ResizeTarget needs a render target handle");
  }
  if (width == 0 || height == 0) {
    return Fail(ErrorCode::kInvalidArgument, "a render target needs a size");
  }
  if (slot->width == width && slot->height == height &&
      slot->resident != nullptr) {
    return {};
  }
  SDL_GPUTexture* texture = NewSlotTexture(*slot, width, height);
  if (texture == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                SdlError(std::format("SDL_CreateGPUTexture(target '{}' {}x{})",
                                     slot->name, width, height)));
  }
  Retire(std::exchange(slot->resident, texture));
  slot->width = width;
  slot->height = height;
  ++slot->content_generation;
  return {};
}

void GpuResources::Remove(TextureHandle handle) {
  SE_ASSERT_GPU_THREAD();
  Slot* slot = SlotOf(handle);
  if (slot == nullptr || handle == magenta_) return;
  Retire(std::exchange(slot->resident, nullptr));
  if (slot->kind == SlotKind::kUploaded) by_key_.erase(slot->key);
  slot->alive = false;
  ++slot->generation;
  slot->payload.reset();
  slot->name.clear();
  free_slots_.push_back(handle.index - 1);
}

ResolvedTexture GpuResources::Resolve(TextureHandle handle) noexcept {
  ++stats_.resolves;
  const Slot* slot = SlotOf(handle);
  if (slot != nullptr && slot->resident != nullptr) {
    return ResolvedTexture{slot->resident, slot->width, slot->height, false};
  }
  if (slot == nullptr) {
    ++stats_.stale_resolves;
  } else {
    ++stats_.unresident_resolves;
  }
  const Slot& magenta = slots_[magenta_.index - 1];
  SE_ASSERT_MSG(magenta.resident != nullptr,
                "Resolve while the GPU objects are released");
  return ResolvedTexture{magenta.resident, 1, 1, true};
}

bool GpuResources::IsResident(TextureHandle handle) const noexcept {
  const Slot* slot = SlotOf(handle);
  return slot != nullptr && slot->resident != nullptr;
}

std::string_view GpuResources::NameOf(TextureHandle handle) const noexcept {
  const Slot* slot = SlotOf(handle);
  return slot != nullptr ? std::string_view(slot->name) : std::string_view();
}

std::uint32_t GpuResources::ContentGeneration(
    TextureHandle handle) const noexcept {
  const Slot* slot = SlotOf(handle);
  return slot != nullptr ? slot->content_generation : 0;
}

// --- Uploads ---

std::expected<void, Error> GpuResources::QueueUpload(
    TextureHandle handle, std::shared_ptr<const TexturePayload> payload) {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(plan_.chunks.empty(), "QueueUpload inside a frame's uploads");
  Slot* slot = SlotOf(handle);
  if (slot == nullptr || slot->kind != SlotKind::kUploaded) {
    return Fail(ErrorCode::kInvalidArgument,
                "QueueUpload needs a texture handle that is not stale");
  }
  if (payload == nullptr || payload->width == 0 || payload->height == 0 ||
      payload->pixels.size() != payload->Bytes()) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("texture '{}': the payload is empty or its size "
                            "does not match its pixels",
                            slot->name));
  }
  SDL_GPUTexture* texture =
      NewSlotTexture(*slot, payload->width, payload->height);
  if (texture == nullptr) {
    return Fail(
        ErrorCode::kUnsupported,
        SdlError(std::format("SDL_CreateGPUTexture('{}' {}x{})", slot->name,
                             payload->width, payload->height)));
  }
  // A queued upload of this slot that has not started copying is replaced.
  const std::uint32_t slot_index = handle.index - 1;
  std::erase_if(pending_, [&](PendingUpload& entry) {
    AssertMainThread();
    if (entry.slot != slot_index || entry.next_row != 0) return false;
    Retire(std::exchange(entry.texture, nullptr));
    ++stats_.uploads_superseded;
    return true;
  });
  PendingUpload entry;
  entry.slot = slot_index;
  entry.slot_generation = slot->generation;
  entry.serial = next_serial_++;
  entry.payload = payload;
  entry.texture = texture;
  pending_.push_back(std::move(entry));
  slot->latest_upload = pending_.back().serial;
  slot->payload = std::move(payload);
  ++stats_.uploads_queued;
  return {};
}

void GpuResources::CollectUploads() {
  SE_ASSERT_GPU_THREAD();
  std::size_t kept = 0;
  for (std::size_t i = 0; i < in_flight_count_; ++i) {
    UploadBatch& batch = in_flight_[i];
    if (!SDL_QueryGPUFence(Native(), batch.fence)) {
      if (kept != i) in_flight_[kept] = std::move(batch);
      ++kept;
      continue;
    }
    for (Completion& completion : batch.completions) {
      Slot& slot = slots_[completion.slot];
      const bool current = slot.alive &&
                           slot.generation == completion.slot_generation &&
                           slot.latest_upload == completion.serial;
      if (current) {
        Retire(std::exchange(slot.resident, completion.texture));
        slot.width = completion.width;
        slot.height = completion.height;
        ++slot.content_generation;
        ++stats_.uploads_resident;
      } else {
        Retire(completion.texture);
        ++stats_.uploads_superseded;
      }
      completion.texture = nullptr;
    }
    batch.completions.clear();
    ReleaseFence(std::exchange(batch.fence, nullptr));
  }
  in_flight_count_ = kept;
}

std::uint64_t GpuResources::PlanUploads() {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(plan_.chunks.empty(), "PlanUploads twice in one frame");
  plan_.bytes = 0;
  plan_.recorded = false;
  plan_.completing = 0;
  // Uploads of removed slots, and uploads a newer one of their slot
  // replaces, are dropped first (copies already submitted to the texture
  // only delay its release).
  std::erase_if(pending_, [&](PendingUpload& entry) {
    AssertMainThread();
    const Slot& slot = slots_[entry.slot];
    if (slot.alive && slot.generation == entry.slot_generation &&
        slot.latest_upload == entry.serial) {
      return false;
    }
    Retire(std::exchange(entry.texture, nullptr));
    ++stats_.uploads_superseded;
    return true;
  });
  std::uint32_t offset = 0;
  const auto capacity = static_cast<std::uint32_t>(kUploadBudgetBytes);
  // Every batch slot busy: nothing this frame.
  const std::size_t plannable =
      in_flight_count_ < kMaxInFlight ? pending_.size() : 0;
  for (std::size_t i = 0; i < plannable; ++i) {
    const PendingUpload& entry = pending_[i];
    const std::uint32_t row_bytes = entry.payload->width * 4;
    const std::uint32_t rows_left = entry.payload->height - entry.next_row;
    const std::uint32_t room = capacity > offset ? capacity - offset : 0;
    const std::uint32_t rows = std::min(rows_left, room / row_bytes);
    if (rows == 0) break;
    plan_.chunks.push_back(Chunk{i, entry.next_row, rows, offset});
    plan_.bytes += std::uint64_t{rows} * row_bytes;
    offset = AlignUp(offset + rows * row_bytes, kChunkAlignment);
    if (rows < rows_left) break;
    ++plan_.completing;
  }
  // UploadsSubmitted runs in the noexcept recording phase: size now.
  plan_.completions.assign(plan_.completing, Completion{});
  return plan_.bytes;
}

bool GpuResources::RecordUploads(SDL_GPUCommandBuffer* commands,
                                 bool fail_copy_pass) noexcept {
  SE_ASSERT_GPU_THREAD();
  if (plan_.chunks.empty() || commands == nullptr) return false;
  auto* mapped = static_cast<std::uint8_t*>(
      SDL_MapGPUTransferBuffer(Native(), upload_buffer_, true));
  if (mapped == nullptr) {
    ++stats_.upload_failures;
    return false;
  }
  for (const Chunk& chunk : plan_.chunks) {
    const PendingUpload& entry = pending_[chunk.pending];
    const std::size_t row_bytes = std::size_t{entry.payload->width} * 4;
    std::memcpy(mapped + chunk.offset,
                entry.payload->pixels.data() + chunk.y * row_bytes,
                chunk.rows * row_bytes);
  }
  SDL_UnmapGPUTransferBuffer(Native(), upload_buffer_);
  SDL_GPUCopyPass* pass =
      fail_copy_pass ? nullptr : SDL_BeginGPUCopyPass(commands);
  if (pass == nullptr) {
    ++stats_.upload_failures;
    return false;
  }
  for (const Chunk& chunk : plan_.chunks) {
    const PendingUpload& entry = pending_[chunk.pending];
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload_buffer_;
    source.offset = chunk.offset;
    source.pixels_per_row = entry.payload->width;
    source.rows_per_layer = chunk.rows;
    SDL_GPUTextureRegion region{};
    region.texture = entry.texture;
    region.y = chunk.y;
    region.w = entry.payload->width;
    region.h = chunk.rows;
    region.d = 1;
    SDL_UploadToGPUTexture(pass, &source, &region, false);
  }
  SDL_EndGPUCopyPass(pass);
  plan_.recorded = true;
  return true;
}

void GpuResources::UploadsSubmitted(SDL_GPUFence* fence) noexcept {
  SE_ASSERT_GPU_THREAD();
  if (fence != nullptr) CountFence(fence);
  if (!plan_.recorded || fence == nullptr) {
    if (plan_.recorded) ++stats_.upload_failures;
    if (fence != nullptr) ReleaseFence(fence);
    DropUploadPlan();
    return;
  }
  for (const Chunk& chunk : plan_.chunks) {
    pending_[chunk.pending].next_row += chunk.rows;
  }
  // Fully planned uploads form a prefix of the queue: each chunk takes all
  // the rows that fit, and planning stops at the first that does not.
  for (std::size_t i = 0; i < plan_.completing; ++i) {
    PendingUpload& entry = pending_.front();
    SE_ASSERT(entry.next_row == entry.payload->height);
    plan_.completions[i] =
        Completion{entry.slot,    entry.slot_generation, entry.serial,
                   entry.texture, entry.payload->width,  entry.payload->height};
    pending_.pop_front();
  }
  SE_ASSERT(in_flight_count_ < kMaxInFlight);
  UploadBatch& batch = in_flight_[in_flight_count_++];
  batch.fence = fence;
  batch.completions.swap(plan_.completions);
  ++stats_.upload_batches;
  stats_.upload_bytes += plan_.bytes;
  DropUploadPlan();
}

std::expected<void, Error> GpuResources::FlushUploads() {
  SE_ASSERT_GPU_THREAD();
  for (int round = 0; round < kMaxFlushRounds; ++round) {
    CollectUploads();
    if (pending_.empty() && in_flight_count_ == 0) return {};
    if (!pending_.empty()) {
      PlanUploads();
      SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(Native());
      if (commands == nullptr) {
        DropUploadPlan();
        return Fail(ErrorCode::kUnsupported,
                    SdlError("SDL_AcquireGPUCommandBuffer(uploads)"));
      }
      RecordUploads(commands, false);
      UploadsSubmitted(SDL_SubmitGPUCommandBufferAndAcquireFence(commands));
      RunRetireQueue();
    }
    std::vector<SDL_GPUFence*> fences;
    fences.reserve(in_flight_count_);
    for (std::size_t i = 0; i < in_flight_count_; ++i) {
      fences.push_back(in_flight_[i].fence);
    }
    bool waited = true;
    if (!fences.empty()) {
      // A fence wait: the watchdog's GPU or compositor stall label (M12).
      const DriverWaitScope in_driver;
      waited = SDL_WaitForGPUFences(Native(), true, fences.data(),
                                    static_cast<std::uint32_t>(fences.size()));
    }
    if (!waited) {
      return Fail(ErrorCode::kUnsupported,
                  SdlError("SDL_WaitForGPUFences(uploads)"));
    }
  }
  return Fail(ErrorCode::kUnsupported, "FlushUploads made no progress");
}

// --- Retire queue and counted objects ---

void GpuResources::RetireObject(GpuObjectKind kind, void* object) {
  if (object == nullptr) return;
  retired_.push_back(RetiredObject{kind, object});
  ++stats_.retired;
}

void GpuResources::Retire(SDL_GPUTexture* texture) {
  RetireObject(GpuObjectKind::kTexture, texture);
}
void GpuResources::Retire(SDL_GPUBuffer* buffer) {
  RetireObject(GpuObjectKind::kBuffer, buffer);
}
void GpuResources::Retire(SDL_GPUTransferBuffer* buffer) {
  RetireObject(GpuObjectKind::kTransferBuffer, buffer);
}
void GpuResources::Retire(SDL_GPUSampler* sampler) {
  RetireObject(GpuObjectKind::kSampler, sampler);
}
void GpuResources::Retire(SDL_GPUGraphicsPipeline* pipeline) {
  RetireObject(GpuObjectKind::kGraphicsPipeline, pipeline);
}

void GpuResources::RunRetireQueue() noexcept {
  SE_ASSERT_GPU_THREAD();
  for (const RetiredObject& retired : retired_) {
    ReleaseNow(retired.kind, retired.object);
  }
  retired_.clear();
}

void GpuResources::ReleaseNow(GpuObjectKind kind, void* object) noexcept {
  if (object == nullptr) return;
  SDL_GPUDevice* device = Native();
  switch (kind) {
    case GpuObjectKind::kTexture:
      SDL_ReleaseGPUTexture(device, static_cast<SDL_GPUTexture*>(object));
      break;
    case GpuObjectKind::kBuffer:
      SDL_ReleaseGPUBuffer(device, static_cast<SDL_GPUBuffer*>(object));
      break;
    case GpuObjectKind::kTransferBuffer:
      SDL_ReleaseGPUTransferBuffer(device,
                                   static_cast<SDL_GPUTransferBuffer*>(object));
      break;
    case GpuObjectKind::kSampler:
      SDL_ReleaseGPUSampler(device, static_cast<SDL_GPUSampler*>(object));
      break;
    case GpuObjectKind::kGraphicsPipeline:
      SDL_ReleaseGPUGraphicsPipeline(
          device, static_cast<SDL_GPUGraphicsPipeline*>(object));
      break;
    case GpuObjectKind::kFence:
      SDL_ReleaseGPUFence(device, static_cast<SDL_GPUFence*>(object));
      break;
  }
  std::uint64_t& live = live_[static_cast<std::size_t>(kind)];
  SE_ASSERT_MSG(live > 0, "a GPU object released twice or never counted");
  --live;
  ++stats_.released;
}

void GpuResources::ReleaseFence(SDL_GPUFence* fence) noexcept {
  ReleaseNow(GpuObjectKind::kFence, fence);
}

void GpuResources::Created(GpuObjectKind kind) noexcept {
  ++live_[static_cast<std::size_t>(kind)];
  ++stats_.created;
}

void GpuResources::CountFence(SDL_GPUFence* fence) noexcept {
  if (fence != nullptr) Created(GpuObjectKind::kFence);
}

SDL_GPUTexture* GpuResources::CreateGpuTexture(
    const SDL_GPUTextureCreateInfo& info) noexcept {
  SE_ASSERT_GPU_THREAD();
  SDL_GPUTexture* texture = SDL_CreateGPUTexture(Native(), &info);
  if (texture != nullptr) Created(GpuObjectKind::kTexture);
  return texture;
}

SDL_GPUBuffer* GpuResources::CreateGpuBuffer(
    const SDL_GPUBufferCreateInfo& info) noexcept {
  SE_ASSERT_GPU_THREAD();
  SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(Native(), &info);
  if (buffer != nullptr) Created(GpuObjectKind::kBuffer);
  return buffer;
}

SDL_GPUTransferBuffer* GpuResources::CreateTransferBuffer(
    const SDL_GPUTransferBufferCreateInfo& info) noexcept {
  SE_ASSERT_GPU_THREAD();
  SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(Native(), &info);
  if (buffer != nullptr) Created(GpuObjectKind::kTransferBuffer);
  return buffer;
}

SDL_GPUSampler* GpuResources::CreateSampler(
    const SDL_GPUSamplerCreateInfo& info) noexcept {
  SE_ASSERT_GPU_THREAD();
  SDL_GPUSampler* sampler = SDL_CreateGPUSampler(Native(), &info);
  if (sampler != nullptr) Created(GpuObjectKind::kSampler);
  return sampler;
}

SDL_GPUGraphicsPipeline* GpuResources::CreatePipeline(
    const SDL_GPUGraphicsPipelineCreateInfo& info) noexcept {
  SE_ASSERT_GPU_THREAD();
  SDL_GPUGraphicsPipeline* pipeline =
      SDL_CreateGPUGraphicsPipeline(Native(), &info);
  if (pipeline != nullptr) Created(GpuObjectKind::kGraphicsPipeline);
  return pipeline;
}

std::uint64_t GpuResources::LiveObjects() const noexcept {
  std::uint64_t total = 0;
  for (const std::uint64_t live : live_) total += live;
  return total;
}

std::uint64_t GpuResources::LiveObjects(GpuObjectKind kind) const noexcept {
  return live_[static_cast<std::size_t>(kind)];
}

// --- Device loss ---

void GpuResources::ReleaseAll() {
  SE_ASSERT_GPU_THREAD();
  DropUploadPlan();
  for (Slot& slot : slots_) Retire(std::exchange(slot.resident, nullptr));
  for (PendingUpload& entry : pending_) {
    Retire(std::exchange(entry.texture, nullptr));
  }
  pending_.clear();
  for (std::size_t i = 0; i < in_flight_count_; ++i) {
    UploadBatch& batch = in_flight_[i];
    for (Completion& completion : batch.completions) {
      Retire(std::exchange(completion.texture, nullptr));
    }
    batch.completions.clear();
    ReleaseFence(std::exchange(batch.fence, nullptr));
  }
  in_flight_count_ = 0;
  Retire(std::exchange(upload_buffer_, nullptr));
}

std::expected<void, Error> GpuResources::RecreateAll(GpuDevice& device) {
  SE_ASSERT_GPU_THREAD();
  SE_ASSERT_MSG(retired_.empty(),
                "RecreateAll before the old device's objects were released");
  device_ = &device;
  for (std::uint32_t index = 0; index < slots_.size(); ++index) {
    Slot& slot = slots_[index];
    if (!slot.alive) continue;
    const TextureHandle handle{index + 1, slot.generation};
    if (slot.kind == SlotKind::kTarget) {
      slot.resident = NewSlotTexture(slot, slot.width, slot.height);
      if (slot.resident == nullptr) {
        return Fail(ErrorCode::kUnsupported,
                    SdlError(std::format("SDL_CreateGPUTexture(target '{}')",
                                         slot.name)));
      }
      ++slot.content_generation;
    } else if (slot.payload != nullptr) {
      if (auto queued = QueueUpload(handle, slot.payload); !queued) {
        return queued;
      }
    }
  }
  // The upload buffer, then every payload (the magenta first: it is the
  // first slot).
  SDL_GPUTransferBufferCreateInfo upload{};
  upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  upload.size = static_cast<std::uint32_t>(kUploadBudgetBytes);
  upload_buffer_ = CreateTransferBuffer(upload);
  if (upload_buffer_ == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                SdlError("SDL_CreateGPUTransferBuffer(uploads)"));
  }
  return FlushUploads();
}

}  // namespace se
