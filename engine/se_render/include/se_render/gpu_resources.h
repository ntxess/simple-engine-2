// Copyright 2026 The simple-engine-2 Authors.
//
// GpuResources: the renderer's GPU objects and their lifetimes
// (docs/milestone-specs.md M7, gpu_resources.h; docs/design/mt-design.md
// "Rendering", "Resource lifetime", red team C2 and C10). Main thread only;
// every call that reaches SDL_GPU asserts it (SDL_GPU does no thread
// checks).
//
// Textures are slots named by Handle<Texture>{index, generation}. The
// generation changes only when a slot is removed, so a handle stays valid
// across reloads; Resolve() returns the slot's resident texture for this
// frame only, and callers never store the pointer (a planted check bans
// SDL_GPUTexture* members outside se_render). A stale, unknown or null
// handle, and a slot with nothing resident yet (still uploading, or its
// file failed to load), resolve to a built-in 1x1 magenta texture, counted
// in the stats.
//
// The retire queue: Retire() keeps an object until RunRetireQueue(), which
// the frame transaction calls right after its submit. SDL's D3D12 backend
// frees a texture's client container at release (only the GPU resource is
// deferred), so nothing recorded this frame may name a released object.
//
// The upload queue: QueueUpload() creates the slot's next texture and
// keeps the decoded RGBA8 payload. PlanUploads() (the frame's build phase)
// picks at most kUploadBudgetBytes of rows, oldest upload first; a large
// texture fills over several frames. RecordUploads() copies them through
// one transfer buffer (mapped with cycle) in the upload command buffer,
// which is submitted with its own fence whether or not the frame presents;
// UploadsSubmitted() takes that fence. CollectUploads() at frame start
// makes every upload whose last rows' fence has signalled resident: the
// slot swaps to the new texture (its content generation flips) and the old
// one is retired, so a skipped frame never shows a half-uploaded texture.
// A newer upload of the same slot supersedes an older one.
//
// Every SDL_GPU object the renderer creates goes through the Create*
// helpers, which count live objects; ~GpuResources reports any that were
// never released (the fault-injection tests' leak check, since MSVC's ASan
// has no leak detection).
//
// Device loss (wired at the M6/M7 integration): ReleaseAll() moves every
// object into the retire queue and keeps handles and payloads; once the
// old device is idle and the queue has run, RecreateAll() creates the
// objects on the new device and queues every payload again.

#ifndef SE_RENDER_GPU_RESOURCES_H_
#define SE_RENDER_GPU_RESOURCES_H_

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_device.h"

namespace se {

// Tag types for Handle.
struct Texture {};

template <class T>
struct Handle {
  // Slot index + 1; 0 is the null handle.
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  [[nodiscard]] constexpr bool IsNull() const noexcept { return index == 0; }
  friend constexpr bool operator==(const Handle&, const Handle&) = default;
};

using TextureHandle = Handle<Texture>;

// A decoded texture: RGBA8, straight alpha, rows top to bottom.
struct TexturePayload {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] std::uint64_t Bytes() const noexcept {
    return std::uint64_t{width} * height * 4;
  }
};

// Valid for the current frame only. |texture| is never null.
struct ResolvedTexture {
  SDL_GPUTexture* texture = nullptr;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  // The magenta fallback.
  bool fallback = false;
};

struct GpuResourceStats {
  std::uint64_t resolves = 0;
  // Null, unknown or removed handles.
  std::uint64_t stale_resolves = 0;
  // Valid handles with no resident texture (uploading, or never loaded).
  std::uint64_t unresident_resolves = 0;
  std::uint64_t uploads_queued = 0;
  std::uint64_t uploads_resident = 0;
  // Uploads replaced by a newer one of the same slot.
  std::uint64_t uploads_superseded = 0;
  // Upload command buffers submitted, and the bytes they copied.
  std::uint64_t upload_batches = 0;
  std::uint64_t upload_bytes = 0;
  std::uint64_t upload_failures = 0;
  std::uint64_t retired = 0;
  std::uint64_t released = 0;
  std::uint64_t created = 0;
};

