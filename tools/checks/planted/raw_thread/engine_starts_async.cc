// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.raw_thread): std::async with the async
// launch policy starts a thread outside se::StartThread.

#include <future>

namespace se {

int AsksAThreadForAnAnswer() {
  return std::async(std::launch::async, [] { return 42; }).get();
}

}  // namespace se
