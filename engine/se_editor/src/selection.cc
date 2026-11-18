// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/selection.h"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "se_core/guid.h"
#include "se_core/world.h"

namespace se::editor {

bool Selection::Contains(const Guid& guid) const noexcept {
  // find_if, not find: MSVC's vectorized find has no 16-byte element case.
  return std::find_if(guids_.begin(), guids_.end(), [&guid](const Guid& g) {
           return g == guid;
         }) != guids_.end();
}

Guid Selection::Primary() const noexcept {
  return guids_.empty() ? Guid{} : guids_.front();
}

void Selection::Set(std::span<const Guid> guids) {
  std::vector<Guid> next;
  next.reserve(guids.size());
  for (const Guid& guid : guids) {
    if (guid.IsNil()) continue;
    if (std::find_if(next.begin(), next.end(), [&guid](const Guid& g) {
          return g == guid;
        }) != next.end()) {
      continue;
    }
    next.push_back(guid);
  }
  if (next == guids_) return;
  guids_ = std::move(next);
  ++revision_;
}

void Selection::Clear() noexcept {
  if (guids_.empty()) return;
  guids_.clear();
  ++revision_;
}

void Selection::Prune(const World& world) {
  const auto gone = std::remove_if(
      guids_.begin(), guids_.end(),
      [&world](const Guid& guid) { return !world.Find(guid).has_value(); });
  if (gone == guids_.end()) return;
  guids_.erase(gone, guids_.end());
  ++revision_;
}

}  // namespace se::editor
