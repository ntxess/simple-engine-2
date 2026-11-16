// Copyright 2026 The simple-engine-2 Authors.

#include "se_imgui/imgui_renderer.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/handle.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_platform/driver_wait.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render/renderer_2d.h"

namespace se::imgui {
namespace {

bool IsMouseEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
      return true;
    default:
      return false;
  }
}

bool IsKeyboardEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
      return true;
    default:
      return false;
  }
}

// Grows the backend's vertex and index buffers to hold |vertices| and
// |indices| on |commands| (an upload of nothing in particular: the
// backend sizes its buffers from the draw data's totals).
void GrowBuffers(SDL_GPUCommandBuffer* commands, std::uint32_t vertices,
                 std::uint32_t indices) {
  ImDrawData sizing;
  sizing.Valid = true;
  sizing.DisplaySize = ImVec2(1.0F, 1.0F);
  sizing.FramebufferScale = ImVec2(1.0F, 1.0F);
  sizing.TotalVtxCount = static_cast<int>(vertices);
  sizing.TotalIdxCount = static_cast<int>(indices);
  sizing.Textures = nullptr;
  ImGui_ImplSDLGPU3_PrepareDrawData(&sizing, commands);
}

}  // namespace

std::expected<std::unique_ptr<ImGuiRenderer>, Error> ImGuiRenderer::Create(
    Context& context, GpuDevice& device, SDL_Window* window,
    Renderer2D& renderer) {
  AssertMainThread();
  SE_ASSERT_MSG(ImGui::GetCurrentContext() == context.Get(),
                "ImGuiRenderer needs its context current");
  if (window == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "ImGuiRenderer needs a window");
  }
  std::unique_ptr<ImGuiRenderer> imgui(
      new ImGuiRenderer(context, device, window, renderer));
  if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
    return Fail(ErrorCode::kUnsupported, "ImGui_ImplSDL3_InitForSDLGPU failed");
  }
  imgui->platform_ = true;
  if (auto made = imgui->InitRendererBackend(); !made) {
    return std::unexpected(std::move(made.error()));
  }
  SE_LOG_INFO(
      "ImGui {}: SDL3 platform and SDL_GPU renderer backends, {} vertices "
      "and {} indices pre-sized{}",
      IMGUI_VERSION, imgui->stats_.vertex_capacity,
      imgui->stats_.index_capacity,
      (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0
          ? ", multi-viewports on"
          : "");
  return imgui;
}

ImGuiRenderer::~ImGuiRenderer() { Shutdown(); }

std::expected<void, Error> ImGuiRenderer::InitRendererBackend() {
  ImGui_ImplSDLGPU3_InitInfo info;
  info.Device = device_->Native();
  info.ColorTargetFormat =
      SDL_GetGPUSwapchainTextureFormat(device_->Native(), window_);
  info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  if (!ImGui_ImplSDLGPU3_Init(&info)) {
    return Fail(ErrorCode::kUnsupported, "ImGui_ImplSDLGPU3_Init failed");
  }
  renderer_backend_ = true;
  Presize();
  return {};
}

void ImGuiRenderer::Presize() {
  SDL_GPUCommandBuffer* commands =
      SDL_AcquireGPUCommandBuffer(device_->Native());
  if (commands == nullptr) {
    SE_LOG_WARN("ImGui: no command buffer to pre-size its buffers: {}",
                SDL_GetError());
    return;
  }
  GrowBuffers(commands, kInitialVertices, kInitialIndices);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    SE_LOG_WARN("ImGui: pre-sizing submit failed: {}", SDL_GetError());
  }
  stats_.vertex_capacity = kInitialVertices;
  stats_.index_capacity = kInitialIndices;
}

void ImGuiRenderer::Shutdown() noexcept {
  if (renderer_ != nullptr) renderer_->ForgetOverlay(this);
  if (ImGui::GetCurrentContext() != context_->Get()) return;
  if (renderer_backend_) {
    ImGui_ImplSDLGPU3_Shutdown();
    renderer_backend_ = false;
  }
  if (platform_) {
    ImGui_ImplSDL3_Shutdown();
    platform_ = false;
  }
}

bool ImGuiRenderer::ProcessEvent(const SDL_Event& event) const {
  AssertMainThread();
  if (!platform_) return false;
  ImGui_ImplSDL3_ProcessEvent(&event);
  const ImGuiIO& io = ImGui::GetIO();
  if (IsMouseEvent(event)) return io.WantCaptureMouse;
  if (IsKeyboardEvent(event)) return io.WantCaptureKeyboard;
  return false;
}

void ImGuiRenderer::NewFrame(double delta_sec) {
  AssertMainThread();
  ++stats_.frames_started;
  if (stats_.growths_logged != stats_.vertex_growths + stats_.index_growths) {
    // Logged here, outside the frame's noexcept recording.
    stats_.growths_logged = stats_.vertex_growths + stats_.index_growths;
    SE_LOG_WARN(
        "ImGui buffers grew to {} vertices and {} indices (the frames needed "
        "up to {} and {})",
        stats_.vertex_capacity, stats_.index_capacity, stats_.max_vertices,
        stats_.max_indices);
  }
  if (renderer_backend_) ImGui_ImplSDLGPU3_NewFrame();
  if (platform_) ImGui_ImplSDL3_NewFrame();
  // The real frame delta, in seconds (the legacy editor passed
  // milliseconds as seconds, app/editor/Editor.cpp:136).
  ImGui::GetIO().DeltaTime =
      delta_sec > 0.0 ? static_cast<float>(delta_sec) : 1e-3F;
  ImGui::NewFrame();
}

