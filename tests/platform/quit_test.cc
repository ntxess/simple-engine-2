// Copyright 2026 The simple-engine-2 Authors.
//
// The two-phase quit, without SDL (docs/milestone-specs.md M5, quit.h).

#include "se_runtime/quit.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <vector>

namespace se {
namespace {

// A test provider: pending for |pending_polls| polls, then ready or cancel.
class TestProvider final : public QuitProvider {
 public:
  TestProvider(int pending_polls, QuitPoll then)
      : pending_polls_(pending_polls), then_(then) {}

  QuitPoll PollQuit(QuitReason reason) override {
    reasons.push_back(reason);
    if (pending_polls_ > 0) {
      --pending_polls_;
      return QuitPoll::kPending;
    }
    return then_;
  }
  [[nodiscard]] std::string_view Name() const override { return "test"; }

  std::vector<QuitReason> reasons;

 private:
  int pending_polls_;
  QuitPoll then_;
};

TEST_CASE("Without providers a quit is ready at the next poll", "[quit]") {
  QuitController quit;
  int deadlines = 0;
  quit.SetDeadlineHook([&] { ++deadlines; });
  CHECK_FALSE(quit.Pending());
  quit.Request(QuitReason::kWindowClose);
  CHECK(quit.Pending());
  CHECK_FALSE(quit.ReadyToExit());
  REQUIRE(deadlines == 0);
  quit.Poll();
  CHECK(quit.ReadyToExit());
  CHECK_FALSE(quit.Pending());
  CHECK(quit.DeadlineStarted());
  CHECK(quit.Reason() == QuitReason::kWindowClose);
  REQUIRE(deadlines == 1);
  quit.Poll();
  quit.Request(QuitReason::kQuitEvent);
  REQUIRE(deadlines == 1);
  CHECK(quit.Reason() == QuitReason::kWindowClose);
}

TEST_CASE("A provider with pending work keeps the quit pending", "[quit]") {
  QuitController quit;
  TestProvider provider(5, QuitPoll::kReady);
  quit.AddProvider(&provider);
  quit.Request(QuitReason::kRequested);
  for (int i = 0; i < 5; ++i) {
    quit.Poll();
    CHECK(quit.Pending());
    CHECK_FALSE(quit.ReadyToExit());
    // The deadline does not start before the drain ends.
    CHECK_FALSE(quit.DeadlineStarted());
  }
  REQUIRE(quit.Blocker() == "test");
  quit.Poll();
  CHECK(quit.ReadyToExit());
  CHECK(quit.DeadlineStarted());
  REQUIRE(provider.reasons.size() == 6);
  CHECK(provider.reasons.front() == QuitReason::kRequested);
  REQUIRE(quit.PendingPolls() == 6);
}

TEST_CASE("A provider can cancel the quit", "[quit]") {
  QuitController quit;
  TestProvider cancelling(2, QuitPoll::kCancel);
  TestProvider ready(0, QuitPoll::kReady);
  quit.AddProvider(&ready);
  quit.AddProvider(&cancelling);
  quit.Request(QuitReason::kQuitEvent);
  quit.Poll();
  quit.Poll();
  CHECK(quit.Pending());
  quit.Poll();
  CHECK_FALSE(quit.Pending());
  CHECK_FALSE(quit.ReadyToExit());
  CHECK_FALSE(quit.DeadlineStarted());
  REQUIRE(quit.Cancellations() == 1);
  CHECK(quit.Reason() == QuitReason::kNone);
  // Polls do nothing until the next request.
  const std::size_t polled = cancelling.reasons.size();
  quit.Poll();
  REQUIRE(cancelling.reasons.size() == polled);
  // A later quit with the provider removed goes through.
  quit.RemoveProvider(&cancelling);
  quit.Request(QuitReason::kWindowClose);
  quit.Poll();
  CHECK(quit.ReadyToExit());
}

TEST_CASE("Session end runs the recovery hook synchronously, then exits",
          "[quit]") {
  QuitController quit;
  TestProvider slow(100, QuitPoll::kReady);
  quit.AddProvider(&slow);
  std::vector<QuitReason> recovered;
  int deadlines = 0;
  quit.SetRecoveryHook([&](QuitReason reason) { recovered.push_back(reason); });
  quit.SetDeadlineHook([&] { ++deadlines; });
  quit.Request(QuitReason::kWindowClose);
  quit.Poll();
  CHECK(quit.Pending());

  quit.RequestImmediate(QuitReason::kSessionEnd);
  // The hook ran before RequestImmediate returned.
  REQUIRE(recovered.size() == 1);
  CHECK(recovered[0] == QuitReason::kSessionEnd);
  CHECK(quit.ReadyToExit());
  CHECK(quit.Reason() == QuitReason::kSessionEnd);
  REQUIRE(deadlines == 1);
  // WM_ENDSESSION after WM_QUERYENDSESSION: the hook runs once per quit.
  quit.RequestImmediate(QuitReason::kSessionEnd);
  REQUIRE(recovered.size() == 1);
  REQUIRE(quit.RecoveryRuns() == 1);
}

TEST_CASE("TERMINATING runs the recovery hook without a pending quit",
          "[quit]") {
  QuitController quit;
  int recoveries = 0;
  quit.SetRecoveryHook([&](QuitReason /*reason*/) { ++recoveries; });
  quit.RequestImmediate(QuitReason::kTerminating);
  REQUIRE(recoveries == 1);
  CHECK(quit.ReadyToExit());
  REQUIRE(QuitReasonName(QuitReason::kTerminating) == "terminating");
}

}  // namespace
}  // namespace se
