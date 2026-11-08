// Copyright 2026 The simple-engine-2 Authors.

#include "rampage/support/seinput.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"

namespace se::testing {
namespace {

struct Line {
  Tick tick = 0;
  std::uint64_t held = 0;
  std::uint64_t pressed = 0;
  std::uint64_t released = 0;
};

InputFrame FrameOf(Tick tick, std::uint64_t held, std::uint64_t pressed,
                   std::uint64_t released) {
  InputFrame frame;
  frame.tick = tick;
  frame.held = held;
  for (int a = 0; a < kMaxActions; ++a) {
    frame.pressed[a] = static_cast<std::uint8_t>((pressed >> a) & 1U);
    frame.released[a] = static_cast<std::uint8_t>((released >> a) & 1U);
  }
  return frame;
}

}  // namespace

std::expected<std::vector<InputFrame>, Error> ReadSeinput(
    const std::string& path) {
  std::ifstream in(path);
  if (!in) return Fail(ErrorCode::kIo, "cannot open " + path);
  std::vector<Line> lines;
  std::string text;
  int number = 0;
  while (std::getline(in, text)) {
    ++number;
    const std::size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos || text[start] == '#' ||
        text[start] == '\r') {
      continue;
    }
    std::istringstream fields(text);
    Line line;
    fields >> std::hex >> line.tick >> line.held >> line.pressed >>
        line.released;
    if (!fields || (!lines.empty() && line.tick <= lines.back().tick)) {
      return std::unexpected(Error{ErrorCode::kParse,
                                   path + ":" + std::to_string(number) +
                                       ": malformed or out-of-order line",
                                   path, number, 0});
    }
    lines.push_back(line);
  }
  std::vector<InputFrame> frames;
  if (lines.empty()) return frames;
  std::size_t cursor = 0;
  std::uint64_t held = 0;
  for (Tick tick = 1; tick <= lines.back().tick; ++tick) {
    // Lines of skipped ticks (0) still update the held state.
    while (cursor < lines.size() && lines[cursor].tick < tick) {
      held = lines[cursor++].held;
    }
    if (cursor < lines.size() && lines[cursor].tick == tick) {
      const Line& line = lines[cursor++];
      held = line.held;
      frames.push_back(FrameOf(tick, line.held, line.pressed, line.released));
    } else {
      frames.push_back(FrameOf(tick, held, 0, 0));
    }
  }
  return frames;
}

InputFrame FrameListInput::Latch(Tick tick, std::uint64_t /*window_end*/,
                                 bool /*last_tick_of_frame*/) {
  if (tick >= 1 && tick - 1 < frames_.size()) {
    return frames_[static_cast<std::size_t>(tick - 1)];
  }
  InputFrame empty;
  empty.tick = tick;
  return empty;
}

}  // namespace se::testing
