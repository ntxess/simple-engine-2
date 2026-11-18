// Copyright 2026 The simple-engine-2 Authors.

#include "se_imgui/imgui_context.h"

#include <imgui.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"

// ImGui's context pointer, one per thread (se_imgui/imconfig_se.h).
thread_local ImGuiContext* g_se_imgui_context = nullptr;

namespace se::imgui {

std::expected<std::unique_ptr<Context>, Error> Context::Create(
    const ContextConfig& config) {
  AssertMainThread();
  if (ImGui::GetCurrentContext() != nullptr) {
    return Fail(ErrorCode::kInvalidArgument,
                "an ImGui context already exists on this thread");
  }
  IMGUI_CHECKVERSION();
  ImGuiContext* context = ImGui::CreateContext();
  if (context == nullptr) {
    return Fail(ErrorCode::kUnsupported, "ImGui::CreateContext failed");
  }
  ImGui::SetCurrentContext(context);
  ImGuiIO& io = ImGui::GetIO();
  // Settings live in memory; the editor persists them through the lane.
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  if (config.docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  if (config.gamepad_nav) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  if (config.viewports) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  if (config.font_size > 0.0F) style.FontSizeBase = config.font_size;
  io.Fonts->AddFontDefault();
  if (!config.settings.empty()) {
    ImGui::LoadIniSettingsFromMemory(config.settings.data(),
                                     config.settings.size());
  }
  return std::unique_ptr<Context>(new Context(context, config));
}

Context::~Context() {
  AssertMainThread();
  if (context_ != nullptr) ImGui::DestroyContext(context_);
}

bool Context::WantSaveSettings() const noexcept {
  return ImGui::GetCurrentContext() == context_ &&
         ImGui::GetIO().WantSaveIniSettings;
}

std::string Context::SaveSettings() {
  AssertMainThread();
  std::size_t size = 0;
  const char* text = ImGui::SaveIniSettingsToMemory(&size);
  ImGui::GetIO().WantSaveIniSettings = false;
  return text != nullptr ? std::string(text, size) : std::string();
}

void Context::LoadSettings(std::string_view settings) {
  AssertMainThread();
  ImGui::LoadIniSettingsFromMemory(settings.data(), settings.size());
}

ImGuiContext* CurrentContext() noexcept { return ImGui::GetCurrentContext(); }

HeadlessFrames::HeadlessFrames(Context& context) : context_(&context) {
  AssertMainThread();
  SE_ASSERT_MSG(ImGui::GetCurrentContext() == context.Get(),
                "HeadlessFrames needs its context current");
  ImGuiIO& io = ImGui::GetIO();
  SE_ASSERT_MSG(io.BackendRendererName == nullptr,
                "HeadlessFrames on a context with a renderer backend");
  io.BackendRendererName = "se_imgui headless";
  io.BackendPlatformName = "se_imgui headless";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures |
                     ImGuiBackendFlags_RendererHasVtxOffset;
}

HeadlessFrames::~HeadlessFrames() {
  if (ImGui::GetCurrentContext() != context_->Get()) return;
  // Every texture the renderer "holds" goes, as a backend's shutdown does.
  for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
    if (texture->Status != ImTextureStatus_Destroyed) {
      texture->SetTexID(ImTextureID_Invalid);
      texture->SetStatus(ImTextureStatus_Destroyed);
    }
  }
  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = nullptr;
  io.BackendPlatformName = nullptr;
  io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures |
                       ImGuiBackendFlags_RendererHasVtxOffset);
}

void HeadlessFrames::NewFrame(float width, float height, double delta_sec) {
  AssertMainThread();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(width, height);
  io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
  io.DeltaTime = delta_sec > 0.0 ? static_cast<float>(delta_sec) : 1e-3F;
  ImGui::NewFrame();
}

ImDrawData* HeadlessFrames::Render() {
  AssertMainThread();
  ImGui::Render();
  for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
    switch (texture->Status) {
      case ImTextureStatus_WantCreate:
        ++created_;
        // Any non-zero id: nothing samples it.
        texture->SetTexID(static_cast<ImTextureID>(texture->UniqueID) + 1);
        texture->SetStatus(ImTextureStatus_OK);
        break;
      case ImTextureStatus_WantUpdates:
        texture->SetStatus(ImTextureStatus_OK);
        break;
      case ImTextureStatus_WantDestroy:
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
        break;
      default:
        break;
    }
  }
  return ImGui::GetDrawData();
}

}  // namespace se::imgui
