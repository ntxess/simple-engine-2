// Copyright 2026 The simple-engine-2 Authors.
//
// SDL's initialisation for the engine (docs/milestone-specs.md M5,
// se_platform). SdlContext::Init, on the main thread:
//
//   - sets SDL_HINT_AUTO_UPDATE_JOYSTICKS=0 (the M8 input sampler updates
//     gamepads itself), SDL_HINT_WINDOWS_RAW_KEYBOARD as asked and, when
//     asked, the video driver hint;
//   - routes SDL's log output to the SE_LOG front end (any thread);
//   - initialises the requested subsystems (VIDEO | GAMEPAD by default);
//   - cross-checks the engine's main thread (RegisterMainThread, ADR-009)
//     against SDL_IsMainThread().
//
// The destructor quits the subsystems it initialised (SDL_QuitSubSystem) and
// points SDL's log output back at SDL's default. SDL_Quit itself is left to
// SDL's main-callback driver (or to the test that initialised SDL).
//
// Every se_platform wrapper that calls an SDL video, event or window
// function asserts the main thread.

#ifndef SE_PLATFORM_SDL_CONTEXT_H_
#define SE_PLATFORM_SDL_CONTEXT_H_

#include <SDL3/SDL_init.h>

#include <expected>
#include <string>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

struct SdlInitConfig {
  SDL_InitFlags subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
  // SDL_HINT_VIDEO_DRIVER when not empty ("dummy", "offscreen", ...). The
  // SDL_VIDEO_DRIVER environment variable applies when this is empty.
  std::string video_driver;
  // SDL_HINT_WINDOWS_RAW_KEYBOARD: key events stamped on SDL's raw-input
  // thread (SDL_GetTicksNS) instead of from Win32 message times (M8).
  bool raw_keyboard = false;
  // SDL_SetAppMetadata; empty fields are left unset.
  std::string app_name;
  std::string app_version;
  std::string app_identifier;
};

class SdlContext {
 public:
  // Initialises SDL as described above. Fails (and undoes what it did) when
  // a subsystem cannot start or SDL disagrees about the main thread.
  [[nodiscard]] static std::expected<SdlContext, Error> Init(
      const SdlInitConfig& config) SE_REQUIRES(g_main);

  SdlContext(SdlContext&& other) noexcept;
  SdlContext& operator=(SdlContext&& other) noexcept;
  SdlContext(const SdlContext&) = delete;
  SdlContext& operator=(const SdlContext&) = delete;
  ~SdlContext();

  [[nodiscard]] SDL_InitFlags Subsystems() const noexcept { return flags_; }
  // The video driver SDL picked, or "" without SDL_INIT_VIDEO.
  [[nodiscard]] std::string VideoDriver() const;

 private:
  explicit SdlContext(SDL_InitFlags flags) noexcept : flags_(flags) {}
  void Release() noexcept;

  SDL_InitFlags flags_ = 0;
};

// Checks that SDL and the engine agree on the main thread (ADR-009): the
// caller must be the registered main thread and SDL_IsMainThread() must be
// true there. SdlContext::Init calls it after SDL_InitSubSystem.
[[nodiscard]] std::expected<void, Error> CheckMainThreadMatchesSdl()
    SE_REQUIRES(g_main);

// Routes SDL's log output (SDL_Log and SDL's own messages) to the SE_LOG
// front end. Safe from any thread; SdlContext::Init installs it.
void RouteSdlLogToEngine() noexcept;
// Points SDL's log output back at SDL's default (stderr).
void RouteSdlLogToDefault() noexcept;

}  // namespace se

#endif  // SE_PLATFORM_SDL_CONTEXT_H_
