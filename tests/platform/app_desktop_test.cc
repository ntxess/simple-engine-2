// Copyright 2026 The simple-engine-2 Authors.
//
// Desktop tests of the App (label desktop): a real window on this
// machine's desktop, opened without taking focus, driven only by messages
// the test posts or sends to its own window and by window-state calls that
// do not activate it. No SendInput, no SetCursorPos; each test checks that
// the cursor and the foreground window are unchanged.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_runtime/app.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/quit.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first.
#include <commctrl.h>
#endif

namespace se {
namespace {

using testing::RunTestApp;
using testing::TestGameSetup;

constexpr std::uint64_t kSafetyIterates = 1500;
constexpr double kNsToSec = 1e-9;

#if defined(_WIN32)

// The time of the last user input anywhere on the desktop.
DWORD LastInputTick() {
  LASTINPUTINFO info{};
  info.cbSize = sizeof(info);
  GetLastInputInfo(&info);
  return info.dwTime;
}

// The cursor and the foreground window must be as they were. A person may
// be using the machine while the desktop tests run: when real input arrived
// during the test, the person may have moved the cursor or switched windows,
// so the comparison is skipped (the test itself never generates input).
class DesktopUntouched {
 public:
  DesktopUntouched()
      : foreground_(GetForegroundWindow()), input_tick_(LastInputTick()) {
    GetCursorPos(&cursor_);
  }
  void Check(HWND ours) const {
    if (LastInputTick() != input_tick_) {
      WARN("user input during the test: cursor and focus checks skipped");
      return;
    }
    POINT now{};
    GetCursorPos(&now);
    CHECK(now.x == cursor_.x);
    CHECK(now.y == cursor_.y);
    CHECK(GetForegroundWindow() != ours);
    CHECK(GetForegroundWindow() == foreground_);
  }

 private:
  HWND foreground_;
  DWORD input_tick_;
  POINT cursor_{};
};

HWND HwndOf(App& app) {
  return static_cast<HWND>(app.GetWindow()->NativeHandle());
}

// The test's own message: open a popup menu, which runs a Windows modal
// loop (WM_ENTERMENULOOP) until something ends it.
constexpr UINT kOpenMenu = WM_APP + 1;
constexpr UINT_PTR kMenuSubclassId = 0x7E57;

LRESULT CALLBACK MenuLoopProc(HWND hwnd, UINT message, WPARAM wparam,
                              LPARAM lparam, UINT_PTR /*id*/,
                              DWORD_PTR /*ref*/) {
  if (message == kOpenMenu) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"se_platform_tests modal loop");
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    // TPM_RETURNCMD: no WM_COMMAND. Without TPM_NONOTIFY, so the owner
    // gets WM_ENTERMENULOOP and WM_EXITMENULOOP.
    TrackPopupMenuEx(menu, TPM_RETURNCMD, rect.left + 40, rect.top + 60, hwnd,
                     nullptr);
    DestroyMenu(menu);
    return 0;
  }
  return DefSubclassProc(hwnd, message, wparam, lparam);
}

