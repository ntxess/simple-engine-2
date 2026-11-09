// Copyright 2026 The simple-engine-2 Authors.
//
// Renderer2D: the batched sprite renderer on main (docs/milestone-specs.md
// M7, renderer_2d.h; docs/design/mt-design.md "Rendering"). Main thread
// only; it owns the GpuResources and drives the FrameRecorder transaction.
//
// Frame() takes RenderLayers, each a World seen through a Camera2D at an
// interpolation alpha (or a RenderList the caller built), for the game view
// and for any ViewportTargets, and runs:
//   - the build phase (may throw, contained by FrameRecorder::RunBuildPhase):
//     uploads whose fence signalled become resident, targets take the size
//     requested for this frame, the RenderLists are built, the camera
//     transform of each layer is computed (FitCanvas: letterboxed,
//     Camera2D::integer_scale honoured), vertices go into a pre-sized CPU
//     buffer, the one vertex/index buffer pair grows if it must (logged),
//     batches become draws (one per texture change), uploads are planned;
//   - the recording phase (noexcept): uploads, the vertex copy pass through
//     a transfer buffer mapped with cycle, the viewport passes, the
//     composite pass, the swapchain acquire and the blit.
// The sprite pipeline samples with the nearest texel and blends straight
// alpha as SFML's BlendAlpha does (ADR-061). The game view renders into an
// offscreen composite the size of the window's pixels; the blit draws it
// to the swapchain. The composite serves readback (goldens, tests,
// --readback) and RepresentLast(), HalfRate's re-present in Standard pacing
// (M6 integration). A target whose pass fails keeps its last image.
//
// PumpUploads() keeps uploads going on iterates that do not render (a
// minimized window). Device loss (integration): ReleaseGpuObjects(), then
// RecreateGpuObjects() on the new device; handles survive.

#ifndef SE_RENDER_RENDERER_2D_H_
#define SE_RENDER_RENDERER_2D_H_

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_render/frame_faults.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render_core/render_components.h"
#include "se_render_core/render_list.h"
#include "se_render_core/sprite_vertices.h"

namespace se {

// One list of sprites drawn into a target, in order.
struct RenderLayer {
  // Built with BuildRenderList(*world, camera, alpha) in the build phase,
  // unless |list| is given (a list the caller built: tests, tools).
  const World* world = nullptr;
  const RenderList* list = nullptr;
  Camera2D camera;
  float alpha = 1.0F;
};

// An offscreen colour target with its own size and camera, for M10's
// editor viewports. Renderer2D owns it (CreateViewport).
class ViewportTarget {
 public:
  ViewportTarget(const ViewportTarget&) = delete;
  ViewportTarget& operator=(const ViewportTarget&) = delete;

  // The target's texture, resolvable through GpuResources (M10 shows it
  // with ImGui::Image through GpuResources::imguiId).
  [[nodiscard]] TextureHandle TargetTexture() const noexcept {
    return texture_;
  }
  [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }
  // Applies at the next frame start; the old texture is retired.
  void RequestSize(std::uint32_t width, std::uint32_t height) noexcept {
    requested_width_ = width;
    requested_height_ = height;
  }
  void SetCamera(const Camera2D& camera) noexcept { camera_ = camera; }
  [[nodiscard]] const Camera2D& Camera() const noexcept { return camera_; }
  void SetClearColor(const ClearColor& color) noexcept { clear_ = color; }
  [[nodiscard]] const ClearColor& GetClearColor() const noexcept {
    return clear_;
  }

 private:
  friend class Renderer2D;
  ViewportTarget() = default;