ImDrawData* ImGuiRenderer::Render() {
  AssertMainThread();
  ImGui::Render();
  ++stats_.frames;
  if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
    ImGui::UpdatePlatformWindows();
  }
  return ImGui::GetDrawData();
}

void ImGuiRenderer::RenderPlatformWindows() {
  AssertMainThread();
  if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
    // The backend acquires and presents each extra window's swapchain: a
    // hang in there is a GPU or compositor stall to the watchdog (M12).
    const DriverWaitScope in_driver;
    ImGui::RenderPlatformWindowsDefault();
  }
}

ImTextureID ImGuiRenderer::TextureId(TextureHandle texture) {
  AssertMainThread();
  const ResolvedTexture resolved = renderer_->Resources().Resolve(texture);
  return static_cast<ImTextureID>(
      reinterpret_cast<std::uintptr_t>(resolved.texture));
}

ImVec2 ImGuiRenderer::TextureSize(TextureHandle texture) {
  AssertMainThread();
  const ResolvedTexture resolved = renderer_->Resources().Resolve(texture);
  return ImVec2(static_cast<float>(resolved.width),
                static_cast<float>(resolved.height));
}

void ImGuiRenderer::ReleaseDevice() noexcept {
  if (renderer_backend_ && ImGui::GetCurrentContext() == context_->Get()) {
    ImGui_ImplSDLGPU3_Shutdown();
    renderer_backend_ = false;
  }
}

std::expected<void, Error> ImGuiRenderer::RecreateDevice(GpuDevice& device) {
  AssertMainThread();
  device_ = &device;
  if (renderer_backend_) return {};
  return InitRendererBackend();
}

void ImGuiRenderer::BeforeFrame() noexcept {
  if (!renderer_backend_) return;
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr || data->Textures == nullptr) return;
  // Each update is a command buffer of its own, submitted now: before the
  // frame's command buffer, which samples the textures.
  for (ImTextureData* texture : *data->Textures) {
    if (texture->Status != ImTextureStatus_OK) {
      ImGui_ImplSDLGPU3_UpdateTexture(texture);
      ++stats_.texture_updates;
    }
  }
}

void ImGuiRenderer::Prepare(SDL_GPUCommandBuffer* commands) noexcept {
  if (!renderer_backend_ || commands == nullptr) return;
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr || !data->Valid) return;
  const auto vertices = static_cast<std::uint32_t>(data->TotalVtxCount);
  const auto indices = static_cast<std::uint32_t>(data->TotalIdxCount);
  stats_.max_vertices = std::max(stats_.max_vertices, vertices);
  stats_.max_indices = std::max(stats_.max_indices, indices);
  if (vertices > stats_.vertex_capacity || indices > stats_.index_capacity) {
    // The backend recreates a buffer that is too small at the size the
    // frame needs; grow with headroom instead, once, and say so.
    const std::uint32_t grown_vertices = std::max(
        stats_.vertex_capacity, std::max(vertices, stats_.vertex_capacity * 2));
    const std::uint32_t grown_indices = std::max(
        stats_.index_capacity, std::max(indices, stats_.index_capacity * 2));
    if (grown_vertices != stats_.vertex_capacity) ++stats_.vertex_growths;
    if (grown_indices != stats_.index_capacity) ++stats_.index_growths;
    // Logged at the next NewFrame (no logging in the recording steps).
    GrowBuffers(commands, grown_vertices, grown_indices);
    stats_.vertex_capacity = grown_vertices;
    stats_.index_capacity = grown_indices;
  }
  // Texture updates went out in BeforeFrame: nothing is left for the
  // backend to upload on this command buffer.
  ImGui_ImplSDLGPU3_PrepareDrawData(data, commands);
}

void ImGuiRenderer::Draw(SDL_GPUCommandBuffer* commands,
                         SDL_GPURenderPass* pass, std::uint32_t /*width*/,
                         std::uint32_t /*height*/) noexcept {
  if (!renderer_backend_ || commands == nullptr || pass == nullptr) return;
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr || !data->Valid) return;
  CheckBoundTextures(*data);
  ImGui_ImplSDLGPU3_RenderDrawData(data, commands, pass);
}

void ImGuiRenderer::CheckBoundTextures(const ImDrawData& data) noexcept {
#if SE_ASSERTS
  const GpuResources& resources = renderer_->Resources();
  for (const ImDrawList* list : data.CmdLists) {
    for (const ImDrawCmd& command : list->CmdBuffer) {
      // ImGui's own textures (the font atlas) are the backend's.
      if (command.UserCallback != nullptr ||
          command.TexRef._TexData != nullptr) {
        continue;
      }
      // The SDL_GPU backend's ImTextureID is the texture's address.
      const auto* texture =
          reinterpret_cast<  // NOLINT(performance-no-int-to-ptr)
              const SDL_GPUTexture*>(
              static_cast<std::uintptr_t>(command.TexRef._TexID));
      ++stats_.checked_textures;
      SE_ASSERT_MSG(!resources.ReleasedThisFrame(texture),
                    "ImGui binds a texture released this frame: keep a "
                    "Handle<Texture> and ask TextureIds every frame");
    }
  }
#else
  static_cast<void>(data);
#endif
}

}  // namespace se::imgui
