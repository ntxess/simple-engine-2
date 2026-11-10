// Copyright 2026 The simple-engine-2 Authors.
//
// LogView: the main-owned ring of formatted log lines the log panel draws
// (docs/milestone-specs.md M10, the panel interface and the Log panel;
// docs/design/mt-design.md "Editor", panels; ADR-095). The log worker
// formats every record once (se_log's panel sink, M4); at each frame start
// the editor moves the lines it has not seen yet into this ring, with each
// line's level parsed from its level letter, and the counts of records the
// log rings dropped and the rate limit cut. Nothing re-reads the log file
// (the legacy panel re-read it every frame, core/util/LogStream.cpp:19-42).
// Main thread only.

#ifndef SE_EDITOR_LOG_VIEW_H_
#define SE_EDITOR_LOG_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/log.h"

namespace se::editor {

struct LogLine {
  LogLevel level = LogLevel::kInfo;
  // As the worker wrote it: "12.345678 I main t=1440 file.cc:88 message".
  std::string text;
  // Lines appended before this one, since the view was created.
  std::uint64_t serial = 0;
};

class LogView {
 public:
  static constexpr std::size_t kDefaultCapacity = 2000;

  explicit LogView(std::size_t capacity = kDefaultCapacity);

  // Appends one formatted line; its level comes from the level letter
  // after the timestamp (T, D, I, W, E, F), info when there is none. The
  // oldest line goes when the ring is full.
  void Append(std::string text);
  void Append(LogLevel level, std::string text);
  // The log rings' drop and rate-limit counts (the log worker's stats).
  void SetCounts(std::uint64_t dropped, std::uint64_t suppressed) noexcept {
    dropped_ = dropped;
    suppressed_ = suppressed;
  }
  void Clear() noexcept;

  // Lines held, oldest first: At(0) is the oldest.
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] const LogLine& At(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t Capacity() const noexcept { return ring_.size(); }

  // Lines appended since the view was created (including those the ring
  // no longer holds).
  [[nodiscard]] std::uint64_t Total() const noexcept { return total_; }
  // Records the log rings dropped (full rings) and the rate limit cut.
  [[nodiscard]] std::uint64_t Dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t Suppressed() const noexcept {
    return suppressed_;
  }
  // Bumped by every change (a panel redraws or scrolls on change).
  [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

  // The level letter of a formatted line, or info.
  [[nodiscard]] static LogLevel LevelOf(std::string_view text) noexcept;

 private:
  std::vector<LogLine> ring_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  std::uint64_t total_ = 0;
  std::uint64_t dropped_ = 0;
  std::uint64_t suppressed_ = 0;
  std::uint64_t revision_ = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_LOG_VIEW_H_