  TextureHandle texture_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t requested_width_ = 0;
  std::uint32_t requested_height_ = 0;
  Camera2D camera_;
  ClearColor clear_;
};

// A viewport and what it shows; every layer is seen through the
// viewport's camera.
struct ViewportDraw {
  ViewportTarget* target = nullptr;
  std::span<const RenderLayer> layers;
};

struct FrameRequest {
  // The game view, bottom first, into the composite.
  std::span<const RenderLayer> layers;
  ClearColor clear;
  // Drawn before the composite.
  std::span<const ViewportDraw> viewports;
  // Acquire the swapchain and blit the composite (false: offscreen only).
  bool present = true;
};

struct FrameResult {
  // A frame command buffer was submitted.
  bool submitted = false;
  bool presented = false;
  bool null_acquire = false;
  // SDL_SubmitGPUCommandBufferAndAcquireFence failed (the App then checks
  // the device status).
  bool submit_failed = false;
  // The build phase threw; nothing was recorded.
  std::optional<Error> build_fault;
  // The frame was skipped for another reason (the command buffer, or a
  // buffer that could not grow).
  std::optional<Error> error;
};

// One frame's numbers (the design's per-frame stats; the build and record
// times feed FrameCosts' R).
struct Renderer2DStats {
  std::uint64_t frame = 0;
  std::uint32_t layers = 0;
  std::uint32_t targets = 0;
  std::uint32_t quads = 0;
  std::uint32_t batches = 0;
  std::uint32_t draws = 0;
  std::uint64_t upload_bytes = 0;
  bool presented = false;
  bool null_acquire = false;
  std::uint64_t build_ns = 0;
  std::uint64_t record_ns = 0;
  // Inside the swapchain wait and acquire (not CPU work).
  std::uint64_t swapchain_wait_ns = 0;
  std::uint64_t submit_ns = 0;
  // The GPU duration of the newest earlier frame whose timing arrived at
  // this frame's start (se::present timestamps; FrameCosts' g).
  bool gpu_timed = false;
  std::uint64_t gpu_ns = 0;
};

struct Renderer2DTotals {
  std::uint64_t frames = 0;
  std::uint64_t submitted = 0;
  std::uint64_t presented = 0;
  std::uint64_t null_acquires = 0;
  std::uint64_t represents = 0;
  std::uint64_t build_faults = 0;
  std::uint64_t skipped_frames = 0;
  std::uint64_t prepare_failures = 0;
  std::uint64_t copy_pass_failures = 0;
  std::uint64_t pass_failures = 0;
  std::uint64_t swapchain_pass_failures = 0;
  std::uint64_t submit_failures = 0;
  std::uint64_t upload_command_failures = 0;
  std::uint64_t vertex_growths = 0;
  std::uint64_t readbacks = 0;
  // Frames whose GPU duration was read.
  std::uint64_t gpu_timings = 0;
};

struct Renderer2DConfig {
  // Initial capacity of the vertex and index buffers.
  std::uint32_t initial_quads = 4096;
};

class Renderer2D {
 public:
  static constexpr SDL_GPUTextureFormat kTargetFormat =
      SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  // Fences kept for the pacer's N-L wait (frame latency 1 or 2).
  static constexpr std::size_t kFenceRing = 3;

  // The pipelines, the sampler and the buffers; the blit pipeline when the
  // device has claimed a window.
  [[nodiscard]] static std::expected<std::unique_ptr<Renderer2D>, Error> Create(
      GpuDevice& device, const Renderer2DConfig& config = {})
      SE_REQUIRES(g_main);

  Renderer2D(const Renderer2D&) = delete;
  Renderer2D& operator=(const Renderer2D&) = delete;
  // Waits for the GPU and releases everything, before the GpuDevice goes;
  // logs nothing (Shutdown reports).
  ~Renderer2D();

  // The orderly end: waits for the GPU, releases every object and logs the
  // totals and the leak check ("GPU objects: N created, N released, 0
  // alive", or an error with the counts that were never released). The
  // renderer draws nothing afterwards.
  void Shutdown() SE_REQUIRES(g_main);

  [[nodiscard]] GpuResources& Resources() noexcept { return *resources_; }
  [[nodiscard]] FrameFaults& Faults() noexcept { return faults_; }

  [[nodiscard]] FrameResult Frame(const FrameRequest& request)
      SE_REQUIRES(g_main);
  // The blit of the last composite again: no new RenderList.
  [[nodiscard]] FrameResult RepresentLast() SE_REQUIRES(g_main);
  // Iterates that do not render: residency, then the uploads alone.
  void PumpUploads() SE_REQUIRES(g_main);

  [[nodiscard]] std::expected<ViewportTarget*, Error> CreateViewport(
      std::uint32_t width, std::uint32_t height) SE_REQUIRES(g_main);
  void DestroyViewport(ViewportTarget* viewport) SE_REQUIRES(g_main);

  // The composite's size: the window's pixels at each frame start (0, 0),
  // or a fixed size (tools without a window: the canvas size).
  void SetCompositeSize(std::uint32_t width, std::uint32_t height) noexcept {
    fixed_width_ = width;
    fixed_height_ = height;
  }
  [[nodiscard]] TextureHandle Composite() const noexcept { return composite_; }

  // Copies the composite, or |viewport|'s target, back to the CPU (RGBA8):
  // records a download, submits and waits for it. Test hooks and tools.
  [[nodiscard]] std::expected<TexturePayload, Error> Readback(
      const ViewportTarget* viewport = nullptr) SE_REQUIRES(g_main);

  // The fence of the frame submitted |frames_back| frames ago (1: the
  // last), or null: the pacer's N-L wait (RenderPresentPort).
  [[nodiscard]] SDL_GPUFence* FrameFence(
      std::size_t frames_back) const noexcept;

