// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.raw_thread): engine code outside se_os
// starts a thread itself instead of through se::StartThread.

#include <thread>

namespace se {

void StartsItsOwnThread() {
  std::thread worker([] {});
  worker.join();
}

}  // namespace se
