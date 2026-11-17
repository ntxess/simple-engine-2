// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/edit_document.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/component_registry.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_editor/commands.h"
#include "se_editor/editor_game.h"
#include "src/json_value.h"

namespace se::editor {

std::string_view DocumentKindName(DocumentKind kind) noexcept {
  switch (kind) {
    case DocumentKind::kScene:
      return "scene";
    case DocumentKind::kPrefab:
      return "prefab";
  }
  return "?";
}

std::expected<std::unique_ptr<EditDocument>, Error> EditDocument::Open(
    const ComponentRegistry& registry, const EditorGame& game,
    DocumentDesc desc, std::string_view text) {
  const bool prefab = desc.kind == DocumentKind::kPrefab;
  if (prefab ? !game.load_prefab : !game.load_scene) {
    return Fail(ErrorCode::kUnsupported, prefab
                                             ? "the game has no prefab loader"
                                             : "the game has no scene loader");
  }
  WorldDesc world_desc;
  world_desc.name = desc.name.empty() ? std::string("document") : desc.name;
  world_desc.kind = WorldKind::kEditDocument;
  world_desc.seed = desc.seed;
  world_desc.tick_rate = desc.tick_rate;
  auto world = std::make_unique<World>(registry, std::move(world_desc));
  std::map<std::string, std::string, std::less<>> prefabs;
  {
    const MutationGate gate = world->OpenGate(GateReason::kLoad);
    std::expected<void, Error> loaded;
    if (prefab) {
      loaded = game.load_prefab(*world, text, desc.ref, desc.asset_root);
    } else {
      loaded = game.load_scene(
          *world, text, desc.asset_root,
          [&prefabs](std::string_view ref, std::string_view prefab_text) {
            prefabs.insert_or_assign(std::string(ref),
                                     std::string(prefab_text));
          });
    }
    if (!loaded) {
      Error error = std::move(loaded.error());
      if (error.path.empty()) error.path = desc.path;
      return std::unexpected(std::move(error));
    }
    world->RecordHash();
  }
  auto document = std::unique_ptr<EditDocument>(
      new EditDocument(game, std::move(desc), std::move(world)));
  document->prefab_texts_ = std::move(prefabs);
  document->file_text_ = std::string(text);
  return document;
}

EditDocument::EditDocument(const EditorGame& game, DocumentDesc desc,
                           std::unique_ptr<World> world)
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

std::expected<void, Error> EditDocument::ApplyPrefabChange(
    std::unique_ptr<EditorCommand> command) {
  EndCoalesce();
  const bool was_clean = !Dirty();
  if (auto applied = Apply(std::move(command)); !applied) {
    return applied;
  }
  if (!was_clean) return {};
  // Clean before: the change keeps it clean when the file it would write
  // is the file it has (the instances now load as the file says): the
  // same JSON, whatever the file's layout (a hand-written file is not the
  // writer's text byte for byte).
  auto text = SerializeFile();
  if (!text) return {};
  auto written = json::Parse(*text);
  auto held = json::Parse(file_text_);
  if (written && held && json::Equal(*written, *held)) {
    saved_revision_ = revision_;
  }
  return {};
}

std::string_view EditDocument::CoalesceLabel() const noexcept {
  return group_ != nullptr ? group_->Label() : std::string_view();
}

void EditDocument::SetPrefabText(std::string_view ref, std::string text) {
  prefab_texts_.insert_or_assign(std::string(ref), std::move(text));
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

std::expected<std::string, Error> EditDocument::SerializeFile() const {
  if (desc_.kind == DocumentKind::kPrefab) {
    if (!game_->save_prefab) {
      return Fail(ErrorCode::kUnsupported, "the game has no prefab writer");
    }
    return game_->save_prefab(*world_, desc_.ref, desc_.name);
  }
  if (!game_->save_scene_file) return Serialize();
  return game_->save_scene_file(*world_, desc_.name, desc_.asset_root);
}

std::uint64_t EditDocument::Hash() const { return world_->Hash().Folded(); }

std::uint64_t EditDocument::ContentHash() const {
  auto text = Serialize();
  if (!text) return 0;
  Hasher hasher(0xC0B7E87ULL);
  hasher.AddString(*text);
  return hasher.Finish();
}

}  // namespace se::editor
