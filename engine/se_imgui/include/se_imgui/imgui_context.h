// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's Dear ImGui context (docs/milestone-specs.md M10a, se_imgui;
// ADR-095). One context, created and used on main only: GImGui is
// thread_local (se_imgui/imconfig_se.h), so the context exists only on
// the thread that created it.
//
// Context::Create makes the context with the editor's settings: docking
// on, gamepad navigation on (never moving the OS cursor), multi-viewports
// off unless asked for, the dynamic font atlas with ImGui's default font,
// and no ini file: ImGui's settings (the dock layout and window places)
// are read from and written to memory, and the editor persists them under
// the pref path through the background lane.
//
// HeadlessFrames runs ImGui frames without a window or a GPU: the display
// size and delta are given, and the textures ImGui asks a renderer for
// (the font atlas pages) are marked created with fake ids. The editor's
// headless tests and M10b's panel tests draw through it, feeding input
// with ImGui's own IO calls (io.AddMousePosEvent and friends), never OS
// input.

#ifndef SE_IMGUI_IMGUI_CONTEXT_H_
#define SE_IMGUI_IMGUI_CONTEXT_H_

#include <imgui.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::imgui {

struct ContextConfig {
  // ImGuiConfigFlags_DockingEnable.
  bool docking = true;
  // ImGuiConfigFlags_NavEnableGamepad (with the SDL3 backend's first
  // gamepad). NavEnableSetMousePos stays off: ImGui never moves the cursor.
  bool gamepad_nav = true;
  // ImGuiConfigFlags_ViewportsEnable: ImGui windows dragged out of the main
  // window become platform windows, created and drawn on main. Opt-in.
  bool viewports = false;
  // Settings to start from (ImGui's ini text: the dock layout, window
  // places); empty: none, the editor seeds its layout.
  std::string settings;
  // The base font size in pixels (0: ImGui's default).
  float font_size = 0.0F;
};

class Context {
 public:
  // Fails when a context already exists on this thread.
  [[nodiscard]] static std::expected<std::unique_ptr<Context>, Error> Create(
      const ContextConfig& config) SE_REQUIRES(g_main);

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Destroys the context (after the backends shut down).
  ~Context();

  [[nodiscard]] ImGuiContext* Get() const noexcept { return context_; }
  [[nodiscard]] const ContextConfig& Config() const noexcept { return config_; }

  // ImGui wants its settings saved (io.WantSaveIniSettings): a window
  // moved or a dock node changed.
  [[nodiscard]] bool WantSaveSettings() const noexcept;
  // The current settings as ini text; clears WantSaveSettings.
  [[nodiscard]] std::string SaveSettings() SE_REQUIRES(g_main);
  // Replaces the settings (before the first frame).
  void LoadSettings(std::string_view settings) SE_REQUIRES(g_main);

 private:
  Context(ImGuiContext* context, ContextConfig config) noexcept
      : context_(context), config_(std::move(config)) {}

  ImGuiContext* context_;
  ContextConfig config_;
};

// The calling thread's ImGui context, or null: every thread but the one
// that created the context sees null.
[[nodiscard]] ImGuiContext* CurrentContext() noexcept;

// Frames without a window or a GPU. Marks the context's renderer as one
// that takes texture requests (ImGuiBackendFlags_RendererHasTextures) and
// answers them itself.
class HeadlessFrames {
 public:
  explicit HeadlessFrames(Context& context) SE_REQUIRES(g_main);
  HeadlessFrames(const HeadlessFrames&) = delete;
  HeadlessFrames& operator=(const HeadlessFrames&) = delete;
  ~HeadlessFrames();

  // io.DisplaySize and io.DeltaTime, then ImGui::NewFrame.
  void NewFrame(float width, float height, double delta_sec)
      SE_REQUIRES(g_main);
  // ImGui::Render, then every texture request is answered: created (a
  // fake id), updated or destroyed. Returns the frame's draw data.
  ImDrawData* Render() SE_REQUIRES(g_main);

  // Textures created so far (the font atlas's pages).
  [[nodiscard]] std::uint32_t TexturesCreated() const noexcept {
    return created_;
  }

 private:
  Context* context_;
  std::uint32_t created_ = 0;
};

}  // namespace se::imgui

#endif  // SE_IMGUI_IMGUI_CONTEXT_H_
