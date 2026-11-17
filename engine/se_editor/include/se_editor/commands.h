// Copyright 2026 The simple-engine-2 Authors.
//
// Editor commands: every Edit-mode change of a document World
// (docs/milestone-specs.md M10a "Commands and undo", the panel interface;
// docs/design/mt-design.md "Editor", edits; ADR-095, ADR-096).
//
// A command changes one World through the ComponentRegistry's JSON hooks
// only, so it works for every game module's components:
//   - set, add and remove a component (before and after JSON);
//   - create and destroy an entity (stored serialized: every registered
//     component it holds, Spawned included, so an undone destroy restores
//     the entity's hash exactly);
//   - path points: insert, move and delete one point of a path component
//     (rampage's "Path": {"points": [{"x": .., "y": ..}], ...}), the
//     waypoint canvas's edits; each stores the path's JSON before and after.
// Apply and Revert run on main with g_sim_owner held and the World's
// MutationGate open (EditDocument opens it, GateReason::kEditorCommand;
// a document World is closed to every other mutation, ADR-096). Revert
// after Apply restores the World's hash; Apply after Revert gives the
// hash after the first Apply. A failed Apply leaves the World as it was.
//
// During Play the same commands become SessionLog entries (ToSession)
// queued against a tick of the play World (PlayEdits, panel.h).
//
// Coalescing: commands submitted between EditorCommands::BeginCoalesce and
// EndCoalesce (a drag) form one undo step; a command Absorbs a later one
// of the same drag when both change the same thing (a set of the same
// component, a move of the same point), so the step keeps the first
// before and the last after.

#ifndef SE_EDITOR_COMMANDS_H_
#define SE_EDITOR_COMMANDS_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/session_log.h"

namespace se {
class ComponentRegistry;
}  // namespace se

namespace se::editor {

enum class CommandKind : std::uint8_t {
  kSetComponent,
  kAddComponent,
  kRemoveComponent,
  kCreateEntity,
  kDestroyEntity,
  kInsertPathPoint,
  kMovePathPoint,
  kDeletePathPoint,
  // Several commands as one undo step (a coalesced drag, a multi-edit).
  kGroup,
  // A command a game-aware module defines (M11b: se_editor_panels'
  // StatNameCommand, which records a stat's name in the World's ctx).
  kGame,
};

[[nodiscard]] std::string_view CommandKindName(CommandKind kind) noexcept;

// One component of an entity: its registered name and its JSON value.
struct ComponentJson {
  std::string component;
  std::string json;

  friend bool operator==(const ComponentJson&, const ComponentJson&) = default;
};

// The component path-point commands edit by default (rampage's Path).
inline constexpr std::string_view kPathComponent = "Path";

class EditorCommand {
 public:
  EditorCommand() = default;
  EditorCommand(const EditorCommand&) = delete;
  EditorCommand& operator=(const EditorCommand&) = delete;
  virtual ~EditorCommand() = default;

  [[nodiscard]] virtual CommandKind Kind() const noexcept = 0;
  // For the undo menu and the log ("Set Transform", "Move path point").
  [[nodiscard]] virtual std::string_view Label() const noexcept = 0;
  // The entity the command changes (nil for a group of several).
  [[nodiscard]] virtual Guid Target() const noexcept = 0;

  [[nodiscard]] virtual std::expected<void, Error> Apply(World& world)
      SE_REQUIRES(g_sim_owner) = 0;
  [[nodiscard]] virtual std::expected<void, Error> Revert(World& world)
      SE_REQUIRES(g_sim_owner) = 0;

  // The change as SessionLog entries applied at the boundary before
  // |apply_before| (edits during Play).
  [[nodiscard]] virtual std::expected<std::vector<SessionEntry>, Error>
  ToSession(const ComponentRegistry& registry, Tick apply_before) const = 0;

