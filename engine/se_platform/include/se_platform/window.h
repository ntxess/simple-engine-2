// Copyright 2026 The simple-engine-2 Authors.
//
// The game window (docs/milestone-specs.md M5, window.h). Created and used
// on the main thread only.
//
// On Windows the window gets a SetWindowSubclass procedure, installed on
// main and removed before the window is destroyed, that sees messages SDL
// keeps private or never forwards:
//
//   WM_ENTERSIZEMOVE / WM_ENTERMENULOOP  set the Modal flag (SDL then drives
//   WM_EXITSIZEMOVE / WM_EXITMENULOOP    iterates from WM_TIMER); clear it
//   WM_QUERYENDSESSION, WM_ENDSESSION    WindowListener::OnSessionEnd,
//                                        synchronously, before returning
//   WM_POWERBROADCAST                    suspend and resume, to the listener
//
// The Modal flag is an atomic that other threads (the watchdog, from the
// integration step) may read. On Linux there is no subclass and Modal is
// never set.

#ifndef SE_PLATFORM_WINDOW_H_
#define SE_PLATFORM_WINDOW_H_

#include <SDL3/SDL_video.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

struct WindowConfig {
  std::string title = "simple-engine";
  int width = 1280;
  int height = 720;
  bool resizable = true;
  // Leave the window hidden after creation.
  bool hidden = false;
  // False for tests: the window is shown without taking focus.
  bool activate_when_shown = true;
};

enum class SessionEnd : std::uint8_t {
  // WM_QUERYENDSESSION: the session asks whether it may end.
  kQuery,
  // WM_ENDSESSION with wParam TRUE: the session is ending.
  kEnding,
};

enum class PowerChange : std::uint8_t { kSuspend, kResume };

// Receives what the subclass sees, synchronously on main, from inside the
// window procedure. The calls are noexcept: nothing may unwind through the
// window procedure or the Windows modal loop.
class WindowListener {
 public:
  virtual ~WindowListener() = default;
  virtual void OnSessionEnd(SessionEnd kind) noexcept = 0;
  virtual void OnPower(PowerChange change) noexcept = 0;
  virtual void OnModalChanged([[maybe_unused]] bool modal) noexcept {}
};

// The refresh rate of the display that shows the window.
struct DisplayRefresh {
  // 0 when SDL does not know it.
  double hz = 0.0;
  int numerator = 0;
  int denominator = 0;
};

class Window {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<Window>, Error> Create(
      const WindowConfig& config) SE_REQUIRES(g_main);

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;
  // Removes the subclass, then destroys the window. Main thread.
  ~Window();

  [[nodiscard]] SDL_Window* Native() const noexcept { return window_; }
  [[nodiscard]] SDL_WindowID Id() const noexcept { return id_; }
  // The HWND on Windows; nullptr elsewhere.
  [[nodiscard]] void* NativeHandle() const noexcept { return native_; }

  // The listener for subclass messages; nullptr to stop. Main thread.
  void SetListener(WindowListener* listener) noexcept SE_REQUIRES(g_main);

  // Whether a Windows modal loop (drag, resize, menu) is running. Any
  // thread.
  [[nodiscard]] bool Modal() const noexcept {
    return modal_.load(std::memory_order_acquire);
  }
  [[nodiscard]] const std::atomic<bool>& ModalFlag() const noexcept {
    return modal_;
  }
  // Modal loops entered since creation.
  [[nodiscard]] std::uint32_t ModalEntries() const noexcept {
    return modal_entries_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool Subclassed() const noexcept { return subclassed_; }

  [[nodiscard]] SDL_WindowFlags Flags() const SE_REQUIRES(g_main);
  // Not minimized, hidden or occluded.
  [[nodiscard]] bool Visible() const SE_REQUIRES(g_main);
  [[nodiscard]] bool Focused() const SE_REQUIRES(g_main);
  void SetTitle(std::string_view title) SE_REQUIRES(g_main);
  [[nodiscard]] DisplayRefresh Refresh() const SE_REQUIRES(g_main);

  // Called by the subclass procedure (main thread).
  void EnterModal() noexcept;
  void ExitModal() noexcept;
  void NotifySessionEnd(SessionEnd kind) noexcept;
  void NotifyPower(PowerChange change) noexcept;

 private:
  explicit Window(SDL_Window* window) noexcept;
  void InstallSubclass() noexcept;
  void RemoveSubclass() noexcept;

  SDL_Window* window_ = nullptr;
  SDL_WindowID id_ = 0;
  void* native_ = nullptr;
  bool subclassed_ = false;
  WindowListener* listener_ = nullptr;
  int modal_depth_ = 0;
  std::atomic<bool> modal_{false};
  std::atomic<std::uint32_t> modal_entries_{0};
};

}  // namespace se

#endif  // SE_PLATFORM_WINDOW_H_
