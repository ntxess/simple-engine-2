// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor_panels/component_drawers.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "se_core/component_registry.h"
#include "se_core/log.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor_panels/panel_support.h"
#include "se_editor_panels/stat_name_command.h"

namespace se::editor {
namespace {

namespace json = rampage::json;

constexpr float kDragSpeed = 0.5F;

std::string Join(const std::vector<std::string>& path, char separator) {
  std::string out;
  for (const std::string& step : path) {
    if (!out.empty()) out += separator;
    // Array indices share one probe and one probe key per field.
    out += step.starts_with('#') && separator == '/' ? std::string("#") : step;
  }
  return out;
}

json::Value* Navigate(json::Value& root, const std::vector<std::string>& path) {
  json::Value* node = &root;
  for (const std::string& step : path) {
    if (step.starts_with('#')) {
      std::size_t index = 0;
      for (const char c : step.substr(1)) {
        index = index * 10 + static_cast<std::size_t>(c - '0');
      }
      if (!node->IsArray() || index >= node->Items().size()) return nullptr;
      node = &node->MutableItems()[index];
    } else {
      node = node->Find(step);
      if (node == nullptr) return nullptr;
    }
  }
  return node;
}

// The registry's own JSON for |json| (read into a scratch registry and
// written back), or nullopt when the hook rejects it.
std::optional<std::string> Canonical(const ComponentInfo& info,
                                     std::string_view text) {
  entt::registry scratch;
  const entt::entity e = scratch.create();
  if (info.precreate != nullptr) info.precreate(scratch);
  if (!info.read_json(scratch, e, text)) return std::nullopt;
  auto out = info.write_json(scratch, e);
  if (!out) return std::nullopt;
  return std::move(*out);
}

bool IsVec2(const json::Value& value) {
  if (!value.IsObject() || value.Members().size() != 2) return false;
  const json::Value* x = value.Find("x");
  const json::Value* y = value.Find("y");
  return x != nullptr && y != nullptr && x->IsNumber() && y->IsNumber();
}

float AsFloat(const json::Value& value) {
  return static_cast<float>(value.AsDouble().value_or(0.0));
}

void Probe(const DrawTarget& target, std::string key) {
  if (target.probe != nullptr) target.probe->RecordLastItem(std::move(key));
}

// A column header cut to its |width| (an ellipsis, the whole text as a
// tooltip): the item is exactly the column, so a header never runs into
// the next one in a narrow docked Inspector.
void HeaderCell(const char* text, float width) {
  const ImVec2 min = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetTextLineHeight();
  const ImVec2 max(min.x + width, min.y + height);
  ImGui::Dummy(ImVec2(width, height));
  ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), min, max, max.x, text,
                            nullptr, nullptr);
  if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text).x > width) {
    ImGui::SetTooltip("%s", text);
  }
}

void SubmitValue(const DrawTarget& target, std::string_view component,
                 std::string before, const auto& after) {
  target.sink->Submit(SetFromValue(*target.world, target.guid, component,
                                   std::move(before), after));
}

// Submits the set of |component| from |before| to |after|'s JSON with the
// stat names typed in the edit (|names|): one undo step (ADR-119).
template <class T>
void SubmitWithNames(const DrawTarget& target, std::string_view component,
                     std::string before, const T& after,
                     const std::vector<std::string>& names) {
  auto set = SetFromValue(*target.world, target.guid, component,
                          std::move(before), after);
  if (!set) {
    EditSink::ReportError(set.error());
    return;
  }
  if (names.empty()) {
    target.sink->Submit(std::move(*set));
    return;
  }
  target.sink->Submit(WithStatNames(std::format("Add stat {}", names.front()),
                                    names, std::move(*set)));
}

}  // namespace

bool ReflectionDrawer::IsFloatField(const ComponentInfo& info,
                                    const json::Value& root,
                                    const std::vector<std::string>& path) {
  std::string key = std::string(info.name) + "/" + Join(path, '/');
  if (const auto it = float_fields_.find(key); it != float_fields_.end()) {
    return it->second;
  }
  json::Value probe = root;
  json::Value* node = Navigate(probe, path);
  bool is_float = false;
  if (node != nullptr && node->IsNumber()) {
    *node = json::Value::Number("0.5");
    is_float = Canonical(info, json::Write(probe)).has_value();
  }
  float_fields_.emplace(std::move(key), is_float);
  return is_float;
}