  // Device loss: every GPU object through the retire queue, the GPU idle,
  // the queue run; then the objects again on |device|.
  void ReleaseGpuObjects() SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> RecreateGpuObjects(GpuDevice& device)
      SE_REQUIRES(g_main);

  [[nodiscard]] const Renderer2DStats& LastFrame() const noexcept {
    return last_;
  }
  [[nodiscard]] const Renderer2DTotals& Totals() const noexcept {
    return totals_;
  }
  [[nodiscard]] std::uint32_t QuadCapacity() const noexcept {
    return quad_capacity_;
  }

 private:
  struct Draw {
    TextureHandle texture;
    std::uint32_t first_quad = 0;
    std::uint32_t quads = 0;
    std::uint32_t clip = 0;
  };
  struct TargetPlan {
    TextureHandle texture;
    ClearColor clear;
    std::uint32_t first_draw = 0;
    std::uint32_t draws = 0;
    bool composite = false;
  };

  Renderer2D(GpuDevice& device, std::unique_ptr<GpuResources> resources,
             const Renderer2DConfig& config) noexcept;

  [[nodiscard]] std::expected<void, Error> CreateDeviceObjects()
      SE_REQUIRES(g_main);
  void RetireDeviceObjects() SE_REQUIRES(g_main);
  // Teardown with the GPU idle: the fences and this class's objects,
  // released at once. Returns the objects still alive afterwards.
  std::uint64_t ReleaseEverything() noexcept SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> EnsureCapacity(std::uint32_t quads)
      SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> PrepareTargets(
      const FrameRequest& request) SE_REQUIRES(g_main);
  // The build phase's work (may throw).
  [[nodiscard]] std::expected<void, Error> Build(const FrameRequest& request)
      SE_REQUIRES(g_main);
  void PlanTarget(TextureHandle texture, const ClearColor& clear,
                  std::span<const RenderLayer> layers,
                  const Camera2D* camera_override, bool composite)
      SE_REQUIRES(g_main);
  bool UploadVertices(FrameRecorder& recorder) noexcept SE_REQUIRES(g_main);
  bool DrawTarget(FrameRecorder& recorder, const TargetPlan& target) noexcept
      SE_REQUIRES(g_main);
  void Blit(FrameRecorder& recorder, SDL_GPUTexture* swapchain,
            const ClearColor& clear) noexcept SE_REQUIRES(g_main);
  // |timed|: a Frame() submission, whose GPU duration CollectGpuTimes
  // reads (not RepresentLast's blit).
  void KeepFence(SDL_GPUFence* fence, bool timed) noexcept SE_REQUIRES(g_main);
  void ReleaseFences() noexcept SE_REQUIRES(g_main);
  // Frame start: the GPU durations of the kept frames whose fences have
  // signalled; the newest goes into last_.
  void CollectGpuTimes() SE_REQUIRES(g_main);
  void CountRecord(const FrameRecorder& recorder, FrameResult& result) noexcept;

  GpuDevice* device_;
  std::unique_ptr<GpuResources> resources_;
  Renderer2DConfig config_;
  FrameFaults faults_;

  SDL_GPUGraphicsPipeline* sprite_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* blit_pipeline_ = nullptr;
  SDL_GPUSampler* nearest_ = nullptr;
  SDL_GPUBuffer* vertex_buffer_ = nullptr;
  SDL_GPUBuffer* index_buffer_ = nullptr;
  SDL_GPUTransferBuffer* vertex_transfer_ = nullptr;
  std::uint32_t quad_capacity_ = 0;
  bool indices_dirty_ = true;

  TextureHandle composite_;
  bool composite_drawn_ = false;
  std::uint32_t fixed_width_ = 0;
  std::uint32_t fixed_height_ = 0;
  std::vector<std::unique_ptr<ViewportTarget>> viewports_;

  // Build-phase output, reused across frames.
  // A deque: growing it keeps the lists already built this frame in place.
  std::deque<RenderList> lists_;
  std::vector<const RenderList*> layer_lists_;
  std::vector<std::array<float, 4>> clips_;
  std::vector<Draw> draws_;
  std::vector<TargetPlan> targets_;
  std::vector<SpriteVertex> vertices_;
  std::vector<std::uint32_t> indices_;
  std::uint32_t frame_quads_ = 0;
  std::uint64_t planned_upload_bytes_ = 0;

  std::array<SDL_GPUFence*, kFenceRing> fences_{};
  // The kept frame's GPU duration has yet to be read.
  std::array<bool, kFenceRing> fence_timed_{};
  std::size_t fence_head_ = 0;

  Renderer2DStats last_;
  Renderer2DTotals totals_;
  bool shut_down_ = false;
};

}  // namespace se

#endif  // SE_RENDER_RENDERER_2D_H_