// A modal loop entered programmatically on main and ended from a helper
// thread. A drag's move loop cannot be started without the mouse button
// down (WM_SYSCOMMAND SC_MOVE | HTCAPTION is ignored then) or without
// moving the cursor (the keyboard SC_MOVE centres it on the caption), so
// the test runs a menu loop: the same Modal flag (WM_ENTERMENULOOP) and the
// same WM_TIMER-driven iterates. A real drag is a gates.md entry.
TEST_CASE("Ticks continue during a modal loop with Modal set",
          "[desktop][app]") {
  const DesktopUntouched desktop;
  std::atomic<HWND> hwnd{nullptr};
  std::atomic<bool> modal_seen{false};
  std::atomic<bool> stop_ender{false};
  std::thread ender;
  std::uint64_t modal_start_ns = 0;
  std::uint64_t modal_last_ns = 0;
  std::uint64_t modal_ticks = 0;
  int modal_frames = 0;
  int cap_violations = 0;
  int max_modal_steps = 0;
  int frames_after_modal = 0;
  std::uint32_t modal_entries = 0;

  GameSetup setup = TestGameSetup();
  setup.on_boot = [&](App& app) {
    hwnd.store(HwndOf(app));
    SetWindowSubclass(hwnd.load(), &MenuLoopProc, kMenuSubclassId, 0);
  };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    if (report.iterate == 20) {
      // Posted: the loop runs from SDL's message pump, not inside this
      // iterate.
      PostMessageW(hwnd.load(), kOpenMenu, 0, 0);
      ender = std::thread([&] {
        for (int i = 0; i < 300 && !modal_seen.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        // End the loop from this thread: a posted Escape, then
        // WM_CANCELMODE should it still run.
        PostMessageW(hwnd.load(), WM_KEYDOWN, VK_ESCAPE, 0);
        for (int i = 0; i < 100 && !stop_ender.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!stop_ender.load()) PostMessageW(hwnd.load(), WM_CANCELMODE, 0, 0);
      });
    }
    if (report.modal) {
      if (!modal_seen.load()) {
        modal_seen.store(true);
        modal_start_ns = report.now_ns;
      }
      modal_last_ns = report.now_ns;
      ++modal_frames;
      modal_ticks += static_cast<std::uint64_t>(report.plan.steps);
      max_modal_steps = std::max(max_modal_steps, report.plan.steps);
      if (report.plan.steps > report.plan.k_max) ++cap_violations;
      CHECK(report.state == LifecycleState::kModal);
      CHECK(report.rendered);
    } else if (modal_seen.load() && ++frames_after_modal == 10) {
      stop_ender.store(true);
      modal_entries = app.GetWindow()->ModalEntries();
      app.RequestQuit();
    }
    if (report.iterate > kSafetyIterates) app.RequestQuit();
  };
  setup.on_shutdown = [&](App& /*app*/, SDL_AppResult /*result*/) {
    stop_ender.store(true);
    if (ender.joinable()) ender.join();
    RemoveWindowSubclass(hwnd.load(), &MenuLoopProc, kMenuSubclassId);
  };
  REQUIRE(RunTestApp(std::move(setup), {}) == 0);
  if (ender.joinable()) ender.join();

  REQUIRE(modal_seen.load());
  REQUIRE(modal_entries == 1);
  const double modal_sec =
      static_cast<double>(modal_last_ns - modal_start_ns) * kNsToSec;
  std::printf("modal for %.3f s: %d frames, %" PRIu64
              " ticks (%.1f per s), at most %d per frame\n",
              modal_sec, modal_frames, modal_ticks,
              static_cast<double>(modal_ticks) / modal_sec, max_modal_steps);
  REQUIRE(modal_sec > 0.8);
  // At least 100 ticks per second of modal time, with the cap applied.
  CHECK(static_cast<double>(modal_ticks) >= 100.0 * modal_sec);
  REQUIRE(cap_violations == 0);
  CHECK(max_modal_steps <= kMaxStepsPerFrame);
  desktop.Check(hwnd.load());
}

TEST_CASE("NULL acquires on a visible window never suspend", "[desktop][app]") {
  if (!kRuntimeTestHooks) SKIP("test hooks are not built into Release");
  const DesktopUntouched desktop;
  std::atomic<HWND> hwnd{nullptr};
  std::vector<FrameReport> frames;
  GameSetup setup = TestGameSetup();
  setup.on_boot = [&](App& app) { hwnd.store(HwndOf(app)); };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    frames.push_back(report);
    if (report.iterate == 80) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {"--inject-null-acquire", "20:30"}) ==
          0);
  REQUIRE(frames.size() == 80);
  int null_frames = 0;
  for (const FrameReport& frame : frames) {
    CHECK(frame.state != LifecycleState::kSuspended);
    CHECK_FALSE(frame.suspended);
    CHECK(frame.rate == CallbackRate::kFull);
    if (frame.iterate >= 20 && frame.iterate < 50) {
      CHECK(frame.rendered);
      CHECK_FALSE(frame.presented);
      ++null_frames;
    }
    // Gameplay continues through the NULL acquires.
    CHECK(frame.gameplay_ran);
  }
  REQUIRE(null_frames == 30);
  // Presentation resumes within 2 frames.
  CHECK((frames[49].presented || frames[50].presented));
  CHECK(frames[52].presented);
  desktop.Check(hwnd.load());
}