bool ReflectionDrawer::DrawValue(const DrawTarget& target,
                                 const ComponentInfo& info,
                                 const json::Value& root, json::Value& value,
                                 std::vector<std::string>& path,
                                 const std::string& label) {
  const std::string probe_key =
      std::string(info.name) + (path.empty() ? "" : ".") + Join(path, '.');
  const std::string coalesce = std::format("Edit {}", info.name);
  bool changed = false;
  switch (value.GetKind()) {
    case json::Kind::kNull:
      ImGui::TextDisabled("%s: null", label.c_str());
      break;
    case json::Kind::kBool: {
      bool b = value.AsBool();
      if (ImGui::Checkbox(label.c_str(), &b)) {
        value = json::Value::Bool(b);
        changed = true;
      }
      Probe(target, probe_key);
      break;
    }
    case json::Kind::kNumber: {
      if (IsFloatField(info, root, path)) {
        float f = AsFloat(value);
        if (ImGui::DragFloat(label.c_str(), &f, kDragSpeed)) {
          value = json::Value::NumberOf(static_cast<double>(f));
          changed = true;
        }
      } else if (const std::optional<std::int64_t> i = value.AsInt64()) {
        std::int64_t v = *i;
        if (ImGui::DragScalar(label.c_str(), ImGuiDataType_S64, &v, 1.0F)) {
          value = json::Value::NumberOf(v);
          changed = true;
        }
      } else {
        ImGui::Text("%s: %s", label.c_str(), value.Text().c_str());
      }
      Probe(target, probe_key);
      TrackCoalesce(*target.sink, coalesce);
      break;
    }
    case json::Kind::kString: {
      ImGui::PushID(label.c_str());
      ImGui::TextUnformatted(label.c_str());
      ImGui::SameLine();
      const std::optional<std::string> text =
          target.text->Input("##text", value.Text());
      Probe(target, probe_key);
      ImGui::PopID();
      if (text) {
        value = json::Value::String(*text);
        changed = true;
      }
      break;
    }
    case json::Kind::kArray: {
      const std::string title =
          std::format("{} [{}]", label, value.Items().size());
      if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& items = value.MutableItems();
        for (std::size_t i = 0; i < items.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          path.push_back(std::format("#{}", i));
          changed |= DrawValue(target, info, root, items[i], path,
                               std::format("[{}]", i));
          path.pop_back();
          ImGui::PopID();
        }
        ImGui::TreePop();
      }
      break;
    }
    case json::Kind::kObject: {
      if (IsVec2(value)) {
        path.emplace_back("x");
        const bool x_float = IsFloatField(info, root, path);
        path.back() = "y";
        const bool y_float = IsFloatField(info, root, path);
        path.pop_back();
        if (x_float && y_float) {
          float xy[2] = {AsFloat(*value.Find("x")), AsFloat(*value.Find("y"))};
          if (ImGui::DragFloat2(label.c_str(), xy, kDragSpeed)) {
            value.Set("x", json::Value::NumberOf(static_cast<double>(xy[0])));
            value.Set("y", json::Value::NumberOf(static_cast<double>(xy[1])));
            changed = true;
          }
          Probe(target, probe_key);
          TrackCoalesce(*target.sink, coalesce);
          break;
        }
      }
      const bool nested = !path.empty();
      if (nested &&
          !ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        break;
      }
      for (json::Member& member : value.MutableMembers()) {
        ImGui::PushID(member.key.c_str());
        path.push_back(member.key);
        changed |=
            DrawValue(target, info, root, member.value, path, member.key);
        path.pop_back();
        ImGui::PopID();
      }
      if (nested) ImGui::TreePop();
      break;
    }
  }
  return changed;
}

void ReflectionDrawer::Draw(const DrawTarget& target,
                            const ComponentInfo& info) {
  if (info.empty) {
    ImGui::TextDisabled("(a tag: no fields)");
    return;
  }
  auto before = info.write_json(target.world->CReg(), target.entity);
  if (!before) {
    ImGui::TextDisabled("(%s)", before.error().message.c_str());
    return;
  }
  auto document = json::Parse(*before);
  if (!document) {
    ImGui::TextDisabled("(%s)", document.error().message.c_str());
    return;
  }
  const json::Value root = *document;
  std::vector<std::string> path;
  ImGui::PushID(info.name.data(), info.name.data() + info.name.size());
  ImGui::BeginDisabled(target.read_only);
  const bool changed =
      DrawValue(target, info, root, *document, path, std::string(info.name));
  ImGui::EndDisabled();
  ImGui::PopID();
  if (!changed) return;
  const std::optional<std::string> after =
      Canonical(info, json::Write(*document));
  if (!after) {
    SE_LOG_WARN("editor: {} rejected the edited value", info.name);
    return;
  }
  if (*after == *before) return;
  target.sink->Submit(std::make_unique<SetComponentCommand>(
      target.guid, std::string(info.name), std::move(*before), *after));
}

