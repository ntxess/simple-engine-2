// Copyright 2026 The simple-engine-2 Authors.

#include "editor_panels/support/panel_harness.h"

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/panel.h"
#include "se_imgui/imgui_context.h"

namespace se::testing {

bool RecordingCommands::ApplyPending(World& world) {
  bool ok = true;
  for (; applied_ < commands.size(); ++applied_) {
    const MutationGate gate = world.OpenGate(GateReason::kEditorCommand);
    if (auto applied = commands[applied_]->Apply(world); !applied) {
      std::fprintf(stderr, "command %s failed: %s\n",
                   std::string(commands[applied_]->Label()).c_str(),
                   applied.error().message.c_str());
      ok = false;
    }
  }
  return ok;
}

PanelHarness::PanelHarness() {
  auto context = imgui::Context::Create(imgui::ContextConfig{});
  if (!context) {
    std::fprintf(stderr, "ImGui context: %s\n",
                 context.error().message.c_str());
    std::abort();
  }
  context_ = std::move(*context);
  // Synthetic events apply in the frame they are queued for, in order.
  ImGui::GetIO().ConfigInputTrickleEventQueue = false;
  frames_ = std::make_unique<imgui::HeadlessFrames>(*context_);
}

PanelHarness::~PanelHarness() {
  frames_.reset();
  context_.reset();
}

void PanelHarness::Frame(std::span<editor::Panel* const> panels,
                         const editor::PanelContext& context) {
  frames_->NewFrame(kDisplayWidth, kDisplayHeight, 1.0 / 60.0);
  for (std::size_t i = 0; i < panels.size(); ++i) {
    // The first panel fills the display; the others share a strip below
    // it, so the first one's widgets are never covered.
    if (i == 0) {
      ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
      ImGui::SetNextWindowSize({first_width_, kDisplayHeight},
                               ImGuiCond_Always);
    } else {
      const float w = kDisplayWidth / static_cast<float>(panels.size());
      ImGui::SetNextWindowPos({w * static_cast<float>(i), kDisplayHeight},
                              ImGuiCond_Always);
      ImGui::SetNextWindowSize({w, 300.0F}, ImGuiCond_Always);
    }
    const std::string title(panels[i]->Name());
    ImGui::Begin(title.c_str());
    panels[i]->Draw(context);
    ImGui::End();
  }
  frames_->Render();
}

void PanelHarness::MoveMouse(ImVec2 pos) {
  ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
}

void PanelHarness::Button(int button, bool down) {
  ImGui::GetIO().AddMouseButtonEvent(button, down);
}

void PanelHarness::Wheel(float notches) {
  ImGui::GetIO().AddMouseWheelEvent(0.0F, notches);
}

void PanelHarness::Key(ImGuiKey key, bool down) {
  ImGui::GetIO().AddKeyEvent(key, down);
}

void PanelHarness::Type(std::string_view text) {
  ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
}

void PanelHarness::Click(editor::Panel& panel,
                         const editor::PanelContext& context, ImVec2 pos,
                         int button) {
  // The mouse arrives first, moving (ImGui ignores mouse hover after a
  // jump from no position until the mouse moves, as when keyboard or
  // gamepad navigation took over).
  MoveMouse({pos.x + 1.0F, pos.y + 1.0F});
  Frame(panel, context);
  MoveMouse(pos);
  Frame(panel, context);
  Button(button, true);
  Frame(panel, context);
  Button(button, false);
  Frame(panel, context);
}

}  // namespace se::testing