// The kinds of objects the retire queue and the live counts know.
enum class GpuObjectKind : std::uint8_t {
  kTexture,
  kBuffer,
  kTransferBuffer,
  kSampler,
  kGraphicsPipeline,
  kFence,
};
inline constexpr std::size_t kGpuObjectKinds = 6;

class GpuResources {
 public:
  // Bytes RecordUploads copies at most per frame (the design's budget).
  static constexpr std::uint64_t kUploadBudgetBytes = std::uint64_t{8} << 20;
  static constexpr SDL_GPUTextureFormat kTextureFormat =
      SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

  // The upload transfer buffer and the magenta fallback, uploaded before
  // Create returns.
  [[nodiscard]] static std::expected<std::unique_ptr<GpuResources>, Error>
  Create(GpuDevice& device) SE_REQUIRES(g_main);

  GpuResources(const GpuResources&) = delete;
  GpuResources& operator=(const GpuResources&) = delete;
  // Releases every object at once (the device must be idle).
  ~GpuResources();

  // Releases every object this class still holds (the device must be
  // idle) and returns how many counted objects are still alive: objects
  // created through the helpers and never retired, that is leaks. Logs
  // nothing (destructors call it); Renderer2D::Shutdown reports it.
  std::uint64_t ReleaseRemaining() noexcept SE_REQUIRES(g_main);

  // --- Texture slots ---
  // The slot for |name| (a key of config/texture.json), keyed by
  // TextureKeyOf(name); the existing handle when the name has one. Nothing
  // is resident until an upload completes.
  TextureHandle AddTexture(std::string_view name) SE_REQUIRES(g_main);
  // The handle registered for |key|, or the null handle.
  [[nodiscard]] TextureHandle Find(std::uint32_t key) const noexcept;
  // A render target slot (offscreen colour target, sampled afterwards).
  [[nodiscard]] std::expected<TextureHandle, Error> AddTarget(
      std::string_view name, std::uint32_t width, std::uint32_t height,
      SDL_GPUTextureFormat format) SE_REQUIRES(g_main);
  // Gives a target slot a new texture of the new size; the old one is
  // retired. Call at frame start.
  [[nodiscard]] std::expected<void, Error> ResizeTarget(TextureHandle handle,
                                                        std::uint32_t width,
                                                        std::uint32_t height)
      SE_REQUIRES(g_main);
  // Retires the slot's textures; the handle goes stale.
  void Remove(TextureHandle handle) SE_REQUIRES(g_main);

  [[nodiscard]] ResolvedTexture Resolve(TextureHandle handle) noexcept;
  [[nodiscard]] bool IsResident(TextureHandle handle) const noexcept;
  // The slot's name, empty for a stale handle.
  [[nodiscard]] std::string_view NameOf(TextureHandle handle) const noexcept;
  // Changes when the slot's resident texture changes (a reload, a resize).
  [[nodiscard]] std::uint32_t ContentGeneration(
      TextureHandle handle) const noexcept;
  [[nodiscard]] TextureHandle Magenta() const noexcept { return magenta_; }

