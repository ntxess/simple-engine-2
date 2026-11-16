// Copyright 2026 The simple-engine-2 Authors.
//
// The Inspector's component drawers (ADR-100).
//
// The reflection drawer serves every registered component: it takes the
// component's JSON from the ComponentRegistry's write hook (glaze
// reflection gives the field names and values), draws a widget per field
// and, when one changes, writes the edited document back as the "after" of
// a set-component command whose "before" is the JSON it read. Numbers keep
// their text until edited (game_rampage/json.h), so the command changes
// only the edited field. A number field is edited as a float when the
// component's read hook accepts 0.5 there, else as an integer; the answer
// is probed once per field (on a scratch registry, never the World) and
// kept. Objects with only x and y numbers draw as one two-float widget.
// ComponentInfo::inspect stays unused: game modules cannot link ImGui, so
// no module could supply a drawing hook (ADR-100).
//
// The custom drawers port the legacy visitors (app/editor/
// EditorComponentVisitor.cpp at master) to rampage's components, each edit
// a set-component command built from an edited copy:
//   - Stats (EntityStatus, :127-176): name and value per stat, "-" to drop
//     one, a row to add one ("+" with a non-empty name); names from the
//     World's StatNames, or ones typed in this editor session;
//   - EffectSource (EffectsList, :8-125, and UpdateEntityPolling/Event,
//     :366-404): kind, affected stat (typed as text through imgui_stdlib,
//     where the legacy code wrote through a const_cast of the string's
//     buffer, :39-41), value, duration and interval in milliseconds, "-"
//     per effect and an add row; then polled, the cooldown ("I-Frame
//     duration", ms) and ready-on-start;
//   - Team (TeamTag, :353-364): a combo of the three teams.
// A value drag or a text edit is one undo step (TrackCoalesce).

#ifndef SE_EDITOR_PANELS_COMPONENT_DRAWERS_H_
#define SE_EDITOR_PANELS_COMPONENT_DRAWERS_H_

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/json.h"
#include "se_core/component_registry.h"
#include "se_core/guid.h"
#include "se_core/world.h"
#include "se_editor_panels/panel_support.h"

namespace se::editor {

// What a drawer needs: the World, the entity, where edits go, the text
// edits in progress and the probe.
struct DrawTarget {
  const World* world = nullptr;
  entt::entity entity = entt::null;
  Guid guid;
  EditSink* sink = nullptr;
  TextEdits* text = nullptr;
  WidgetProbe* probe = nullptr;
  // Widgets are drawn disabled (Play.Faulted).
  bool read_only = false;
};

class ReflectionDrawer {
 public:
  // Draws |info|'s component of the target; submits a set-component
  // command when a field changed this frame.
  void Draw(const DrawTarget& target, const ComponentInfo& info);
  // Whether the number at |path| in |info|'s JSON |root| is a float field
  // (probed and kept).
  [[nodiscard]] bool IsFloatField(const ComponentInfo& info,
                                  const rampage::json::Value& root,
                                  const std::vector<std::string>& path);

 private:
  bool DrawValue(const DrawTarget& target, const ComponentInfo& info,
                 const rampage::json::Value& root, rampage::json::Value& value,
                 std::vector<std::string>& path, const std::string& label);

  // "<component>/<path>" -> float.
  std::map<std::string, bool, std::less<>> float_fields_;
};

class StatsDrawer {
 public:
  void Draw(const DrawTarget& target);

 private:
  std::string new_name_;
  float new_value_ = 0.0F;
  // Stat names typed here (StatIdOf -> name), for ids the World's
  // StatNames does not know yet.
  std::map<rampage::StatId, std::string> typed_names_;

  friend class EffectSourceDrawer;
};

class EffectSourceDrawer {
 public:
  explicit EffectSourceDrawer(StatsDrawer* names) noexcept : names_(names) {}
  void Draw(const DrawTarget& target);

 private:
  StatsDrawer* names_;
  int new_kind_ = 0;
  std::string new_stat_;
  float new_value_ = 0.0F;
  std::int64_t new_duration_ms_ = 0;
  std::int64_t new_interval_ms_ = 0;
};

void DrawTeam(const DrawTarget& target);

// The name of stat |id|: the World's StatNames, |typed|, or "#<id>".
[[nodiscard]] std::string StatName(
    const World& world, rampage::StatId id,
    const std::map<rampage::StatId, std::string>* typed = nullptr);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_COMPONENT_DRAWERS_H_
