// Copyright 2026 The simple-engine-2 Authors.
//
// se_imgui's context (M10a): GImGui is thread_local, so the main thread's
// context does not exist on another thread; headless frames draw and
// answer the font atlas's texture requests.

#include "se_imgui/imgui_context.h"

#include <imgui.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <thread>

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
