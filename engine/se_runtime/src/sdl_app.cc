// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: the SDL main callbacks (ADR-006). Every callback is
// noexcept and catches everything, so a standard-library, EnTT or glaze
// throw never unwinds through SDL's C frames or the Windows modal loop. A
// caught fault requests a dump, tells the user and runs the quit path.

#include "se_runtime/sdl_app.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_platform/fatal.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/integration.h"

namespace se {
namespace {

std::atomic<bool> g_terminate_on_quit{kReleaseBuild};

using ExceptionText = std::array<char, 512>;

// Copies |text| into |buffer|, truncated; no allocation.
std::string_view CopyText(const char* text, ExceptionText& buffer) noexcept {
  const std::size_t length = std::min(std::strlen(text), buffer.size() - 1);
  std::memcpy(buffer.data(), text, length);
  buffer[length] = 0;
  return std::string_view(buffer.data(), length);
}

// The current exception's message, inside a catch handler, copied into
// |buffer| so that nothing allocates on the fault path. The copy happens
// inside the inner handler: the rethrown object, which what() points into,
// ends with it (ASan caught the earlier dangling read).
std::string_view CurrentExceptionText(ExceptionText& buffer) noexcept {
  try {
    std::rethrow_exception(std::current_exception());
  } catch (const std::exception& e) {
    return CopyText(e.what(), buffer);
  } catch (...) {
    return CopyText("an exception that is not a std::exception", buffer);
  }
}

// Logs a fault caught in a callback.
void LogFault(std::string_view where, std::string_view what) noexcept {
  try {
    SE_LOG_ERROR("fault in {}: {}", where, what);
  } catch (...) {
    std::fputs("simple-engine: a fault while logging a fault", stderr);
  }
}

// A boot failure or a fault outside the App: log it and tell the user.
void Report(std::string_view where, std::string_view text) noexcept {
  try {
    SE_LOG_ERROR("{}: {}", where, text);
    integration::RequestFaultDump(where);
    ShowFatalMessage("simple-engine",
                     std::string(where) + ": " + std::string(text), nullptr);
  } catch (...) {
    std::fputs("simple-engine: a fault while reporting a fault\n", stderr);
  }
}

void ReportBootFailure(const Error& error) noexcept {
  try {
    std::string text = error.message;
    if (!error.path.empty()) {
      text += " (";
      text += error.path;
      if (error.line > 0) text += ":" + std::to_string(error.line);
      text += ")";
    }
    SE_LOG_ERROR("boot failed: {}", text);
    ShowFatalMessage("simple-engine", "Start-up failed: " + text, nullptr);
  } catch (...) {
    std::fputs("simple-engine: start-up failed\n", stderr);
  }
}

}  // namespace

void SetTerminateOnQuit(bool terminate) noexcept {
  g_terminate_on_quit.store(terminate);
}

SDL_AppResult SdlAppInit(void** appstate, int argc, char** argv,
                         GameFactory factory) noexcept {
  if (appstate == nullptr) return SDL_APP_FAILURE;
  *appstate = nullptr;
  integration::EnterMainThread();
  try {
    AssertMainThread();
    ConfigureFaultDialogs();
    auto config = ParseAppConfig(argc, argv);
    if (!config) {
      ReportBootFailure(config.error());
      return SDL_APP_FAILURE;
    }
    g_terminate_on_quit.store(config->terminate_on_quit);
    auto app = App::Create(*config, factory);
    if (!app) {
      // App::Create has undone every step that ran.
      ReportBootFailure(app.error());
      return SDL_APP_FAILURE;
    }
    *appstate = app->release();
    return SDL_APP_CONTINUE;
  } catch (...) {
    ExceptionText buffer;
    Report("SDL_AppInit", CurrentExceptionText(buffer));
    return SDL_APP_FAILURE;
  }
}

SDL_AppResult SdlAppIterate(void* appstate) noexcept {
  auto* app = static_cast<App*>(appstate);
  if (app == nullptr) return SDL_APP_FAILURE;
  try {
    AssertMainThread();
    return app->Iterate();
  } catch (...) {
    ExceptionText buffer;
    const std::string_view what = CurrentExceptionText(buffer);
    LogFault("SDL_AppIterate", what);
    return app->OnCallbackFault("SDL_AppIterate", what);
  }
}

SDL_AppResult SdlAppEvent(void* appstate, SDL_Event* event) noexcept {
  auto* app = static_cast<App*>(appstate);
  if (app == nullptr || event == nullptr) return SDL_APP_CONTINUE;
  try {
    AssertMainThread();
    return app->HandleEvent(*event);
  } catch (...) {
    ExceptionText buffer;
    const std::string_view what = CurrentExceptionText(buffer);
    LogFault("SDL_AppEvent", what);
    return app->OnCallbackFault("SDL_AppEvent", what);
  }
}

void SdlAppQuit(void* appstate, SDL_AppResult result) noexcept {
  // Null after a failed boot.
  std::unique_ptr<App> app(static_cast<App*>(appstate));
  try {
    AssertMainThread();
    if (app) {
      app->Shutdown(result);
      app.reset();
    } else {
      SE_LOG_INFO(
          "SDL_AppQuit without an app (boot failed): {} engine threads "
          "running",
          integration::EngineThreadsRunning());
      integration::ShutdownLoggingOnly();
    }
  } catch (...) {
    ExceptionText buffer;
    Report("SDL_AppQuit", CurrentExceptionText(buffer));
  }
  integration::LeaveMainThread();
  if (g_terminate_on_quit.load()) {
    // Release builds skip CRT and DLL teardown, which have no deadline.
    TerminateProcessNow(result == SDL_APP_FAILURE ? 1 : 0);
  }
}

}  // namespace se
