// Copyright 2026 The simple-engine-2 Authors.
//
// The legacy reference's .seinput files (core/util/LegacyInput.hpp): after
// '#' comment lines, "tick held pressed released" in hex, bit i = action i,
// with strictly increasing ticks. A tick without a line repeats the previous
// held state with no edges (LegacyInputReader::next), so files may be
// sparse.

#ifndef TESTS_RAMPAGE_SUPPORT_SEINPUT_H_
#define TESTS_RAMPAGE_SUPPORT_SEINPUT_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/types.h"

namespace se::testing {

// One InputFrame per tick from 1 to the file's last tick.
[[nodiscard]] std::expected<std::vector<InputFrame>, Error> ReadSeinput(
    const std::string& path);

// Plays a list of frames (frames[i] is tick i + 1); empty frames after it.
class FrameListInput final : public InputSource {
 public:
  explicit FrameListInput(std::vector<InputFrame> frames)
      : frames_(std::move(frames)) {}
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;
  [[nodiscard]] std::size_t Size() const noexcept { return frames_.size(); }

 private:
  std::vector<InputFrame> frames_;
};

}  // namespace se::testing

#endif  // TESTS_RAMPAGE_SUPPORT_SEINPUT_H_
