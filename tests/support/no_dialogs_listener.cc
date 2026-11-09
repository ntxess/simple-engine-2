// Copyright 2026 The simple-engine-2 Authors.
//
// Tests run unattended, often on the owner's desktop: nothing may wait for
// a click. This listener, compiled into every Catch2 test executable that
// links SDL (se_platform), suppresses the fault dialogs before the first
// test case, whether or not SE_NO_MESSAGE_BOX is set: no CRT abort or
// assert box, no Windows Error Reporting dialog, and a failed SDL
// assertion (SDL's own or the presentation patch's) prints and aborts
// instead of opening SDL's Abort/Retry/Ignore dialog (ADR-070).

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "se_platform/fatal.h"

namespace se::testing {
namespace {

class NoDialogsListener final : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(const Catch::TestRunInfo& /*info*/) override {
    SuppressFaultDialogs();
  }
};

}  // namespace
}  // namespace se::testing

CATCH_REGISTER_LISTENER(se::testing::NoDialogsListener)