  // Takes |next|, a later command of the same coalesce scope, into this
  // one; false when the two change different things.
  [[nodiscard]] virtual bool Absorb(const EditorCommand& /*next*/) {
    return false;
  }
};

// Sets an entity's component to |after| (replacing it, or adding it when
// the entity lacks it; Revert then removes it again). Before() is read
// again when the command first applies, so commands made against one
// World state and applied in order each undo to their own start.
class SetComponentCommand final : public EditorCommand {
 public:
  // |before| empty: the entity did not have the component.
  SetComponentCommand(Guid target, std::string component, std::string before,
                      std::string after);
  // Reads |before| from |world|.
  [[nodiscard]] static std::expected<std::unique_ptr<SetComponentCommand>,
                                     Error>
  Make(const World& world, Guid target, std::string_view component,
       std::string after);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kSetComponent;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return label_;
  }
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;
  // A set of the same component of the same entity.
  [[nodiscard]] bool Absorb(const EditorCommand& next) override;

  [[nodiscard]] const std::string& Component() const noexcept {
    return component_;
  }
  [[nodiscard]] const std::string& Before() const noexcept { return before_; }
  [[nodiscard]] const std::string& After() const noexcept { return after_; }

 private:
  Guid target_;
  std::string component_;
  std::string before_;
  std::string after_;
  std::string label_;
  // Apply ran once: before_ is the value it replaced.
  bool applied_ = false;
};

// Adds a component the entity does not have.
class AddComponentCommand final : public EditorCommand {
 public:
  AddComponentCommand(Guid target, std::string component, std::string json);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kAddComponent;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return label_;
  }
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

  [[nodiscard]] const std::string& Component() const noexcept {
    return component_;
  }
  [[nodiscard]] const std::string& Json() const noexcept { return json_; }

 private:
  Guid target_;
  std::string component_;
  std::string json_;
  std::string label_;
};

// Removes a component; its JSON is taken when it applies, for Revert.
class RemoveComponentCommand final : public EditorCommand {
 public:
  RemoveComponentCommand(Guid target, std::string component);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kRemoveComponent;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return label_;
  }
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

  [[nodiscard]] const std::string& Component() const noexcept {
    return component_;
  }
  // The removed value (after Apply).
  [[nodiscard]] const std::string& Before() const noexcept { return before_; }

 private:
  Guid target_;
  std::string component_;
  std::string before_;
  std::string label_;
};

// Creates an entity with |components| (IdComponent comes from the Guid;
// a Spawned value, when given, replaces the one Create records).
class CreateEntityCommand final : public EditorCommand {
 public:
  CreateEntityCommand(Guid target, std::vector<ComponentJson> components);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kCreateEntity;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return "Create entity";
  }
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

  [[nodiscard]] const std::vector<ComponentJson>& Components() const noexcept {
    return components_;
  }

 private:
  Guid target_;
  std::vector<ComponentJson> components_;
};

// Destroys an entity; it is stored serialized when it applies, for Revert.
class DestroyEntityCommand final : public EditorCommand {
 public:
  explicit DestroyEntityCommand(Guid target);

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kDestroyEntity;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return "Destroy entity";
  }
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

  // The destroyed entity's components (after Apply).
  [[nodiscard]] const std::vector<ComponentJson>& Components() const noexcept {
    return components_;
  }

 private:
  Guid target_;
  std::vector<ComponentJson> components_;
};

// One point of a path component: inserted before |index| (index == the
// point count appends), moved, or deleted. Made from the World the panel
// sees: the path's JSON before and after the edit is computed then, and
// again from the World when the command first applies (an earlier command
// of the same frame may have changed the path), so undo inverts the Apply.
class PathPointCommand final : public EditorCommand {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<PathPointCommand>, Error>
  Insert(const World& world, Guid target, std::uint32_t index, Vec2 point,
         std::string_view component = kPathComponent);
  [[nodiscard]] static std::expected<std::unique_ptr<PathPointCommand>, Error>
  Move(const World& world, Guid target, std::uint32_t index, Vec2 point,
       std::string_view component = kPathComponent);
  [[nodiscard]] static std::expected<std::unique_ptr<PathPointCommand>, Error>
  Delete(const World& world, Guid target, std::uint32_t index,
         std::string_view component = kPathComponent);

