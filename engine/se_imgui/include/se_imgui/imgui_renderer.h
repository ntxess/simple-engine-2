// Copyright 2026 The simple-engine-2 Authors.
//
// ImGuiRenderer: Dear ImGui's SDL3 platform backend and SDL_GPU renderer
// backend in M7's frame (docs/milestone-specs.md M10a, the renderer
// integration; ADR-098). Main thread only.
//
// It is Renderer2D's SwapchainOverlay: ImGui draws in the swapchain pass
// after the composite's blit.
//   - BeforeFrame (before the frame's command buffer): ImGui's texture
//     requests (the dynamic font atlas) are uploaded by the backend, each
//     in a command buffer of its own submitted at once, so they reach the
//     GPU before the frame that samples them.
//   - Prepare (the frame's command buffer, outside any pass):
//     ImGui_ImplSDLGPU3_PrepareDrawData copies the vertices and indices.
//     Its buffers are pre-sized at Create for kInitialVertices and
//     kInitialIndices; a frame that needs more grows them (the backend
//     releases and recreates them) and the growth is logged.
//   - Draw (the swapchain pass): the debug wrapper of RenderDrawData. In
//     builds with asserts it checks every user texture the draw data binds
//     (an ImTextureID from TextureId) against GpuResources' release log:
//     none may have been released this frame (GpuResources::
//     MarkFrameStart at the editor's frame start). RepresentLast draws the
//     same frame's data again. The draw data's display is cropped to the
//     swapchain it is given when it is larger (the window grew before SDL
//     resized the swapchain, R2 of the M10-M14 review; ADR-113).
// TextureId(handle) resolves a GpuResources slot to its texture of this
// frame, as an ImTextureID (the backend's SDL_GPUTexture*).
//
// Input: ProcessEvent feeds ImGui first; it returns whether ImGui wants
// the mouse or the keyboard (in 'Play game' the editor decides by its
// view's focus instead, ADR-113). The platform backend never moves the OS
// cursor (navigation's
// NavEnableSetMousePos stays off) and opens the first gamepad itself.
// Multi-viewports, when the context enables them, render on main after the
// frame (RenderPlatformWindows).
//
// Device loss: ReleaseDevice drops the renderer backend's objects with the
// old device; RecreateDevice initialises it on the new one.

#ifndef SE_IMGUI_IMGUI_RENDERER_H_
#define SE_IMGUI_IMGUI_RENDERER_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <cstdint>
#include <expected>
#include <memory>

#include "se_core/error.h"
#include "se_core/handle.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_imgui/imgui_context.h"
#include "se_render/gpu_device.h"
#include "se_render/gpu_resources.h"
#include "se_render/renderer_2d.h"

namespace se::imgui {

struct RendererStats {
  std::uint64_t frames = 0;
  std::uint64_t frames_started = 0;
  // Growths already reported in the log.
  std::uint32_t growths_logged = 0;
  std::uint64_t texture_updates = 0;
  std::uint32_t vertex_capacity = 0;
  std::uint32_t index_capacity = 0;
  std::uint32_t vertex_growths = 0;
  std::uint32_t index_growths = 0;
  std::uint32_t max_vertices = 0;
  std::uint32_t max_indices = 0;
  // User textures checked against the release log.
  std::uint64_t checked_textures = 0;
  // Draws whose display was cropped to a smaller swapchain.
  std::uint64_t cropped_draws = 0;
};

// The display size to draw |display| (at |scale|, ImGui's FramebufferScale)
// with on a |width| x |height| target: |display| itself when the backend's
// framebuffer fits, otherwise each axis that does not fit shrunk until it
// does. ImGui's pixels keep their place; the rest is cropped.
[[nodiscard]] ImVec2 FitDisplayToTarget(ImVec2 display, ImVec2 scale,
                                        std::uint32_t width,
                                        std::uint32_t height) noexcept;

class ImGuiRenderer final : public SwapchainOverlay {
 public:
  static constexpr std::uint32_t kInitialVertices = 128 * 1024;
  static constexpr std::uint32_t kInitialIndices = 384 * 1024;

  // Both backends, on |context| (current), |window| and |device|, which
  // has claimed the window. |renderer| resolves texture ids and outlives
  // this object.
  [[nodiscard]] static std::expected<std::unique_ptr<ImGuiRenderer>, Error>
  Create(Context& context, GpuDevice& device, SDL_Window* window,
         Renderer2D& renderer) SE_REQUIRES(g_main);

  ImGuiRenderer(const ImGuiRenderer&) = delete;
  ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
  // Shutdown() if it has not run.
  ~ImGuiRenderer() override;

  // Shutdown step 11: ImGui_ImplSDLGPU3_Shutdown, ImGui_ImplSDL3_Shutdown.
  void Shutdown() noexcept SE_REQUIRES(g_main);

  // ImGui first: returns whether ImGui wants the mouse (a mouse event) or
  // the keyboard (a key or text event).
  bool ProcessEvent(const SDL_Event& event) const SE_REQUIRES(g_main);
  // The frame's UI starts: the backends' NewFrame, then ImGui::NewFrame.
  // |delta_sec| is the real frame delta in seconds.
  void NewFrame(double delta_sec) SE_REQUIRES(g_main);
  // The frame's UI ends: ImGui::Render. Platform windows (multi-viewports)
  // are updated afterwards.
  ImDrawData* Render() SE_REQUIRES(g_main);
  // Multi-viewports: draws the platform windows (after the frame).
  void RenderPlatformWindows() SE_REQUIRES(g_main);

  // |texture|'s ImTextureID for this frame (the magenta fallback for a
  // stale or unresident handle).
  [[nodiscard]] ImTextureID TextureId(TextureHandle texture)
      SE_REQUIRES(g_main);
  [[nodiscard]] ImVec2 TextureSize(TextureHandle texture) SE_REQUIRES(g_main);

  // Device loss.
  void ReleaseDevice() noexcept SE_REQUIRES(g_main);
  [[nodiscard]] std::expected<void, Error> RecreateDevice(GpuDevice& device)
      SE_REQUIRES(g_main);

  // SwapchainOverlay.
  void BeforeFrame() noexcept override;
  void Prepare(SDL_GPUCommandBuffer* commands) noexcept override;
  void Draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
            std::uint32_t width, std::uint32_t height) noexcept override;

  [[nodiscard]] const RendererStats& Stats() const noexcept { return stats_; }

 private:
  ImGuiRenderer(Context& context, GpuDevice& device, SDL_Window* window,
                Renderer2D& renderer) noexcept
      : context_(&context),
        device_(&device),
        window_(window),
        renderer_(&renderer) {}

  [[nodiscard]] std::expected<void, Error> InitRendererBackend()
      SE_REQUIRES(g_main);
  // Grows the backend's buffers to the pre-sized capacity at once.
  void Presize() SE_REQUIRES(g_main);
  // The debug wrapper's check (builds with asserts).
  void CheckBoundTextures(const ImDrawData& data) noexcept;

  Context* context_;
  GpuDevice* device_;
  SDL_Window* window_;
  Renderer2D* renderer_;
  bool platform_ = false;
  bool renderer_backend_ = false;
  RendererStats stats_;
};

}  // namespace se::imgui

#endif  // SE_IMGUI_IMGUI_RENDERER_H_
