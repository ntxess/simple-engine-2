// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's selection: Guids, never entt::entity (whose ids change with
// undo, Play and reloads). The editor owns one per document; panels read
// it through PanelContext::selection and change it only with
// EditorCommands::Select (docs/milestone-specs.md M10, the panel
// interface; ADR-095).

#ifndef SE_EDITOR_SELECTION_H_
#define SE_EDITOR_SELECTION_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/guid.h"

namespace se {
class World;
}  // namespace se

namespace se::editor {

class Selection {
 public:
  // The selected Guids, in the order they were selected; no duplicates and
  // no nil Guid.
  [[nodiscard]] std::span<const Guid> Guids() const noexcept { return guids_; }
  [[nodiscard]] bool Empty() const noexcept { return guids_.empty(); }
  [[nodiscard]] std::size_t Count() const noexcept { return guids_.size(); }
  [[nodiscard]] bool Contains(const Guid& guid) const noexcept;
  // The first selected Guid (what the inspector shows); nil when empty.
  [[nodiscard]] Guid Primary() const noexcept;

  // Replaces the selection; duplicates and nil Guids are dropped.
  void Set(std::span<const Guid> guids);
  void Clear() noexcept;
  // Drops the Guids |world| does not hold (after an undo, a Stop, a
  // destroy).
  void Prune(const World& world);

  // Bumped by every change.
  [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

 private:
  std::vector<Guid> guids_;
  std::uint64_t revision_ = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_SELECTION_H_