  // --- Upload queue ---
  // Creates the slot's next texture and queues |payload| (kept for
  // RecreateAll). Fails for a stale handle, a target, an empty or
  // inconsistent payload, or when SDL cannot create the texture.
  [[nodiscard]] std::expected<void, Error> QueueUpload(
      TextureHandle handle, std::shared_ptr<const TexturePayload> payload)
      SE_REQUIRES(g_main);
  // Frame start: uploads whose fence has signalled become resident.
  void CollectUploads() SE_REQUIRES(g_main);
  [[nodiscard]] bool UploadsPending() const noexcept {
    return !pending_.empty();
  }
  [[nodiscard]] bool UploadsInFlight() const noexcept {
    return in_flight_count_ != 0;
  }
  // Build phase: plans this frame's copies; returns their bytes. Allocates.
  std::uint64_t PlanUploads() SE_REQUIRES(g_main);
  // Records the planned copies into |commands| (the upload command
  // buffer). Returns false when nothing was recorded (no plan, or SDL could
  // not map the transfer buffer or begin the copy pass): the uploads stay
  // queued. |fail_copy_pass| simulates the copy pass failing (test hook).
  bool RecordUploads(SDL_GPUCommandBuffer* commands,
                     bool fail_copy_pass) noexcept SE_REQUIRES(g_main);
  // The upload command buffer was submitted: |fence| (owned from now on)
  // signals when the recorded copies are done. nullptr: the submit failed
  // and the copies will be planned again.
  void UploadsSubmitted(SDL_GPUFence* fence) noexcept SE_REQUIRES(g_main);
  // The recorded plan was dropped without a submit.
  void DropUploadPlan() noexcept {
    plan_.chunks.clear();
    plan_.completing = 0;
    plan_.recorded = false;
  }
  // Boot and device recreation: records, submits and waits until every
  // queued upload is resident.
  [[nodiscard]] std::expected<void, Error> FlushUploads() SE_REQUIRES(g_main);

  // --- Retire queue ---
  void Retire(SDL_GPUTexture* texture) SE_REQUIRES(g_main);
  void Retire(SDL_GPUBuffer* buffer) SE_REQUIRES(g_main);
  void Retire(SDL_GPUTransferBuffer* buffer) SE_REQUIRES(g_main);
  void Retire(SDL_GPUSampler* sampler) SE_REQUIRES(g_main);
  void Retire(SDL_GPUGraphicsPipeline* pipeline) SE_REQUIRES(g_main);
  // Right after a submit: releases everything retired before it.
  void RunRetireQueue() noexcept SE_REQUIRES(g_main);
  [[nodiscard]] std::size_t RetireQueueSize() const noexcept {
    return retired_.size();
  }
  // Releases a fence now (fences are never recorded into a command
  // buffer).
  void ReleaseFence(SDL_GPUFence* fence) noexcept SE_REQUIRES(g_main);
  // Releases |object| now, past the retire queue: only while the GPU is
  // idle (teardown).
  void ReleaseWhileIdle(GpuObjectKind kind, void* object) noexcept
      SE_REQUIRES(g_main) {
    ReleaseNow(kind, object);
  }

  // --- Counted creation ---
  [[nodiscard]] SDL_GPUTexture* CreateGpuTexture(
      const SDL_GPUTextureCreateInfo& info) noexcept SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUBuffer* CreateGpuBuffer(
      const SDL_GPUBufferCreateInfo& info) noexcept SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUTransferBuffer* CreateTransferBuffer(
      const SDL_GPUTransferBufferCreateInfo& info) noexcept SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUSampler* CreateSampler(
      const SDL_GPUSamplerCreateInfo& info) noexcept SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUGraphicsPipeline* CreatePipeline(
      const SDL_GPUGraphicsPipelineCreateInfo& info) noexcept
      SE_REQUIRES(g_main);
  // Counts a fence from SDL_SubmitGPUCommandBufferAndAcquireFence.
  void CountFence(SDL_GPUFence* fence) noexcept;

  // --- Device loss ---
  void ReleaseAll() SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> RecreateAll(GpuDevice& device)
      SE_REQUIRES(g_main);

  [[nodiscard]] GpuDevice& Device() const noexcept { return *device_; }
  [[nodiscard]] SDL_GPUDevice* Native() const noexcept {
    return device_->Native();
  }
  [[nodiscard]] const GpuResourceStats& Stats() const noexcept {
    return stats_;
  }
  // SDL_GPU objects created through the helpers and not yet released.
  [[nodiscard]] std::uint64_t LiveObjects() const noexcept;
  [[nodiscard]] std::uint64_t LiveObjects(GpuObjectKind kind) const noexcept;

