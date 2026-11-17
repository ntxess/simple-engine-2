// Copyright 2026 The simple-engine-2 Authors.
//
// se_imgui's context (M10a): GImGui is thread_local, so the main thread's
// context does not exist on another thread; headless frames draw and
// answer the font atlas's texture requests; the overlay's display fits a
// smaller swapchain.

#include "se_imgui/imgui_context.h"

#include <imgui.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "se_imgui/imgui_renderer.h"
#include "support/test_roles.h"

namespace se::imgui {
namespace {

TEST_CASE("The ImGui context exists only on the thread that made it",
          "[imgui]") {
  const testing::MainThreadScope main_thread;
  auto context = Context::Create(ContextConfig{});
  REQUIRE(context.has_value());
  REQUIRE(CurrentContext() == (*context)->Get());
  ImGuiContext* seen_elsewhere = (*context)->Get();
  std::thread other([&seen_elsewhere] { seen_elsewhere = CurrentContext(); });
  other.join();
  REQUIRE(seen_elsewhere == nullptr);
  // A second context on the same thread is refused.
  REQUIRE_FALSE(Context::Create(ContextConfig{}).has_value());
}

// The backend's framebuffer for a display, rounded as it rounds.
int Framebuffer(float display, float scale) {
  return static_cast<int>(std::floor(display * scale + 0.5F));
}

TEST_CASE("The overlay's display is cropped only where it does not fit",
          "[imgui]") {
  // It fits: unchanged.
  ImVec2 fitted = FitDisplayToTarget(ImVec2(1280.0F, 720.0F),
                                     ImVec2(1.0F, 1.0F), 1280, 720);
  REQUIRE(fitted.x == 1280.0F);
  REQUIRE(fitted.y == 720.0F);
  // The window grew to 1280 x 720 while the swapchain is 1100 x 640.
  fitted = FitDisplayToTarget(ImVec2(1280.0F, 720.0F), ImVec2(1.0F, 1.0F), 1100,
                              640);
  REQUIRE(Framebuffer(fitted.x, 1.0F) == 1100);
  REQUIRE(Framebuffer(fitted.y, 1.0F) == 640);
  // One axis only.
  fitted = FitDisplayToTarget(ImVec2(1280.0F, 600.0F), ImVec2(1.0F, 1.0F), 1100,
                              640);
  REQUIRE(Framebuffer(fitted.x, 1.0F) == 1100);
  REQUIRE(fitted.y == 600.0F);
  // A high-DPI scale: every target width fits after the crop, and no
  // pixel is lost.
  for (const float scale : {1.25F, 1.5F, 1.75F, 2.0F, 3.0F}) {
    for (std::uint32_t width = 1; width < 3000; width += 7) {
      const ImVec2 at = FitDisplayToTarget(ImVec2(4000.0F, 10.0F),
                                           ImVec2(scale, scale), width, 4000);
      INFO("scale " << scale << " width " << width);
      REQUIRE(Framebuffer(at.x, scale) <= static_cast<int>(width));
      REQUIRE(Framebuffer(at.x, scale) >= static_cast<int>(width) - 1);
    }
  }
  // Nothing to draw (a minimized window, no scale): left alone.
  fitted = FitDisplayToTarget(ImVec2(0.0F, 0.0F), ImVec2(1.0F, 1.0F), 10, 10);
  REQUIRE(fitted.x == 0.0F);
  fitted =
      FitDisplayToTarget(ImVec2(100.0F, 100.0F), ImVec2(0.0F, 0.0F), 10, 10);
  REQUIRE(fitted.x == 100.0F);
}

TEST_CASE("Headless frames draw windows and create the font atlas", "[imgui]") {
  const testing::MainThreadScope main_thread;
  auto context = Context::Create(ContextConfig{});
  REQUIRE(context.has_value());
  REQUIRE((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0);
  REQUIRE((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) !=
          0);
  REQUIRE((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0);
  REQUIRE(ImGui::GetIO().IniFilename == nullptr);
  HeadlessFrames frames(**context);
  for (int frame = 0; frame < 3; ++frame) {
    frames.NewFrame(800.0F, 600.0F, 1.0 / 60.0);
    ImGui::Begin("Test window");
    ImGui::TextUnformatted("hello");
    ImGui::End();
    ImDrawData* data = frames.Render();
    REQUIRE(data != nullptr);
    // A new window is hidden on its first frame while it measures itself.
    if (frame > 0) REQUIRE(data->TotalVtxCount > 0);
  }
  REQUIRE(frames.TexturesCreated() >= 1);
  // The window's place is in the settings, kept in memory.
  const std::string settings = (*context)->SaveSettings();
  REQUIRE(settings.find("[Window][Test window]") != std::string::npos);
}

}  // namespace
}  // namespace se::imgui
