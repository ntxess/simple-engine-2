// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/renderer_2d.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_render/frame_faults.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render/present.h"
#include "se_render/shaders.h"
#include "se_render_core/render_list.h"
#include "se_render_core/sprite_vertices.h"
#include "src/gpu_assert.h"

namespace se {
namespace {

// The sprite vertex shader's uniform block (sprite.vert.hlsl).
struct SpriteUniforms {
  std::array<float, 4> canvas_to_clip{};
  std::array<float, 4> texel_to_uv{};
};
static_assert(sizeof(SpriteUniforms) == 32);

constexpr std::uint32_t kMinQuads = 256;
constexpr std::uint32_t kVertexBytesPerQuad =
    kVerticesPerQuad * sizeof(SpriteVertex);
constexpr std::uint32_t kIndexBytesPerQuad =
    kIndicesPerQuad * sizeof(std::uint32_t);

std::string SdlError(std::string_view what) {
  std::string message(what);
  message += ": ";
  message += SDL_GetError();
  return message;
}

SDL_FColor ToFColor(const ClearColor& color) {
  return SDL_FColor{color.r, color.g, color.b, color.a};
}

std::uint64_t NowNs() { return SDL_GetTicksNS(); }

}  // namespace

Renderer2D::Renderer2D(GpuDevice& device,
                       std::unique_ptr<GpuResources> resources,
                       const Renderer2DConfig& config) noexcept
    : device_(&device), resources_(std::move(resources)), config_(config) {}

std::expected<std::unique_ptr<Renderer2D>, Error> Renderer2D::Create(
    GpuDevice& device, const Renderer2DConfig& config) {
  SE_ASSERT_GPU_THREAD();
  auto resources = GpuResources::Create(device);
  if (!resources) return std::unexpected(std::move(resources.error()));
  std::unique_ptr<Renderer2D> renderer(
      new Renderer2D(device, std::move(*resources), config));
  if (auto made = renderer->CreateDeviceObjects(); !made) {
    return std::unexpected(std::move(made.error()));
  }
  return renderer;
}

Renderer2D::~Renderer2D() {
  SE_ASSERT_GPU_THREAD();
  if (!shut_down_) ReleaseEverything();
}

std::uint64_t Renderer2D::ReleaseEverything() noexcept {
  device_->WaitForIdle();
  ReleaseFences();
  for (void* pipeline : {static_cast<void*>(sprite_pipeline_),
                         static_cast<void*>(blit_pipeline_)}) {
    resources_->ReleaseWhileIdle(GpuObjectKind::kGraphicsPipeline, pipeline);
  }
  sprite_pipeline_ = nullptr;
  blit_pipeline_ = nullptr;
  resources_->ReleaseWhileIdle(GpuObjectKind::kSampler,
                               std::exchange(nearest_, nullptr));
  resources_->ReleaseWhileIdle(GpuObjectKind::kBuffer,
                               std::exchange(vertex_buffer_, nullptr));
  resources_->ReleaseWhileIdle(GpuObjectKind::kBuffer,
                               std::exchange(index_buffer_, nullptr));
  resources_->ReleaseWhileIdle(GpuObjectKind::kTransferBuffer,
                               std::exchange(vertex_transfer_, nullptr));
  // The composite and the viewports are slots: released with the rest.
  return resources_->ReleaseRemaining();
}

void Renderer2D::Shutdown() {
  SE_ASSERT_GPU_THREAD();
  if (shut_down_) return;
  shut_down_ = true;
  const std::uint64_t alive = ReleaseEverything();
  SE_LOG_INFO(
      "renderer: {} frames, {} presented, {} NULL acquires, {} skipped, {} "
      "build faults",
      totals_.frames, totals_.presented, totals_.null_acquires,
      totals_.skipped_frames, totals_.build_faults);
  const GpuResourceStats& stats = resources_->Stats();
  if (alive != 0) {
    SE_LOG_ERROR(
        "GPU objects never released: {} ({} textures, {} buffers, {} "
        "transfer buffers, {} samplers, {} pipelines, {} fences)",
        alive, resources_->LiveObjects(GpuObjectKind::kTexture),
        resources_->LiveObjects(GpuObjectKind::kBuffer),
        resources_->LiveObjects(GpuObjectKind::kTransferBuffer),
        resources_->LiveObjects(GpuObjectKind::kSampler),
        resources_->LiveObjects(GpuObjectKind::kGraphicsPipeline),
        resources_->LiveObjects(GpuObjectKind::kFence));
  } else {
    SE_LOG_INFO("GPU objects: {} created, {} released, 0 alive", stats.created,
                stats.released);
  }
}

std::expected<void, Error> Renderer2D::CreateDeviceObjects() {
  SDL_GPUDevice* device = device_->Native();
  SDL_GPUSamplerCreateInfo sampler{};
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  nearest_ = resources_->CreateSampler(sampler);
  if (nearest_ == nullptr) {
    return Fail(ErrorCode::kUnsupported, SdlError("SDL_CreateGPUSampler"));
  }

  // The sprite pipeline: SpriteVertex in buffer slot 0, straight alpha
  // blended as SFML's BlendAlpha (colour: src alpha, 1 - src alpha; alpha:
  // one, 1 - src alpha), no culling (flipped sprites reverse the winding).
  {
    auto vertex = CreateShader(device, ShaderId::kSpriteVert);
    if (!vertex) return std::unexpected(std::move(vertex.error()));
    auto fragment = CreateShader(device, ShaderId::kSpriteFrag);
    if (!fragment) {
      SDL_ReleaseGPUShader(device, *vertex);
      return std::unexpected(std::move(fragment.error()));
    }
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(SpriteVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    std::array<SDL_GPUVertexAttribute, 3> attributes{};
    attributes[0].location = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = offsetof(SpriteVertex, x);
    attributes[1].location = 1;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[1].offset = offsetof(SpriteVertex, u);
    attributes[2].location = 2;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attributes[2].offset = offsetof(SpriteVertex, tint);
    SDL_GPUColorTargetDescription color{};
    color.format = kTargetFormat;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor =
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = *vertex;
    info.fragment_shader = *fragment;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes =
        static_cast<std::uint32_t>(attributes.size());
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    sprite_pipeline_ = resources_->CreatePipeline(info);
    SDL_ReleaseGPUShader(device, *vertex);
    SDL_ReleaseGPUShader(device, *fragment);
    if (sprite_pipeline_ == nullptr) {
      return Fail(ErrorCode::kUnsupported,
                  SdlError("SDL_CreateGPUGraphicsPipeline(sprite)"));
    }
  }

  // The blit pipeline, for the claimed window's swapchain format.
  if (SDL_Window* window = device_->ClaimedWindow(); window != nullptr) {
    auto vertex = CreateShader(device, ShaderId::kBlitVert);
    if (!vertex) return std::unexpected(std::move(vertex.error()));
    auto fragment = CreateShader(device, ShaderId::kBlitFrag);
    if (!fragment) {
      SDL_ReleaseGPUShader(device, *vertex);
      return std::unexpected(std::move(fragment.error()));
    }
    SDL_GPUColorTargetDescription color{};
    color.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = *vertex;
    info.fragment_shader = *fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1;
    blit_pipeline_ = resources_->CreatePipeline(info);
    SDL_ReleaseGPUShader(device, *vertex);
    SDL_ReleaseGPUShader(device, *fragment);
    if (blit_pipeline_ == nullptr) {
      return Fail(ErrorCode::kUnsupported,
                  SdlError("SDL_CreateGPUGraphicsPipeline(blit)"));
    }
  }

  const std::uint32_t quads =
      std::max(kMinQuads, std::max(config_.initial_quads, quad_capacity_));
  quad_capacity_ = 0;
  return EnsureCapacity(quads);
}

void Renderer2D::RetireDeviceObjects() {
  resources_->Retire(std::exchange(sprite_pipeline_, nullptr));
  resources_->Retire(std::exchange(blit_pipeline_, nullptr));
  resources_->Retire(std::exchange(nearest_, nullptr));
  resources_->Retire(std::exchange(vertex_buffer_, nullptr));
  resources_->Retire(std::exchange(index_buffer_, nullptr));
  resources_->Retire(std::exchange(vertex_transfer_, nullptr));
}

std::expected<void, Error> Renderer2D::EnsureCapacity(std::uint32_t quads) {
  if (quads <= quad_capacity_ && vertex_buffer_ != nullptr) return {};
  std::uint32_t capacity = std::max(kMinQuads, quad_capacity_);
  while (capacity < quads) capacity *= 2;
  SDL_GPUBufferCreateInfo vertex{};
  vertex.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
  vertex.size = capacity * kVertexBytesPerQuad;
  SDL_GPUBufferCreateInfo index{};
  index.usage = SDL_GPU_BUFFERUSAGE_INDEX;
  index.size = capacity * kIndexBytesPerQuad;
  SDL_GPUTransferBufferCreateInfo transfer{};
  transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer.size = vertex.size + index.size;
  SDL_GPUBuffer* new_vertex = resources_->CreateGpuBuffer(vertex);
  SDL_GPUBuffer* new_index = resources_->CreateGpuBuffer(index);
  SDL_GPUTransferBuffer* new_transfer =
      resources_->CreateTransferBuffer(transfer);
  if (new_vertex == nullptr || new_index == nullptr ||
      new_transfer == nullptr) {
    resources_->Retire(new_vertex);
    resources_->Retire(new_index);
    resources_->Retire(new_transfer);
    return Fail(
        ErrorCode::kUnsupported,
        SdlError(std::format("the sprite buffers for {} quads", capacity)));
  }
  if (quad_capacity_ != 0) {
    SE_LOG_INFO("renderer: sprite buffers grown from {} to {} quads",
                quad_capacity_, capacity);
    ++totals_.vertex_growths;
  }
  resources_->Retire(std::exchange(vertex_buffer_, new_vertex));
  resources_->Retire(std::exchange(index_buffer_, new_index));
  resources_->Retire(std::exchange(vertex_transfer_, new_transfer));
  quad_capacity_ = capacity;
  vertices_.resize(std::size_t{capacity} * kVerticesPerQuad);
  indices_.resize(std::size_t{capacity} * kIndicesPerQuad);
  WriteQuadIndices(indices_);
  indices_dirty_ = true;
  return {};
}

std::expected<ViewportTarget*, Error> Renderer2D::CreateViewport(
    std::uint32_t width, std::uint32_t height) {
  SE_ASSERT_GPU_THREAD();
  auto texture =
      resources_->AddTarget("<viewport>", width, height, kTargetFormat);
  if (!texture) return std::unexpected(std::move(texture.error()));
  std::unique_ptr<ViewportTarget> viewport(new ViewportTarget());
  viewport->texture_ = *texture;
  viewport->width_ = width;
  viewport->height_ = height;
  viewport->requested_width_ = width;
  viewport->requested_height_ = height;
  viewport->camera_.virtual_size = {static_cast<float>(width),
                                    static_cast<float>(height)};
  viewport->camera_.center = {0.5F * static_cast<float>(width),
                              0.5F * static_cast<float>(height)};
  viewports_.push_back(std::move(viewport));
  return viewports_.back().get();
}

void Renderer2D::DestroyViewport(ViewportTarget* viewport) {
  SE_ASSERT_GPU_THREAD();
  const auto found =
      std::find_if(viewports_.begin(), viewports_.end(),
                   [&](const auto& owned) { return owned.get() == viewport; });
  if (found == viewports_.end()) return;
  resources_->Remove((*found)->texture_);
  viewports_.erase(found);
}

std::expected<void, Error> Renderer2D::PrepareTargets(
    const FrameRequest& request) {
  for (const ViewportDraw& draw : request.viewports) {
    ViewportTarget& viewport = *draw.target;
    if (viewport.requested_width_ != viewport.width_ ||
        viewport.requested_height_ != viewport.height_) {
      if (auto resized = resources_->ResizeTarget(viewport.texture_,
                                                  viewport.requested_width_,
                                                  viewport.requested_height_);
          !resized) {
        return resized;
      }
      viewport.width_ = viewport.requested_width_;
      viewport.height_ = viewport.requested_height_;
    }
  }
  std::uint32_t width = fixed_width_;
  std::uint32_t height = fixed_height_;
  if (width == 0 || height == 0) {
    int w = 0;
    int h = 0;
    if (SDL_Window* window = device_->ClaimedWindow();
        window != nullptr && SDL_GetWindowSizeInPixels(window, &w, &h) &&
        w > 0 && h > 0) {
      width = static_cast<std::uint32_t>(w);
      height = static_cast<std::uint32_t>(h);
    }
  }
  if (width == 0 || height == 0) {
    // A minimized window reports no pixels: keep the last size.
    if (!composite_.IsNull()) return {};
    width = 1280;
    height = 720;
  }
  if (composite_.IsNull()) {
    auto texture =
        resources_->AddTarget("<composite>", width, height, kTargetFormat);
    if (!texture) return std::unexpected(std::move(texture.error()));
    composite_ = *texture;
    composite_drawn_ = false;
    return {};
  }
  const ResolvedTexture current = resources_->Resolve(composite_);
  if (current.width != width || current.height != height) {
    if (auto resized = resources_->ResizeTarget(composite_, width, height);
        !resized) {
      return resized;
    }
    composite_drawn_ = false;
  }
  return {};
}

void Renderer2D::PlanTarget(TextureHandle texture, const ClearColor& clear,
                            std::span<const RenderLayer> layers,
                            const Camera2D* camera_override, bool composite) {
  const ResolvedTexture target = resources_->Resolve(texture);
  TargetPlan plan;
  plan.texture = texture;
  plan.clear = clear;
  plan.first_draw = static_cast<std::uint32_t>(draws_.size());
  plan.composite = composite;
  for (const RenderLayer& layer : layers) {
    const Camera2D& camera =
        camera_override != nullptr ? *camera_override : layer.camera;
    const RenderList* list = layer.list;
    if (list == nullptr) {
      if (layer.world == nullptr) continue;
      const std::size_t slot = layer_lists_.size();
      if (lists_.size() <= slot) lists_.resize(slot + 1);
      BuildRenderList(*layer.world, camera, layer.alpha, lists_[slot]);
      list = &lists_[slot];
    }
    const auto clip = static_cast<std::uint32_t>(clips_.size());
    const CanvasFit fit = FitCanvas(camera.virtual_size, camera.integer_scale,
                                    target.width, target.height);
    clips_.push_back(CanvasToClip(fit, target.width, target.height));
    for (const QuadBatch& batch : list->batches) {
      draws_.push_back(Draw{resources_->Find(batch.texture),
                            frame_quads_ + batch.first, batch.count, clip});
    }
    layer_lists_.push_back(list);
    frame_quads_ += static_cast<std::uint32_t>(list->quads.size());
  }
  plan.draws = static_cast<std::uint32_t>(draws_.size()) - plan.first_draw;
  targets_.push_back(plan);
}

std::expected<void, Error> Renderer2D::Build(const FrameRequest& request) {
  SE_PROFILE_ZONE("Renderer2D build");
  resources_->CollectUploads();
  if (auto ready = PrepareTargets(request); !ready) {
    return ready;
  }
  layer_lists_.clear();
  clips_.clear();
  draws_.clear();
  targets_.clear();
  frame_quads_ = 0;
  if (faults_.Fires(FrameStep::kBuild)) ThrowInjectedFault(FrameStep::kBuild);
  for (const ViewportDraw& draw : request.viewports) {
    PlanTarget(draw.target->texture_, draw.target->clear_, draw.layers,
               &draw.target->camera_, false);
  }
  PlanTarget(composite_, request.clear, request.layers, nullptr, true);
  if (auto grown = EnsureCapacity(frame_quads_); !grown) {
    return grown;
  }

  if (faults_.Fires(FrameStep::kVertices)) {
    ThrowInjectedFault(FrameStep::kVertices);
  }
  std::size_t first = 0;
  for (const RenderList* list : layer_lists_) {
    WriteSpriteVertices(
        list->quads,
        std::span<SpriteVertex>(vertices_).subspan(
            first * kVerticesPerQuad, list->quads.size() * kVerticesPerQuad));
    first += list->quads.size();
  }
  planned_upload_bytes_ =
      resources_->UploadsPending() ? resources_->PlanUploads() : 0;
  return {};
}

bool Renderer2D::UploadVertices(FrameRecorder& recorder) noexcept {
  if (frame_quads_ == 0 && !indices_dirty_) return true;
  SDL_GPUDevice* device = device_->Native();
  auto* mapped = static_cast<std::uint8_t*>(
      SDL_MapGPUTransferBuffer(device, vertex_transfer_, true));
  if (mapped == nullptr) return false;
  const std::uint32_t vertex_bytes = frame_quads_ * kVertexBytesPerQuad;
  const std::uint32_t index_offset = quad_capacity_ * kVertexBytesPerQuad;
  const std::uint32_t index_bytes = quad_capacity_ * kIndexBytesPerQuad;
  if (vertex_bytes != 0) std::memcpy(mapped, vertices_.data(), vertex_bytes);
  if (indices_dirty_) {
    std::memcpy(mapped + index_offset, indices_.data(), index_bytes);
  }
  SDL_UnmapGPUTransferBuffer(device, vertex_transfer_);
  SDL_GPUCopyPass* pass = recorder.BeginCopyPass();
  if (pass == nullptr) return false;
  if (vertex_bytes != 0) {
    const SDL_GPUTransferBufferLocation source{vertex_transfer_, 0};
    const SDL_GPUBufferRegion region{vertex_buffer_, 0, vertex_bytes};
    SDL_UploadToGPUBuffer(pass, &source, &region, true);
  }
  if (indices_dirty_) {
    const SDL_GPUTransferBufferLocation source{vertex_transfer_, index_offset};
    const SDL_GPUBufferRegion region{index_buffer_, 0, index_bytes};
    SDL_UploadToGPUBuffer(pass, &source, &region, true);
    indices_dirty_ = false;
  }
  recorder.EndCopyPass();
  return true;
}

bool Renderer2D::DrawTarget(FrameRecorder& recorder,
                            const TargetPlan& target) noexcept {
  const ResolvedTexture texture = resources_->Resolve(target.texture);
  SDL_GPUColorTargetInfo info{};
  info.texture = texture.texture;
  info.clear_color = ToFColor(target.clear);
  info.load_op = SDL_GPU_LOADOP_CLEAR;
  info.store_op = SDL_GPU_STOREOP_STORE;
  // The previous frame's blit may still read it: let SDL cycle.
  info.cycle = true;
  SDL_GPURenderPass* pass = recorder.BeginRenderPass(info, false);
  if (pass == nullptr) return false;
  if (target.draws != 0) {
    SDL_BindGPUGraphicsPipeline(pass, sprite_pipeline_);
    const SDL_GPUBufferBinding vertices{vertex_buffer_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
    const SDL_GPUBufferBinding indices{index_buffer_, 0};
    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (std::uint32_t i = 0; i < target.draws; ++i) {
      const Draw& draw = draws_[target.first_draw + i];
      const ResolvedTexture sprite = resources_->Resolve(draw.texture);
      const SDL_GPUTextureSamplerBinding binding{sprite.texture, nearest_};
      SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
      SpriteUniforms uniforms;
      uniforms.canvas_to_clip = clips_[draw.clip];
      uniforms.texel_to_uv = {1.0F / static_cast<float>(sprite.width),
                              1.0F / static_cast<float>(sprite.height), 0.0F,
                              0.0F};
      SDL_PushGPUVertexUniformData(recorder.Commands(), 0, &uniforms,
                                   sizeof(uniforms));
      SDL_DrawGPUIndexedPrimitives(pass, draw.quads * kIndicesPerQuad, 1,
                                   draw.first_quad * kIndicesPerQuad, 0, 0);
      ++last_.draws;
    }
  }
  recorder.EndRenderPass();
  return true;
}

void Renderer2D::Blit(FrameRecorder& recorder, SDL_GPUTexture* swapchain,
                      const ClearColor& clear) noexcept {
  SDL_GPUColorTargetInfo info{};
  info.texture = swapchain;
  info.clear_color = ToFColor(clear);
  info.load_op =
      composite_drawn_ ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_CLEAR;
  info.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = recorder.BeginRenderPass(info, true);
  if (pass == nullptr) {
    ++totals_.swapchain_pass_failures;
    return;
  }
  if (composite_drawn_ && blit_pipeline_ != nullptr) {
    SDL_BindGPUGraphicsPipeline(pass, blit_pipeline_);
    const SDL_GPUTextureSamplerBinding binding{
        resources_->Resolve(composite_).texture, nearest_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  }
  recorder.EndRenderPass();
}

void Renderer2D::KeepFence(SDL_GPUFence* fence, bool timed) noexcept {
  if (fence == nullptr) return;
  fence_head_ = (fence_head_ + 1) % kFenceRing;
  if (fences_[fence_head_] != nullptr) {
    // A timing nobody read stays in the patch's ring until it is reused.
    resources_->ReleaseFence(fences_[fence_head_]);
  }
  fences_[fence_head_] = fence;
  fence_timed_[fence_head_] = timed;
}

void Renderer2D::ReleaseFences() noexcept {
  for (std::size_t i = 0; i < kFenceRing; ++i) {
    if (fences_[i] != nullptr) {
      resources_->ReleaseFence(std::exchange(fences_[i], nullptr));
    }
    fence_timed_[i] = false;
  }
}

void Renderer2D::CollectGpuTimes() {
  // Oldest first, so the newest arrival is the one reported.
  for (std::size_t back = kFenceRing; back >= 1; --back) {
    const std::size_t slot =
        (fence_head_ + kFenceRing - (back - 1)) % kFenceRing;
    if (!fence_timed_[slot] || fences_[slot] == nullptr) continue;
    const std::optional<present::GpuTime> time =
        present::ReadGpuTime(*device_, fences_[slot]);
    if (!time) continue;
    fence_timed_[slot] = false;
    last_.gpu_timed = true;
    last_.gpu_ns = time->duration_ns;
    ++totals_.gpu_timings;
  }
}

SDL_GPUFence* Renderer2D::FrameFence(std::size_t frames_back) const noexcept {
  if (frames_back == 0 || frames_back > kFenceRing) return nullptr;
  return fences_[(fence_head_ + kFenceRing - (frames_back - 1)) % kFenceRing];
}

void Renderer2D::CountRecord(const FrameRecorder& recorder,
                             FrameResult& result) noexcept {
  const FrameRecord& record = recorder.Record();
  result.submitted = record.submitted;
  result.presented = record.submitted && recorder.Swapchain() != nullptr;
  if (record.submitted) ++totals_.submitted;
  result.submit_failed = record.submit_failed;
  if (record.submit_failed) ++totals_.submit_failures;
  if (result.presented) ++totals_.presented;
  last_.presented = result.presented;
  last_.swapchain_wait_ns = record.swapchain_wait_ns;
  last_.submit_ns = record.submit_ns;
}

FrameResult Renderer2D::Frame(const FrameRequest& request) {
  SE_ASSERT_GPU_THREAD();
  SE_PROFILE_ZONE("Renderer2D frame");
  FrameResult result;
  last_ = Renderer2DStats{};
  last_.frame = ++totals_.frames;
  const std::uint64_t build_start = NowNs();
  CollectGpuTimes();

  // 1. The build phase.
  std::expected<void, Error> built;
  auto build = [&] {
    AssertMainThread();
    built = Build(request);
  };
  if (auto contained = FrameRecorder::RunBuildPhase(build); !contained) {
    ++totals_.build_faults;
    ++totals_.skipped_frames;
    resources_->DropUploadPlan();
    result.build_fault = std::move(contained.error());
    return result;
  }
  if (!built) {
    ++totals_.skipped_frames;
    resources_->DropUploadPlan();
    result.error = std::move(built.error());
    return result;
  }
  const std::uint64_t record_start = NowNs();
  last_.build_ns = record_start - build_start;
  last_.layers = static_cast<std::uint32_t>(layer_lists_.size());
  last_.targets = static_cast<std::uint32_t>(targets_.size());
  last_.quads = frame_quads_;
  last_.batches = static_cast<std::uint32_t>(draws_.size());

  // 2. Prepare.
  PrepareOptions options;
  options.uploads = planned_upload_bytes_ > 0;
  auto recorder =
      FrameRecorder::Prepare(*device_, *resources_, faults_, options);
  if (!recorder) {
    ++totals_.prepare_failures;
    ++totals_.skipped_frames;
    resources_->DropUploadPlan();
    if (totals_.prepare_failures == 1 || totals_.prepare_failures % 120 == 0) {
      SE_LOG_WARN("frame {} skipped ({} so far): {}", last_.frame,
                  totals_.prepare_failures, recorder.error().message);
    }
    // Uploads do not wait for a frame.
    PumpUploads();
    result.error = std::move(recorder.error());
    return result;
  }

  // 3. Uploads, vertices, the offscreen passes; then the swapchain.
  if (options.uploads) {
    if (!recorder->HasUploadCommands()) ++totals_.upload_command_failures;
    if (recorder->SubmitUploads()) last_.upload_bytes = planned_upload_bytes_;
  }
  const bool vertices_ok = UploadVertices(*recorder);
  if (!vertices_ok) ++totals_.copy_pass_failures;
  for (const TargetPlan& target : targets_) {
    // Without this frame's vertices a target keeps its last image.
    if (!vertices_ok) continue;
    if (!DrawTarget(*recorder, target)) {
      ++totals_.pass_failures;
      continue;
    }
    if (target.composite) composite_drawn_ = true;
  }
  if (request.present && device_->ClaimedWindow() != nullptr) {
    SDL_GPUTexture* swapchain = recorder->AcquireSwapchain();
    if (swapchain != nullptr) {
      // 4. The swapchain pass.
      Blit(*recorder, swapchain, request.clear);
    } else {
      result.null_acquire = true;
      last_.null_acquire = true;
      ++totals_.null_acquires;
    }
  }

  // 5. The guard: submit, keep the fence, run the retire queue.
  recorder->Submit();
  KeepFence(recorder->TakeFence(), /*timed=*/true);
  CountRecord(*recorder, result);
  last_.record_ns = NowNs() - record_start - last_.swapchain_wait_ns;
  return result;
}

FrameResult Renderer2D::RepresentLast() {
  SE_ASSERT_GPU_THREAD();
  FrameResult result;
  ++totals_.represents;
  auto recorder =
      FrameRecorder::Prepare(*device_, *resources_, faults_, PrepareOptions{});
  if (!recorder) {
    ++totals_.prepare_failures;
    result.error = std::move(recorder.error());
    return result;
  }
  if (device_->ClaimedWindow() != nullptr) {
    SDL_GPUTexture* swapchain = recorder->AcquireSwapchain();
    if (swapchain != nullptr) {
      Blit(*recorder, swapchain, ClearColor{});
    } else {
      result.null_acquire = true;
      ++totals_.null_acquires;
    }
  }
  recorder->Submit();
  KeepFence(recorder->TakeFence(), /*timed=*/false);
  CountRecord(*recorder, result);
  return result;
}

void Renderer2D::PumpUploads() {
  SE_ASSERT_GPU_THREAD();
  resources_->CollectUploads();
  if (!resources_->UploadsPending()) {
    resources_->RunRetireQueue();
    return;
  }
  resources_->PlanUploads();
  PrepareOptions options;
  options.frame = false;
  options.uploads = true;
  auto recorder =
      FrameRecorder::Prepare(*device_, *resources_, faults_, options);
  if (!recorder) {
    resources_->DropUploadPlan();
    return;
  }
  if (!recorder->HasUploadCommands()) ++totals_.upload_command_failures;
  recorder->SubmitUploads();
  // ~FrameRecorder runs the retire queue.
}

std::expected<TexturePayload, Error> Renderer2D::Readback(
    const ViewportTarget* viewport) {
  SE_ASSERT_GPU_THREAD();
  const TextureHandle handle =
      viewport != nullptr ? viewport->texture_ : composite_;
  if (handle.IsNull() || !resources_->IsResident(handle)) {
    return Fail(ErrorCode::kInvalidArgument, "nothing to read back yet");
  }
  const ResolvedTexture texture = resources_->Resolve(handle);
  TexturePayload image;
  image.width = texture.width;
  image.height = texture.height;
  SDL_GPUTransferBufferCreateInfo info{};
  info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  info.size = static_cast<std::uint32_t>(image.Bytes());
  SDL_GPUTransferBuffer* download = resources_->CreateTransferBuffer(info);
  if (download == nullptr) {
    return Fail(ErrorCode::kUnsupported,
                SdlError("SDL_CreateGPUTransferBuffer(readback)"));
  }
  SDL_GPUDevice* device = device_->Native();
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
  if (commands == nullptr) {
    resources_->Retire(download);
    resources_->RunRetireQueue();
    return Fail(ErrorCode::kUnsupported,
                SdlError("SDL_AcquireGPUCommandBuffer(readback)"));
  }
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(commands);
  if (pass != nullptr) {
    SDL_GPUTextureRegion region{};
    region.texture = texture.texture;
    region.w = image.width;
    region.h = image.height;
    region.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = download;
    destination.pixels_per_row = image.width;
    destination.rows_per_layer = image.height;
    SDL_DownloadFromGPUTexture(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);
  }
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  resources_->CountFence(fence);
  std::expected<TexturePayload, Error> result = std::move(image);
  if (pass == nullptr || fence == nullptr ||
      !SDL_WaitForGPUFences(device, true, &fence, 1)) {
    result = Fail(ErrorCode::kUnsupported, SdlError("the readback"));
  } else {
    const auto* mapped = static_cast<const std::uint8_t*>(
        SDL_MapGPUTransferBuffer(device, download, false));
    if (mapped == nullptr) {
      result = Fail(ErrorCode::kUnsupported,
                    SdlError("SDL_MapGPUTransferBuffer(readback)"));
    } else {
      result->pixels.assign(mapped, mapped + result->Bytes());
      SDL_UnmapGPUTransferBuffer(device, download);
      ++totals_.readbacks;
    }
  }
  if (fence != nullptr) resources_->ReleaseFence(fence);
  resources_->Retire(download);
  resources_->RunRetireQueue();
  return result;
}

void Renderer2D::ReleaseGpuObjects() {
  SE_ASSERT_GPU_THREAD();
  ReleaseFences();
  RetireDeviceObjects();
  resources_->ReleaseAll();
  composite_drawn_ = false;
  device_->WaitForIdle();
  resources_->RunRetireQueue();
}

std::expected<void, Error> Renderer2D::RecreateGpuObjects(GpuDevice& device) {
  SE_ASSERT_GPU_THREAD();
  device_ = &device;
  if (auto recreated = resources_->RecreateAll(device); !recreated) {
    return recreated;
  }
  indices_dirty_ = true;
  return CreateDeviceObjects();
}

}  // namespace se
