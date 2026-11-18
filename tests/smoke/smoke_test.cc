// Copyright 2026 The simple-engine-2 Authors.

#include <catch2/catch_test_macros.hpp>

#include "se_core/assert.h"

namespace se {
namespace {

TEST_CASE("Assertions compile in every configuration", "[smoke]") {
  SE_ASSERT(1 + 1 == 2);
  SE_ASSERT_MSG(true, "never fires");
  SUCCEED();
}

}  // namespace
}  // namespace se