  [[nodiscard]] CommandKind Kind() const noexcept override { return kind_; }
  [[nodiscard]] std::string_view Label() const noexcept override;
  [[nodiscard]] Guid Target() const noexcept override { return target_; }
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;
  // A later move of the same point (a drag).
  [[nodiscard]] bool Absorb(const EditorCommand& next) override;

  [[nodiscard]] const std::string& Component() const noexcept {
    return component_;
  }
  [[nodiscard]] std::uint32_t Index() const noexcept { return index_; }
  // The inserted or moved-to point (the deleted one for a delete).
  [[nodiscard]] Vec2 Point() const noexcept { return point_; }
  [[nodiscard]] const std::string& Before() const noexcept { return before_; }
  [[nodiscard]] const std::string& After() const noexcept { return after_; }

  // The points of a path component's JSON ({"points": [{"x", "y"}, ...]}).
  [[nodiscard]] static std::expected<std::vector<Vec2>, Error> PointsOf(
      std::string_view json);

 private:
  PathPointCommand(CommandKind kind, Guid target, std::string component,
                   std::uint32_t index, Vec2 point, std::string before,
                   std::string after);
  [[nodiscard]] static std::expected<std::unique_ptr<PathPointCommand>, Error>
  Make(CommandKind kind, const World& world, Guid target, std::uint32_t index,
       Vec2 point, std::string_view component);
  // The path JSON after |kind| at |index| of |before|'s points; a delete
  // sets |point| to the deleted point.
  [[nodiscard]] static std::expected<std::string, Error> Edit(
      CommandKind kind, std::string_view before, std::uint32_t index,
      Vec2* point);

  CommandKind kind_;
  Guid target_;
  std::string component_;
  std::uint32_t index_;
  Vec2 point_;
  std::string before_;
  std::string after_;
  // Apply ran once: before_ and after_ are from the path it changed.
  bool applied_ = false;
};

// Commands applied in order and reverted in reverse, as one undo step.
class CommandGroup final : public EditorCommand {
 public:
  explicit CommandGroup(std::string label);

  // Adds |command| (already applied, when the group is built from applied
  // commands): absorbed by the last one when it can be.
  void Add(std::unique_ptr<EditorCommand> command);
  [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] const EditorCommand& At(std::size_t index) const noexcept {
    return *commands_[index];
  }
  // The single command when the group holds one (the group is then
  // unwrapped by the undo stack).
  [[nodiscard]] std::unique_ptr<EditorCommand> TakeSingle();

  [[nodiscard]] CommandKind Kind() const noexcept override {
    return CommandKind::kGroup;
  }
  [[nodiscard]] std::string_view Label() const noexcept override {
    return label_;
  }
  // The common target, or nil.
  [[nodiscard]] Guid Target() const noexcept override;
  [[nodiscard]] std::expected<void, Error> Apply(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<void, Error> Revert(World& world) override
      SE_REQUIRES(g_sim_owner);
  [[nodiscard]] std::expected<std::vector<SessionEntry>, Error> ToSession(
      const ComponentRegistry& registry, Tick apply_before) const override;

 private:
  std::string label_;
  std::vector<std::unique_ptr<EditorCommand>> commands_;
};

// The registered components |entity| holds, as JSON, in registration
// order, without IdComponent and DestroyRequest: what DestroyEntityCommand
// stores and CreateEntityCommand restores.
[[nodiscard]] std::expected<std::vector<ComponentJson>, Error> SerializeEntity(
    const World& world, Guid target);

}  // namespace se::editor

#endif  // SE_EDITOR_COMMANDS_H_
