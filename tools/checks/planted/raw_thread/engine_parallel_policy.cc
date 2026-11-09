// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.raw_thread): a parallel algorithm runs
// on the standard library's own threads.

#include <algorithm>
#include <execution>
#include <vector>

namespace se {

void SortsOnHiddenThreads(std::vector<int>& values) {
  std::sort(std::execution::par, values.begin(), values.end());
}

}  // namespace se
