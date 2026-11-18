// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/content_source.h"

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/error.h"

namespace se {

CachedContentSource::CachedContentSource(Reader read)
    : read_(std::move(read)) {}

std::expected<std::string_view, Error> CachedContentSource::Text(
    std::string_view path) const {
  if (const auto found = texts_.find(path); found != texts_.end()) {
    return std::string_view(found->second);
  }
  if (!read_) {
    return std::unexpected(Error{ErrorCode::kNotFound,
                                 "the content source has no reader",
                                 std::string(path), 0, 0});
  }
  auto text = read_(path);
  if (!text) {
    Error error = std::move(text.error());
    if (error.path.empty()) error.path = std::string(path);
    return std::unexpected(std::move(error));
  }
  const auto [it, inserted] =
      texts_.emplace(std::string(path), std::move(*text));
  return std::string_view(it->second);
}

}  // namespace se
