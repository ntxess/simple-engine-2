// Copyright 2026 The simple-engine-2 Authors.
//
// Desktop tests (label desktop): they open a real window on this machine's
// desktop, without activating it, and send it messages on the main thread.
// No real input is generated.

#include "se_platform/window.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>

#include "se_platform/sdl_context.h"
#include "support/test_roles.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace se {
namespace {

class RecordingListener final : public WindowListener {
 public:
  void OnSessionEnd(SessionEnd kind) noexcept override {
    if (kind == SessionEnd::kQuery) {
      ++queries;
    } else {
      ++endings;
    }
  }
  void OnPower(PowerChange change) noexcept override {
    if (change == PowerChange::kSuspend) {
      ++suspends;
    } else {
      ++resumes;
    }
  }
  void OnModalChanged(bool modal) noexcept override {
    modal ? ++modal_on : ++modal_off;
  }

  int queries = 0;
  int endings = 0;
  int suspends = 0;
  int resumes = 0;
  int modal_on = 0;
  int modal_off = 0;
};

TEST_CASE("A window opens without taking focus", "[desktop][window]") {
  const testing::MainThreadScope main;
  {
    auto sdl = SdlContext::Init({});
    REQUIRE(sdl);
#if defined(_WIN32)
    const HWND foreground_before = GetForegroundWindow();
    LASTINPUTINFO input_before{};
    input_before.cbSize = sizeof(input_before);
    GetLastInputInfo(&input_before);
#endif
    WindowConfig config;
    config.title = "se_platform_tests window";
    config.width = 320;
    config.height = 200;
    config.activate_when_shown = false;
    auto window = Window::Create(config);
    REQUIRE(window);
    SDL_PumpEvents();
    CHECK((*window)->Native() != nullptr);
    REQUIRE((*window)->Id() != 0);
    CHECK((*window)->Visible());
    CHECK_FALSE((*window)->Modal());
    CHECK((*window)->Refresh().hz >= 0.0);
#if defined(_WIN32)
    CHECK((*window)->NativeHandle() != nullptr);
    CHECK((*window)->Subclassed());
    // A person using the machine may switch windows meanwhile; the check
    // holds when no real input arrived.
    LASTINPUTINFO input_after{};
    input_after.cbSize = sizeof(input_after);
    GetLastInputInfo(&input_after);
    if (input_after.dwTime == input_before.dwTime) {
      CHECK(GetForegroundWindow() == foreground_before);
      CHECK(GetForegroundWindow() != (*window)->NativeHandle());
    }
#endif
  }
  SDL_Quit();
}

#if defined(_WIN32)
TEST_CASE("The subclass sees modal loops, session end and power",
          "[desktop][window]") {
  const testing::MainThreadScope main;
  {
    auto sdl = SdlContext::Init({});
    REQUIRE(sdl);
    WindowConfig config;
    config.title = "se_platform_tests subclass";
    config.width = 320;
    config.height = 200;
    config.activate_when_shown = false;
    auto created = Window::Create(config);
    REQUIRE(created);
    std::unique_ptr<Window> window = std::move(*created);
    RecordingListener listener;
    window->SetListener(&listener);
    const auto hwnd = static_cast<HWND>(window->NativeHandle());

    // Nested modal loops (a menu loop inside a size-move loop) keep Modal
    // set until the outer one ends.
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    CHECK(window->Modal());
    SendMessageW(hwnd, WM_ENTERMENULOOP, FALSE, 0);
    SendMessageW(hwnd, WM_EXITMENULOOP, FALSE, 0);
    CHECK(window->Modal());
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    CHECK_FALSE(window->Modal());
    REQUIRE(window->ModalEntries() == 1);
    REQUIRE(listener.modal_on == 1);
    REQUIRE(listener.modal_off == 1);

    // Session end reaches the listener before SendMessage returns, and the
    // session may end.
    const LRESULT may_end =
        SendMessageW(hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP);
    REQUIRE(listener.queries == 1);
    CHECK(may_end != FALSE);
    SendMessageW(hwnd, WM_ENDSESSION, FALSE, 0);
    REQUIRE(listener.endings == 0);
    SendMessageW(hwnd, WM_ENDSESSION, TRUE, 0);
    REQUIRE(listener.endings == 1);

    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    REQUIRE(listener.suspends == 1);
    SendMessageW(hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    REQUIRE(listener.resumes == 1);

    window->SetListener(nullptr);
    window.reset();
  }
  SDL_Quit();
}
#endif

}  // namespace
}  // namespace se
