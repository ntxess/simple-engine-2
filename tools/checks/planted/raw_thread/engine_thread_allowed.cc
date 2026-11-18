// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.raw_thread): the thread rule has no
// opt-out; only se_os may start threads.

#include <thread>

namespace se {

void StartsAThreadAnyway() {
  std::thread worker([] {});  // se-allow(thread): not accepted
  worker.join();
}

}  // namespace se
