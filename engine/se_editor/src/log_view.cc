// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/log_view.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/log.h"

namespace se::editor {

LogView::LogView(std::size_t capacity) : ring_(capacity > 0 ? capacity : 1) {}

LogLevel LogView::LevelOf(std::string_view text) noexcept {
  // "12.345678 I main ...": the letter after the first space.
  const std::size_t space = text.find(' ');
  if (space == std::string_view::npos || space + 2 >= text.size() ||
      text[space + 2] != ' ') {
    return LogLevel::kInfo;
  }
  switch (text[space + 1]) {
    case 'T':
      return LogLevel::kTrace;
    case 'D':
      return LogLevel::kDebug;
    case 'W':
      return LogLevel::kWarn;
    case 'E':
      return LogLevel::kError;
    case 'F':
      return LogLevel::kFatal;
    default:
      return LogLevel::kInfo;
  }
}

void LogView::Append(std::string text) {
  const LogLevel level = LevelOf(text);
  Append(level, std::move(text));
}

void LogView::Append(LogLevel level, std::string text) {
  const std::size_t slot = (head_ + count_) % ring_.size();
  ring_[slot] = LogLine{level, std::move(text), total_};
  if (count_ < ring_.size()) {
    ++count_;
  } else {
    head_ = (head_ + 1) % ring_.size();
  }
  ++total_;
  ++revision_;
}

void LogView::Clear() noexcept {
  head_ = 0;
  count_ = 0;
  ++revision_;
}

const LogLine& LogView::At(std::size_t index) const noexcept {
  SE_ASSERT_MSG(index < count_, "LogView::At past the end");
  return ring_[(head_ + index) % ring_.size()];
}

}  // namespace se::editor
