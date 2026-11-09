// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/window.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: commctrl.h needs its types.
#include <commctrl.h>
#endif

namespace se {
namespace {

#if defined(_WIN32)
// The subclass id: one engine subclass per window.
constexpr UINT_PTR kSubclassId = 0x53455731;  // "SEW1"

// Runs on main inside the window procedure. The Window outlives the
// subclass: ~Window removes it first.
LRESULT CALLBACK EngineSubclassProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR /*id*/,
                                    DWORD_PTR ref) noexcept {
  // The subclass API hands the reference data back as an integer; it is the
  // Window that installed the subclass.
  auto* window =
      reinterpret_cast<Window*>(ref);  // NOLINT(performance-no-int-to-ptr)
  switch (message) {
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
      // Before SDL's procedure, which starts the WM_TIMER that drives
      // iterates during the loop.
      window->EnterModal();
      break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP: {
      const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
      window->ExitModal();
      return result;
    }
    case WM_QUERYENDSESSION:
      window->NotifySessionEnd(SessionEnd::kQuery);
      break;
    case WM_ENDSESSION:
      if (wparam != FALSE) window->NotifySessionEnd(SessionEnd::kEnding);
      break;
    case WM_POWERBROADCAST:
      if (wparam == PBT_APMSUSPEND) {
        window->NotifyPower(PowerChange::kSuspend);
      } else if (wparam == PBT_APMRESUMEAUTOMATIC ||
                 wparam == PBT_APMRESUMESUSPEND) {
        window->NotifyPower(PowerChange::kResume);
      }
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, message, wparam, lparam);
}
#endif

// The HWND on Windows; nullptr elsewhere.
void* NativeHandleOf(SDL_Window* window) noexcept {
#if defined(_WIN32)
  return SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
  static_cast<void>(window);
  return nullptr;
#endif
}

Error WindowError(std::string_view what) {
  std::string message(what);
  message += ": ";
  message += SDL_GetError();
  return Error{ErrorCode::kUnsupported, std::move(message), {}, 0, 0};
}

}  // namespace

std::expected<std::unique_ptr<Window>, Error> Window::Create(
    const WindowConfig& config) {
  SE_ASSERT_MAIN_THREAD();
  SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
  if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
  SDL_Window* native = SDL_CreateWindow(config.title.c_str(), config.width,
                                        config.height, flags);
  if (native == nullptr) {
    return std::unexpected(WindowError("SDL_CreateWindow"));
  }
  // The constructor cannot fail; from here ~Window owns the SDL window.
  std::unique_ptr<Window> window(new Window(native));
  window->InstallSubclass();
  if (!config.hidden) {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN,
                config.activate_when_shown ? "1" : "0");
    if (!SDL_ShowWindow(native)) {
      return std::unexpected(WindowError("SDL_ShowWindow"));
    }
  }
  return window;
}

Window::Window(SDL_Window* window) noexcept
    : window_(window),
      id_(SDL_GetWindowID(window)),
      native_(NativeHandleOf(window)) {}

Window::~Window() {
  SE_ASSERT_MAIN_THREAD();
  RemoveSubclass();
  SDL_DestroyWindow(window_);
}

void Window::InstallSubclass() noexcept {
#if defined(_WIN32)
  if (native_ == nullptr) return;
  subclassed_ = SetWindowSubclass(static_cast<HWND>(native_),
                                  &EngineSubclassProc, kSubclassId,
                                  reinterpret_cast<DWORD_PTR>(this)) != FALSE;
#endif
}

void Window::RemoveSubclass() noexcept {
#if defined(_WIN32)
  if (!subclassed_) return;
  RemoveWindowSubclass(static_cast<HWND>(native_), &EngineSubclassProc,
                       kSubclassId);
  subclassed_ = false;
#endif
  if (modal_depth_ > 0) {
    modal_depth_ = 0;
    modal_.store(false, std::memory_order_release);
  }
}

void Window::SetListener(WindowListener* listener) noexcept {
  SE_ASSERT_MAIN_THREAD();
  listener_ = listener;
}

void Window::EnterModal() noexcept {
  ++modal_depth_;
  if (modal_depth_ > 1) return;
  modal_entries_.fetch_add(1, std::memory_order_relaxed);
  modal_.store(true, std::memory_order_release);
  if (listener_ != nullptr) listener_->OnModalChanged(true);
}

void Window::ExitModal() noexcept {
  if (modal_depth_ == 0) return;
  --modal_depth_;
  if (modal_depth_ > 0) return;
  modal_.store(false, std::memory_order_release);
  if (listener_ != nullptr) listener_->OnModalChanged(false);
}

void Window::NotifySessionEnd(SessionEnd kind) noexcept {
  if (listener_ != nullptr) listener_->OnSessionEnd(kind);
}

void Window::NotifyPower(PowerChange change) noexcept {
  if (listener_ != nullptr) listener_->OnPower(change);
}

SDL_WindowFlags Window::Flags() const {
  SE_ASSERT_MAIN_THREAD();
  return SDL_GetWindowFlags(window_);
}

bool Window::Visible() const {
  constexpr SDL_WindowFlags kNotVisible =
      SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED;
  return (Flags() & kNotVisible) == 0;
}

bool Window::Focused() const { return (Flags() & SDL_WINDOW_INPUT_FOCUS) != 0; }

void Window::SetTitle(std::string_view title) {
  SE_ASSERT_MAIN_THREAD();
  const std::string text(title);
  SDL_SetWindowTitle(window_, text.c_str());
}

DisplayRefresh Window::Refresh() const {
  SE_ASSERT_MAIN_THREAD();
  DisplayRefresh refresh;
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
  if (display == 0) return refresh;
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  if (mode == nullptr) return refresh;
  refresh.numerator = mode->refresh_rate_numerator;
  refresh.denominator = mode->refresh_rate_denominator;
  if (refresh.numerator > 0 && refresh.denominator > 0) {
    refresh.hz = static_cast<double>(refresh.numerator) /
                 static_cast<double>(refresh.denominator);
  } else {
    refresh.hz = static_cast<double>(mode->refresh_rate);
  }
  return refresh;
}

}  // namespace se
