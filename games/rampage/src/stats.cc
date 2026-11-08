// Copyright 2026 The simple-engine-2 Authors.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "game_rampage/components.h"

namespace se::rampage {
namespace {

template <class S>
auto LowerBound(S& stats, StatId id) {
  return std::lower_bound(
      stats.values.begin(), stats.values.end(), id,
      [](const StatValue& value, StatId key) { return value.id < key; });
}

}  // namespace

const float* FindStat(const Stats& stats, StatId id) noexcept {
  const auto it = LowerBound(stats, id);
  return it != stats.values.end() && it->id == id ? &it->value : nullptr;
}

float* FindStat(Stats& stats, StatId id) noexcept {
  const auto it = LowerBound(stats, id);
  return it != stats.values.end() && it->id == id ? &it->value : nullptr;
}

void SetStat(Stats& stats, StatId id, float value) {
  const auto it = LowerBound(stats, id);
  if (it != stats.values.end() && it->id == id) {
    it->value = value;
  } else {
    stats.values.insert(it, StatValue{id, value});
  }
}

StatId StatNames::Add(std::string_view name) {
  const StatId id = StatIdOf(name);
  const auto it = std::lower_bound(
      names_.begin(), names_.end(), id,
      [](const auto& entry, StatId key) { return entry.first < key; });
  if (it == names_.end() || it->first != id) {
    names_.emplace(it, id, std::string(name));
  }
  return id;
}

std::string_view StatNames::Name(StatId id) const noexcept {
  const auto it = std::lower_bound(
      names_.begin(), names_.end(), id,
      [](const auto& entry, StatId key) { return entry.first < key; });
  return it != names_.end() && it->first == id ? std::string_view(it->second)
                                               : std::string_view();
}

}  // namespace se::rampage
