// Copyright 2026 The simple-engine-2 Authors.
//
// Tests never start the profiler (docs/milestone-specs.md, M4a): with
// SE_TRACY on, Tracy is built with a manual lifetime, so nothing opens a
// socket or starts a thread until a runtime app calls profile::Start().
// This listener, compiled into every Catch2 test executable, fails the run
// if any test case started it.

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdio>
#include <cstdlib>

#include "se_core/profile.h"

namespace se::testing {
namespace {

class TracyInertListener final : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseEnded(const Catch::TestCaseStats& stats) override {
    if (profile::EverStarted()) {
      std::fprintf(stderr, "test case '%s' started the Tracy profiler\n",
                   stats.testInfo->name.c_str());
      std::_Exit(1);
    }
  }
};

}  // namespace
}  // namespace se::testing

CATCH_REGISTER_LISTENER(se::testing::TracyInertListener)
