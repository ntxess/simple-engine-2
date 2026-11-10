// Copyright 2026 The simple-engine-2 Authors.
//
// FaultBox: a reader that sees the flag (acquire) sees the whole record the
// poster wrote before it (release), and the first post wins.

#include "se_core/fault_box.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/thread_role.h"

namespace se {
namespace {

std::exception_ptr MakeError(const std::string& what) {
  try {
    throw std::runtime_error(what);
  } catch (...) {
    return std::current_exception();
  }
}

TEST_CASE("A posted fault is complete when the flag is seen",
          "[fault_box][threads]") {
  for (int run = 0; run < 200; ++run) {
    FaultBox box;
    bool posted = false;
    bool threw = false;
    std::thread poster([&] {
      try {
        FaultRecord record;
        record.role = ThreadRole::kSim;
        record.tick = 1000 + static_cast<Tick>(run);
        record.session = 7;
        record.SetMessage("boom");
        std::vector<std::byte> replay(64, std::byte{0x5A});
        posted = box.Post(MakeError("boom"), record, std::move(replay));
      } catch (...) {
        threw = true;
      }
    });
    while (!box.Posted()) std::this_thread::yield();
    // Read as soon as the flag is seen, checked after the join.
    const Tick tick = box.Record().tick;
    const ThreadRole role = box.Record().role;
    const std::string message(box.Record().Message());
    const std::size_t replay_size = box.Replay().size();
    const bool has_error = box.Exception() != nullptr;
    poster.join();
    REQUIRE_FALSE(threw);
    REQUIRE(posted);
    REQUIRE(tick == 1000 + static_cast<Tick>(run));
    REQUIRE(role == ThreadRole::kSim);
    REQUIRE(message == "boom");
    REQUIRE(replay_size == 64);
    REQUIRE(has_error);
  }
}

TEST_CASE("The first fault posted wins; later ones are counted",
          "[fault_box]") {
  FaultBox box;
  REQUIRE_FALSE(box.Posted());
  FaultRecord first;
  first.tick = 5;
  first.SetMessage(std::string(1000, 'x'));
  REQUIRE(box.Post(MakeError("first"), first));
  FaultRecord second;
  second.tick = 6;
  REQUIRE_FALSE(box.Post(MakeError("second"), second));
  REQUIRE(box.Posted());
  REQUIRE(box.Record().tick == 5);
  // Cut to fit, NUL-terminated.
  REQUIRE(box.Record().Message().size() == first.message.size() - 1);
  REQUIRE(box.Posts() == 2);
  try {
    std::rethrow_exception(box.Exception());
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()) == "first");
  }
}

}  // namespace
}  // namespace se
