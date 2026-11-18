// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/assert.h"

#include <cstdio>
#include <cstdlib>

namespace se {

void AssertFailed(const char* expression, const char* message, const char* file,
                  int line) noexcept {
  std::fprintf(stderr, "SE_ASSERT failed: %s%s%s\n  at %s:%d\n", expression,
               message != nullptr ? " -- " : "",
               message != nullptr ? message : "", file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace se