 private:
  enum class SlotKind : std::uint8_t { kUploaded, kTarget };

  struct Slot {
    std::string name;
    std::uint32_t key = 0;
    std::uint32_t generation = 1;
    std::uint32_t content_generation = 0;
    bool alive = false;
    SlotKind kind = SlotKind::kUploaded;
    SDL_GPUTexture* resident = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    SDL_GPUTextureFormat format = kTextureFormat;
    // The newest queued upload; older completions are retired.
    std::uint64_t latest_upload = 0;
    // The newest payload, kept for RecreateAll.
    std::shared_ptr<const TexturePayload> payload;
  };

  struct PendingUpload {
    std::uint32_t slot = 0;
    std::uint32_t slot_generation = 0;
    std::uint64_t serial = 0;
    std::shared_ptr<const TexturePayload> payload;
    SDL_GPUTexture* texture = nullptr;
    // Rows already submitted.
    std::uint32_t next_row = 0;
  };

  // Rows [y, y + rows) of pending_[pending], at |offset| in the transfer
  // buffer.
  struct Chunk {
    std::size_t pending = 0;
    std::uint32_t y = 0;
    std::uint32_t rows = 0;
    std::uint32_t offset = 0;
  };

  // An upload whose last rows went into a submitted batch.
  struct Completion {
    std::uint32_t slot = 0;
    std::uint32_t slot_generation = 0;
    std::uint64_t serial = 0;
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  struct UploadPlan {
    std::vector<Chunk> chunks;
    // Reserved by PlanUploads for the uploads it completes, so that
    // UploadsSubmitted (noexcept) does not allocate.
    std::vector<Completion> completions;
    std::size_t completing = 0;
    std::uint64_t bytes = 0;
    bool recorded = false;
  };

  struct UploadBatch {
    SDL_GPUFence* fence = nullptr;
    std::vector<Completion> completions;
  };
  // Upload batches in flight at most; PlanUploads plans nothing while all
  // are (a fixed array, so recording never allocates).
  static constexpr std::size_t kMaxInFlight = 16;

  struct RetiredObject {
    GpuObjectKind kind = GpuObjectKind::kTexture;
    void* object = nullptr;
  };

  explicit GpuResources(GpuDevice& device) : device_(&device) {}

  [[nodiscard]] std::expected<void, Error> CreateDeviceObjects()
      SE_REQUIRES(g_main);
  [[nodiscard]] Slot* SlotOf(TextureHandle handle) noexcept;
  [[nodiscard]] const Slot* SlotOf(TextureHandle handle) const noexcept;
  std::uint32_t NewSlot() SE_REQUIRES(g_main);
  [[nodiscard]] SDL_GPUTexture* NewSlotTexture(const Slot& slot,
                                               std::uint32_t width,
                                               std::uint32_t height) noexcept
      SE_REQUIRES(g_main);
  void RetireObject(GpuObjectKind kind, void* object) SE_REQUIRES(g_main);
  void ReleaseNow(GpuObjectKind kind, void* object) noexcept
      SE_REQUIRES(g_main);
  void Created(GpuObjectKind kind) noexcept;

  GpuDevice* device_;
  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_slots_;
  std::unordered_map<std::uint32_t, std::uint32_t> by_key_;
  TextureHandle magenta_;

  std::deque<PendingUpload> pending_;
  UploadPlan plan_;
  std::array<UploadBatch, kMaxInFlight> in_flight_;
  std::size_t in_flight_count_ = 0;
  std::uint64_t next_serial_ = 1;
  SDL_GPUTransferBuffer* upload_buffer_ = nullptr;

  std::vector<RetiredObject> retired_;
  std::array<std::uint64_t, kGpuObjectKinds> live_{};
  GpuResourceStats stats_;
};

}  // namespace se

#endif  // SE_RENDER_GPU_RESOURCES_H_