TEST_CASE("Minimize suspends at 10 Hz; restore resets the frame clock",
          "[desktop][app]") {
  const DesktopUntouched desktop;
  std::atomic<HWND> hwnd{nullptr};
  std::vector<FrameReport> frames;
  std::uint64_t minimized_at = 0;
  std::uint64_t restored_at = 0;
  GameSetup setup = TestGameSetup();
  setup.on_boot = [&](App& app) { hwnd.store(HwndOf(app)); };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    frames.push_back(report);
    if (report.iterate == 30) {
      // Minimized without activating another window.
      ShowWindow(hwnd.load(), SW_SHOWMINNOACTIVE);
      minimized_at = report.iterate;
    }
    if (minimized_at != 0 && restored_at == 0 && report.suspended &&
        report.iterate >= minimized_at + 12) {
      ShowWindow(hwnd.load(), SW_SHOWNOACTIVATE);
      restored_at = report.iterate;
    }
    if (restored_at != 0 && report.iterate == restored_at + 40) {
      app.RequestQuit();
    }
    if (report.iterate > kSafetyIterates) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {}) == 0);
  REQUIRE(minimized_at != 0);
  REQUIRE(restored_at != 0);

  std::vector<const FrameReport*> suspended;
  for (const FrameReport& frame : frames) {
    if (frame.suspended) suspended.push_back(&frame);
  }
  REQUIRE(suspended.size() >= 10);
  for (const FrameReport* frame : suspended) {
    CHECK(frame->state == LifecycleState::kSuspended);
    CHECK(frame->rate == CallbackRate::kSuspended);
    // No frames rendered, gameplay paused.
    CHECK_FALSE(frame->rendered);
    CHECK_FALSE(frame->gameplay_ran);
    REQUIRE(frame->plan.steps == 0);
    CHECK(frame->next_tick == suspended.front()->next_tick);
  }
  // About ten iterates per second while suspended (the first interval
  // still runs at full rate).
  const double seconds =
      static_cast<double>(suspended.back()->now_ns - suspended[1]->now_ns) *
      kNsToSec;
  const double rate = static_cast<double>(suspended.size() - 2) / seconds;
  INFO("suspended iterate rate " << rate << " Hz");
  CHECK(rate > 7.0);
  CHECK(rate < 13.0);

  // After the restore: no burst of catch-up ticks for the second spent
  // minimized.
  int resumed_steps = 0;
  int max_steps = 0;
  int resumed_frames = 0;
  for (const FrameReport& frame : frames) {
    if (frame.iterate <= restored_at || frame.suspended) continue;
    if (++resumed_frames > 5) break;
    resumed_steps += frame.plan.steps;
    max_steps = std::max(max_steps, frame.plan.steps);
    CHECK(frame.rendered);
  }
  REQUIRE(resumed_frames > 0);
  REQUIRE(max_steps <= 3);
  REQUIRE(resumed_steps <= 12);
  CHECK(frames.back().state != LifecycleState::kSuspended);
  desktop.Check(hwnd.load());
}

TEST_CASE("WM_QUERYENDSESSION runs the recovery hook synchronously",
          "[desktop][app]") {
  const DesktopUntouched desktop;
  std::atomic<HWND> hwnd{nullptr};
  std::vector<QuitReason> recovered;
  std::size_t recovered_during_send = 0;
  LRESULT may_end = FALSE;
  std::uint64_t sent_at = 0;
  std::uint64_t last_iterate = 0;
  GameSetup setup = TestGameSetup();
  setup.recovery = [&](QuitReason reason) { recovered.push_back(reason); };
  setup.on_boot = [&](App& app) { hwnd.store(HwndOf(app)); };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    last_iterate = report.iterate;
    if (report.iterate == 10) {
      may_end =
          SendMessageW(hwnd.load(), WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP);
      recovered_during_send = recovered.size();
      sent_at = report.iterate;
    }
    if (report.iterate > kSafetyIterates) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {}) == 0);
  REQUIRE(sent_at == 10);
  // The hook ran inside SendMessage, on main, before it returned.
  REQUIRE(recovered_during_send == 1);
  REQUIRE(recovered == std::vector<QuitReason>{QuitReason::kSessionEnd});
  CHECK(may_end != FALSE);
  // The app then exits at once, without a drain.
  REQUIRE(last_iterate == 10);
  desktop.Check(hwnd.load());
}

TEST_CASE("Power broadcasts suspend the app and resume it", "[desktop][app]") {
  const DesktopUntouched desktop;
  std::atomic<HWND> hwnd{nullptr};
  std::vector<FrameReport> frames;
  std::uint64_t resumed_at = 0;
  GameSetup setup = TestGameSetup();
  setup.on_boot = [&](App& app) { hwnd.store(HwndOf(app)); };
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    frames.push_back(report);
    if (report.iterate == 10) {
      SendMessageW(hwnd.load(), WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    }
    if (report.iterate == 15) {
      SendMessageW(hwnd.load(), WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
      resumed_at = report.iterate;
    }
    if (report.iterate == 25) app.RequestQuit();
    if (report.iterate > kSafetyIterates) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {}) == 0);
  REQUIRE(resumed_at == 15);
  REQUIRE(frames.size() == 25);
  CHECK_FALSE(frames[8].suspended);
  for (std::size_t i = 10; i < 14; ++i) {
    CHECK(frames[i].state == LifecycleState::kSuspended);
    CHECK(frames[i].rate == CallbackRate::kSuspended);
    CHECK_FALSE(frames[i].rendered);
  }
  for (std::size_t i = 15; i < 25; ++i) {
    CHECK_FALSE(frames[i].suspended);
    CHECK(frames[i].rendered);
  }
  desktop.Check(hwnd.load());
}

#endif  // _WIN32

}  // namespace
}  // namespace se
