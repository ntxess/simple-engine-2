// Copyright 2026 The simple-engine-2 Authors.
//
// The Log panel, the legacy log view's layout (app/editor/Editor.cpp:380-427
// at master): a button and the auto-scroll checkbox on top, then the lines
// in a scrolling child, coloured by severity, kept at the bottom while
// auto-scroll is on and the view was already there. The lines come from
// the editor's LogView (formatted once by the log worker); the panel never
// reads the log file, so the legacy "Enable Log Viewer" toggle became Clear.

#include <imgui.h>

#include <cinttypes>
#include <cstddef>
#include <format>
#include <string>

#include "se_core/log.h"
#include "se_editor/log_view.h"
#include "se_editor/panel.h"
#include "se_editor_panels/panels.h"

namespace se::editor {

ImVec4 LogLevelColor(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return {1.0F, 1.0F, 1.0F, 1.0F};  // white
    case LogLevel::kDebug:
    case LogLevel::kInfo:
      return {0.5F, 0.5F, 1.0F, 1.0F};  // light blue
    case LogLevel::kWarn:
      return {1.0F, 1.0F, 0.0F, 1.0F};  // yellow
    case LogLevel::kError:
    case LogLevel::kFatal:
      return {1.0F, 0.0F, 0.0F, 1.0F};  // red
  }
  return {1.0F, 1.0F, 1.0F, 1.0F};
}

void LogPanel::Draw(const PanelContext& context) {
  static constexpr const char* kLevels[] = {"trace", "debug", "info",
                                            "warn",  "error", "fatal"};
  shown_ = Shown{};
  LogView* log = context.log;
  if (log == nullptr) {
    ImGui::TextDisabled("No log");
    return;
  }
  if (ImGui::Button("Clear")) log->Clear();
  ProbeItem("log.clear");
  ImGui::SameLine();
  ImGui::Checkbox("Enable Auto-Scrolling", &auto_scroll_);
  ProbeItem("log.auto_scroll");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0F);
  ImGui::Combo("Level", &min_level_, kLevels, 6);
  ProbeItem("log.level");

  shown_.counts = std::format("{} lines, {} dropped, {} cut by the rate limit",
                              log->Total(), log->Dropped(), log->Suppressed());
  const bool lost = log->Dropped() > 0 || log->Suppressed() > 0;
  if (lost) {
    ImGui::TextColored({1.0F, 1.0F, 0.0F, 1.0F}, "%s", shown_.counts.c_str());
  } else {
    ImGui::TextDisabled("%s", shown_.counts.c_str());
  }
  ProbeItem("log.counts");

  ImGui::BeginChild("##Log", {0.0F, 0.0F}, ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  const auto min_level = static_cast<LogLevel>(min_level_);
  bool first = true;
  for (std::size_t i = 0; i < log->size(); ++i) {
    const LogLine& line = log->At(i);
    if (line.level < min_level) continue;
    ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(line.level));
    ImGui::TextUnformatted(line.text.c_str());
    ImGui::PopStyleColor();
    if (first) shown_.first = i;
    first = false;
    shown_.last = i;
    ++shown_.lines;
  }
  if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.0F);
  }
  ImGui::EndChild();
}

}  // namespace se::editor