std::string StatName(const World& world, rampage::StatId id) {
  if (const auto* names = world.CReg().ctx().find<rampage::StatNames>()) {
    const std::string_view name = names->Name(id);
    if (!name.empty()) return std::string(name);
  }
  return std::format("#{}", id);
}

void StatsDrawer::Draw(const DrawTarget& target) {
  const World& world = *target.world;
  const auto* current = world.CReg().try_get<rampage::Stats>(target.entity);
  if (current == nullptr) return;
  auto before = ReadComponentJson(world, target.entity, "Stats");
  if (!before) return;
  rampage::Stats stats = *current;
  bool changed = false;
  std::vector<std::string> names;

  ImGui::BeginDisabled(target.read_only);
  const float divider = ImGui::GetContentRegionAvail().x / 3.0F;
  ImGui::TextUnformatted("Status");
  ImGui::SameLine(divider);
  ImGui::TextUnformatted("Value");
  std::optional<std::size_t> discard;
  for (std::size_t i = 0; i < stats.values.size(); ++i) {
    rampage::StatValue& stat = stats.values[i];
    const std::string name = StatName(world, stat.id);
    ImGui::PushID(static_cast<int>(stat.id));
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine(divider);
    ImGui::SetNextItemWidth(divider);
    if (ImGui::DragFloat("##value", &stat.value, kDragSpeed)) changed = true;
    Probe(target, "Stats.value." + name);
    TrackCoalesce(*target.sink, "Edit Stats");
    ImGui::SameLine();
    if (ImGui::SmallButton("-")) discard = i;
    Probe(target, "Stats.remove." + name);
    ImGui::PopID();
  }
  if (discard) {
    stats.values.erase(stats.values.begin() +
                       static_cast<std::ptrdiff_t>(*discard));
    changed = true;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Add New Stat");
  ImGui::SetNextItemWidth(divider);
  ImGui::InputText("##new_stat", &new_name_);
  Probe(target, "Stats.new.name");
  ImGui::SameLine(divider + ImGui::GetStyle().ItemSpacing.x);
  ImGui::SetNextItemWidth(divider / 2.0F);
  ImGui::DragFloat("##new_value", &new_value_, kDragSpeed);
  Probe(target, "Stats.new.value");
  ImGui::SameLine();
  if (ImGui::SmallButton("+") && !new_name_.empty()) {
    const rampage::StatId id = rampage::StatIdOf(new_name_);
    names.push_back(new_name_);
    rampage::SetStat(stats, id, new_value_);
    new_name_.clear();
    new_value_ = 0.0F;
    changed = true;
  }
  Probe(target, "Stats.new.add");
  ImGui::EndDisabled();
  if (changed) {
    SubmitWithNames(target, "Stats", std::move(*before), stats, names);
  }
}

void EffectSourceDrawer::Draw(const DrawTarget& target) {
  static constexpr const char* kKinds[] = {"INSTANT", "OVERTIME", "TEMPTIMED"};
  const World& world = *target.world;
  const auto* current =
      world.CReg().try_get<rampage::EffectSource>(target.entity);
  if (current == nullptr) return;
  auto before = ReadComponentJson(world, target.entity, "EffectSource");
  if (!before) return;
  rampage::EffectSource source = *current;
  bool changed = false;
  std::vector<std::string> names;

  ImGui::BeginDisabled(target.read_only);
  const float column = ImGui::GetContentRegionAvail().x / 6.0F;
  ImGui::PushItemWidth(column);
  // The headers sit over the row widgets' columns (column wide, 10 px
  // apart), each cut to its column.
  const auto header = [&target, column](std::string_view key) {
    static constexpr const char* kHeaders[] = {
        "Effect Type", "Aff. Status", "Modifier", "Max Duration", "Tick Rate"};
    for (int i = 0; i < 5; ++i) {
      if (i > 0) {
        ImGui::SameLine((column + 10.0F) * static_cast<float>(i));
      }
      HeaderCell(kHeaders[i], column);
      Probe(target, std::format("{}.header.{}", key, i));
    }
  };
  header("EffectSource");
  std::optional<std::size_t> discard;
  for (std::size_t i = 0; i < source.effects.size(); ++i) {
    rampage::EffectDef& effect = source.effects[i];
    const std::string key = std::format("EffectSource.{}.", i);
    ImGui::PushID(static_cast<int>(i));
    int kind = static_cast<int>(effect.kind);
    if (ImGui::Combo("##kind", &kind, kKinds, 3)) {
      effect.kind = static_cast<rampage::EffectKind>(kind);
      changed = true;
    }
    Probe(target, key + "kind");
    ImGui::SameLine(column + 10.0F);
    if (const std::optional<std::string> stat = target.text->Input(
            "##stat", StatName(world, effect.stat), column)) {
      effect.stat = rampage::StatIdOf(*stat);
      names.push_back(*stat);
      changed = true;
    }
    Probe(target, key + "stat");
    ImGui::SameLine(column * 2 + 20.0F);
    if (ImGui::DragFloat("##value", &effect.value, kDragSpeed)) changed = true;
    Probe(target, key + "value");
    TrackCoalesce(*target.sink, "Edit EffectSource");
    ImGui::SameLine(column * 3 + 30.0F);
    if (ImGui::DragScalar("##duration", ImGuiDataType_S64, &effect.duration_ms,
                          1.0F)) {
      changed = true;
    }
    Probe(target, key + "duration");
    TrackCoalesce(*target.sink, "Edit EffectSource");
    ImGui::SameLine(column * 4 + 40.0F);
    if (ImGui::DragScalar("##interval", ImGuiDataType_S64, &effect.interval_ms,
                          1.0F)) {
      changed = true;
    }
    Probe(target, key + "interval");
    TrackCoalesce(*target.sink, "Edit EffectSource");
    ImGui::SameLine(column * 5 + 50.0F);
    if (ImGui::SmallButton("-")) discard = i;
    Probe(target, key + "remove");
    ImGui::PopID();
  }
  if (discard) {
    source.effects.erase(source.effects.begin() +
                         static_cast<std::ptrdiff_t>(*discard));
    changed = true;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Add New Effects");
  header("EffectSource.new");
  ImGui::PushID("new");
  ImGui::Combo("##kind", &new_kind_, kKinds, 3);
  ImGui::SameLine(column + 10.0F);
  ImGui::InputText("##stat", &new_stat_);
  Probe(target, "EffectSource.new.stat");
  ImGui::SameLine(column * 2 + 20.0F);
  ImGui::DragFloat("##value", &new_value_, kDragSpeed);
  ImGui::SameLine(column * 3 + 30.0F);
  ImGui::DragScalar("##duration", ImGuiDataType_S64, &new_duration_ms_, 1.0F);
  ImGui::SameLine(column * 4 + 40.0F);
  ImGui::DragScalar("##interval", ImGuiDataType_S64, &new_interval_ms_, 1.0F);
  ImGui::SameLine(column * 5 + 50.0F);
  if (ImGui::SmallButton("+") && !new_stat_.empty()) {
    rampage::EffectDef effect;
    effect.kind = static_cast<rampage::EffectKind>(new_kind_);
    effect.stat = rampage::StatIdOf(new_stat_);
    names.push_back(new_stat_);
    effect.value = new_value_;
    effect.duration_ms = new_duration_ms_;
    effect.interval_ms = new_interval_ms_;
    source.effects.push_back(effect);
    new_stat_.clear();
    new_value_ = 0.0F;
    new_duration_ms_ = 0;
    new_interval_ms_ = 0;
    changed = true;
  }
  Probe(target, "EffectSource.new.add");
  ImGui::PopID();
  ImGui::PopItemWidth();

  // The legacy polled and event-driven sources (UpdateEntityPolling and
  // UpdateEntityEvent): one component with a polled flag since M3.
  ImGui::Spacing();
  if (ImGui::Checkbox("Polled", &source.polled)) changed = true;
  Probe(target, "EffectSource.polled");
  ImGui::SetNextItemWidth(column * 2);
  if (ImGui::DragScalar("I-Frame duration (ms)", ImGuiDataType_S64,
                        &source.cooldown_ms, 1.0F)) {
    changed = true;
  }
  Probe(target, "EffectSource.cooldown");
  TrackCoalesce(*target.sink, "Edit EffectSource");
  if (ImGui::Checkbox("Ready on start", &source.ready_on_start)) changed = true;
  Probe(target, "EffectSource.ready_on_start");
  ImGui::EndDisabled();
  if (changed) {
    SubmitWithNames(target, "EffectSource", std::move(*before), source, names);
  }
}

void DrawTeam(const DrawTarget& target) {
  static constexpr const char* kTeams[] = {"UNAFFILIATED", "FRIENDLY", "ENEMY"};
  const auto* team = target.world->CReg().try_get<rampage::Team>(target.entity);
  if (team == nullptr) return;
  ImGui::BeginDisabled(target.read_only);
  int selected = static_cast<int>(team->id);
  if (ImGui::Combo("Team##team", &selected, kTeams, 3)) {
    auto before = ReadComponentJson(*target.world, target.entity, "Team");
    if (before) {
      rampage::Team after{static_cast<rampage::TeamId>(selected)};
      SubmitValue(target, "Team", std::move(*before), after);
    }
  }
  Probe(target, "Team.id");
  ImGui::EndDisabled();
}

}  // namespace se::editor
