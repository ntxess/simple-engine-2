// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/profile.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace se {
namespace {

void ProfiledWork(std::string_view name) {
  SE_PROFILE_ZONE("ProfiledWork");
  SE_PROFILE_ZONE_DYNAMIC(name);
  SE_PROFILE_PLOT("k_max", std::int64_t{2});
  SE_PROFILE_PLOT("dilation", 0.25);
  SE_PROFILE_MESSAGE(name);
  SE_PROFILE_FRAME_NAMED("tick");
  SE_PROFILE_FRAME();
}

TEST_CASE("The profiling macros are inert until a runtime starts Tracy",
          "[profile]") {
  REQUIRE(profile::Available() == (SE_TRACY != 0));
  SE_PROFILE_THREAD_NAME("test-main");
  ProfiledWork("work");
  std::thread other([] {
    SE_PROFILE_THREAD_NAME("test-other");
    ProfiledWork(std::string("other work"));
  });
  other.join();
  // Nothing started the profiler: no socket, no Tracy thread.
  REQUIRE_FALSE(profile::Started());
  REQUIRE_FALSE(profile::EverStarted());
}

}  // namespace
}  // namespace se
