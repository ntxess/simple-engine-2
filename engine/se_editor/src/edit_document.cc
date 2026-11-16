// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/edit_document.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_game.h"

namespace se::editor {

std::expected<std::unique_ptr<EditDocument>, Error> EditDocument::Open(
    const ComponentRegistry& registry, const EditorGame& game,
    DocumentDesc desc, std::string_view text) {
  if (!game.load_scene) {
    return Fail(ErrorCode::kUnsupported, "the game has no scene loader");
  }
  WorldDesc world_desc;
  world_desc.name = desc.name.empty() ? std::string("document") : desc.name;
  world_desc.kind = WorldKind::kEditDocument;
  world_desc.seed = desc.seed;
  world_desc.tick_rate = desc.tick_rate;
  auto world = std::make_unique<World>(registry, std::move(world_desc));
  {
    const MutationGate gate = world->OpenGate(GateReason::kLoad);
    if (auto loaded = game.load_scene(*world, text, desc.asset_root); !loaded) {
      Error error = std::move(loaded.error());
      if (error.path.empty()) error.path = desc.path;
      return std::unexpected(std::move(error));
    }
    world->RecordHash();
  }
  return std::unique_ptr<EditDocument>(
      new EditDocument(game, std::move(desc), std::move(world)));
}

EditDocument::EditDocument(const EditorGame& game, DocumentDesc desc,
                           std::unique_ptr<World> world) noexcept
    : game_(&game), desc_(std::move(desc)), world_(std::move(world)) {}

EditDocument::~EditDocument() = default;

std::expected<void, Error> EditDocument::Apply(
    std::unique_ptr<EditorCommand> command) {
  if (command == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "no command");
  }
  {
    const MutationGate gate = Mutable().OpenGate(GateReason::kEditorCommand);
    if (auto applied = command->Apply(Mutable()); !applied) {
      return applied;
    }
  }
  ++revision_;
  Record(std::move(command));
  selection_.Prune(*world_);
  return {};
}

void EditDocument::Record(std::unique_ptr<EditorCommand> command) {
  undone_.clear();
  if (group_ != nullptr) {
    group_->Add(std::move(command));
    return;
  }
  done_.push_back(std::move(command));
}

void EditDocument::BeginCoalesce(std::string_view label) {
  EndCoalesce();
  group_ = std::make_unique<CommandGroup>(std::string(label));
}

void EditDocument::EndCoalesce() {
  if (group_ == nullptr) return;
  std::unique_ptr<CommandGroup> group = std::move(group_);
  if (group->empty()) return;
  if (std::unique_ptr<EditorCommand> single = group->TakeSingle()) {
    done_.push_back(std::move(single));
    return;
  }
  done_.push_back(std::move(group));
}

std::expected<bool, Error> EditDocument::Undo() {
  EndCoalesce();
  if (done_.empty()) return false;
  std::unique_ptr<EditorCommand> command = std::move(done_.back());
  done_.pop_back();
  {
    const MutationGate gate = Mutable().OpenGate(GateReason::kEditorCommand);
    if (auto reverted = command->Revert(Mutable()); !reverted) {
      SE_LOG_ERROR("undo '{}': {}", command->Label(), reverted.error().message);
      // The step stays where it was: the document may be partly reverted,
      // and its revision says it changed.
      done_.push_back(std::move(command));
      ++revision_;
      return std::unexpected(std::move(reverted.error()));
    }
  }
  ++revision_;
  undone_.push_back(std::move(command));
  selection_.Prune(*world_);
  return true;
}

std::expected<bool, Error> EditDocument::Redo() {
  EndCoalesce();
  if (undone_.empty()) return false;
  std::unique_ptr<EditorCommand> command = std::move(undone_.back());
  undone_.pop_back();
  {
    const MutationGate gate = Mutable().OpenGate(GateReason::kEditorCommand);
    if (auto applied = command->Apply(Mutable()); !applied) {
      SE_LOG_ERROR("redo '{}': {}", command->Label(), applied.error().message);
      ++revision_;
      return std::unexpected(std::move(applied.error()));
    }
  }
  ++revision_;
  done_.push_back(std::move(command));
  selection_.Prune(*world_);
  return true;
}

std::size_t EditDocument::UndoCount() const noexcept {
  return done_.size() + (group_ != nullptr && !group_->empty() ? 1 : 0);
}

std::string_view EditDocument::UndoLabel() const noexcept {
  if (group_ != nullptr && !group_->empty()) return group_->Label();
  return done_.empty() ? std::string_view() : done_.back()->Label();
}

std::string_view EditDocument::RedoLabel() const noexcept {
  return undone_.empty() ? std::string_view() : undone_.back()->Label();
}

void EditDocument::MarkSaved(std::uint64_t revision) noexcept {
  saved_revision_ = revision;
}

std::expected<std::string, Error> EditDocument::Serialize() const {
  if (!game_->save_scene) {
    return Fail(ErrorCode::kUnsupported, "the game has no scene writer");
  }
  return game_->save_scene(*world_, desc_.name);
}

std::uint64_t EditDocument::Hash() const { return world_->Hash().Folded(); }

}  // namespace se::editor
