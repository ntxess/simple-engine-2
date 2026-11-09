// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/sdl_context.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

LogLevel LevelOf(SDL_LogPriority priority) noexcept {
  switch (priority) {
    case SDL_LOG_PRIORITY_TRACE:
    case SDL_LOG_PRIORITY_VERBOSE:
      return LogLevel::kTrace;
    case SDL_LOG_PRIORITY_DEBUG:
      return LogLevel::kDebug;
    case SDL_LOG_PRIORITY_INFO:
      return LogLevel::kInfo;
    case SDL_LOG_PRIORITY_WARN:
      return LogLevel::kWarn;
    case SDL_LOG_PRIORITY_ERROR:
    case SDL_LOG_PRIORITY_CRITICAL:
      return LogLevel::kError;
    case SDL_LOG_PRIORITY_INVALID:
    case SDL_LOG_PRIORITY_COUNT:
      break;
  }
  return LogLevel::kInfo;
}

// SDL calls this on whichever thread logs. LogWrite formats into a fixed
// buffer and never throws for this fixed format; were it to, noexcept ends
// the process instead of unwinding through SDL's C frames.
void SDLCALL SdlLogOutput(void* /*userdata*/, int /*category*/,
                          SDL_LogPriority priority,
                          const char* message) noexcept {
  const LogLevel level = LevelOf(priority);
  if (!LogEnabled(level) || message == nullptr) return;
  LogWrite(level, "SDL", 0, std::string_view(message));
}

Error SdlError(std::string_view what) {
  std::string message(what);
  message += ": ";
  message += SDL_GetError();
  return Error{ErrorCode::kUnsupported, std::move(message), {}, 0, 0};
}

}  // namespace

void RouteSdlLogToEngine() noexcept {
  SDL_SetLogOutputFunction(&SdlLogOutput, nullptr);
}

void RouteSdlLogToDefault() noexcept {
  SDL_SetLogOutputFunction(SDL_GetDefaultLogOutputFunction(), nullptr);
}

std::expected<void, Error> CheckMainThreadMatchesSdl() {
  if (!IsMainThread()) {
    return Fail(ErrorCode::kInvalidArgument,
                "SDL initialised off the registered main thread");
  }
  if (!SDL_IsMainThread()) {
    return Fail(ErrorCode::kInvalidArgument,
                "SDL_IsMainThread() is false on the engine's main thread "
                "(RegisterMainThread and SDL disagree, ADR-009)");
  }
  return {};
}

std::expected<SdlContext, Error> SdlContext::Init(const SdlInitConfig& config) {
  SE_ASSERT_MAIN_THREAD();
  // The M8 input sampler calls SDL_UpdateGamepads itself.
  SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
  if (!config.video_driver.empty()) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, config.video_driver.c_str());
  }
  if (!config.app_name.empty()) {
    SDL_SetAppMetadata(
        config.app_name.c_str(),
        config.app_version.empty() ? nullptr : config.app_version.c_str(),
        config.app_identifier.empty() ? nullptr
                                      : config.app_identifier.c_str());
  }
  RouteSdlLogToEngine();
  if (!SDL_InitSubSystem(config.subsystems)) {
    Error error = SdlError("SDL_InitSubSystem failed");
    RouteSdlLogToDefault();
    return std::unexpected(std::move(error));
  }
  SdlContext context(config.subsystems);
  if (auto same = CheckMainThreadMatchesSdl(); !same) {
    return std::unexpected(std::move(same.error()));
  }
  return context;
}

SdlContext::SdlContext(SdlContext&& other) noexcept
    : flags_(std::exchange(other.flags_, 0)) {}

SdlContext& SdlContext::operator=(SdlContext&& other) noexcept {
  if (this != &other) {
    Release();
    flags_ = std::exchange(other.flags_, 0);
  }
  return *this;
}

SdlContext::~SdlContext() { Release(); }

void SdlContext::Release() noexcept {
  if (flags_ == 0) return;
  SDL_QuitSubSystem(flags_);
  flags_ = 0;
  RouteSdlLogToDefault();
}

std::string SdlContext::VideoDriver() const {
  if ((flags_ & SDL_INIT_VIDEO) == 0) return {};
  const char* driver = SDL_GetCurrentVideoDriver();
  return driver != nullptr ? std::string(driver) : std::string();
}

}  // namespace se
