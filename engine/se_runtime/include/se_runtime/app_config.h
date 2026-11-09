// Copyright 2026 The simple-engine-2 Authors.
//
// AppConfig: the runtime's command line (docs/milestone-specs.md M5,
// app.h). Options take their value as the next argument or after '='
// ("--frames 300", "--frames=300"); arguments after "--" are the game's.
//
//   --frames N             exit (two-phase quit) after N presented frames;
//                          headless runs count iterates
//   --fixed-frame-dt MS    synthetic frame clock: every frame MS long
//   --gpu-driver NAME      SDL_GPU driver (default: SE_GPU_DRIVER, else
//                          direct3d12 on Windows, vulkan on Linux)
//   --sim-driver inline    the M17 switch; only "inline" is accepted
//   --profile              start the profiler (Tracy; it listens on
//                          localhost for a client, builds with SE_TRACY)
//   --headless             no window and no GPU device (SDL's dummy video
//                          driver; tests and tools)
//   --no-activate          show the window without taking focus (tests)
//   --content-root DIR     instead of searching from SDL's base path
//   --pref-dir DIR         instead of SDL_GetPrefPath(org, app)
//   --return-on-quit       return from SDL_AppQuit even in Release (the
//                          default there is to end the process after the
//                          final flush)
//   --log-level LEVEL      trace, debug, info, warn or error
//   --no-crash-capture     do not start the crash handler (tests that boot
//                          many apps in one process; the dump streams are
//                          still registered)
//   --pacing MODE          paced, standard, vrr or auto (the default): the
//                          FramePacer's mode (frame_pacer.h)
//
// Test hooks, in builds with SE_RUNTIME_TEST_HOOKS (every build but
// Release):
//   --fail-init-step K     boot step K (1-based, or its name) fails
//   --inject-throw SITE[:N]  a standard-library throw in the N-th call
//                          (default 1) of SITE: iterate or event
//   --inject-null-acquire FROM:COUNT  frames FROM .. FROM+COUNT-1 acquire
//                          NULL swapchain textures
//   --crash-test after-ticks:N  an access violation on main right after
//                          tick N (the M4 exit crash dump)
//   --dump-test after-ticks:N  after tick N the watchdog writes a dump; the
//                          ticks stop until it is written, then the app
//                          quits through the two-phase quit
//   --inject-late-frame FRAME:MS  iterate FRAME sleeps MS milliseconds after
//                          its ticks, before it renders (a late frame)
//   --inject-device-lost FRAME  at the start of iterate FRAME the device
//                          status reports removed once: DeviceLost, the
//                          device recreated on main (ADR-068)
//   --inject-frame-fault STEP[:EVERY[:UNTIL]]  the frame transaction's STEP
//                          (build, vertices, upload, prepare, copy_pass,
//                          viewport_pass, acquire, swapchain_pass, submit)
//                          fails on every EVERY-th frame up to frame UNTIL
//                          (se_render/frame_faults.h; M7)
//   --reload-texture NAME@FRAME[=FILE]  at frame FRAME, decode texture NAME
//                          again (from FILE instead of its table path when
//                          given) and queue its upload; repeatable
//   --readback FRAME:FILE  write the composite of the first frame rendered
//                          at or after frame FRAME to FILE as PNG;
//                          repeatable
// Frames are iterates, counted from 1, as for --inject-null-acquire.

#ifndef SE_RUNTIME_APP_CONFIG_H_
#define SE_RUNTIME_APP_CONFIG_H_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_render/frame_faults.h"
#include "se_runtime/pacer_policy.h"

#ifndef SE_RUNTIME_TEST_HOOKS
#define SE_RUNTIME_TEST_HOOKS 0
#endif

namespace se {

inline constexpr bool kRuntimeTestHooks = SE_RUNTIME_TEST_HOOKS != 0;
// Release builds are the ones without asserts.
inline constexpr bool kReleaseBuild = SE_ASSERTS == 0;

enum class ThrowSite : std::uint8_t { kNone, kIterate, kEvent };

// --reload-texture NAME@FRAME[=FILE].
struct TextureReloadHook {
  std::string name;
  std::int64_t frame = 0;
  // Empty: the texture table's path.
  std::string file;
};

// --readback FRAME:FILE.
struct ReadbackHook {
  std::int64_t frame = 0;
  std::string file;
};

struct AppConfig {
  std::int64_t frames = 0;
  double fixed_frame_dt_ms = 0.0;
  std::string gpu_driver;
  std::string sim_driver = "inline";
  bool profile = false;
  bool headless = false;
  bool activate_when_shown = true;
  std::string content_root;
  std::string pref_dir;
  bool terminate_on_quit = kReleaseBuild;
  LogLevel log_level = LogLevel::kInfo;
  bool crash_capture = true;
  PacingRequest pacing = PacingRequest::kAuto;

  // Test hooks.
  int fail_init_step = 0;
  ThrowSite throw_site = ThrowSite::kNone;
  std::int64_t throw_on_call = 1;
  std::int64_t null_acquire_from = 0;
  std::int64_t null_acquire_count = 0;
  std::int64_t crash_test_after_ticks = 0;
  std::int64_t dump_test_after_ticks = 0;
  std::int64_t late_frame_at = 0;
  double late_frame_ms = 0.0;
  std::int64_t device_lost_at = 0;
  FrameFaultConfig frame_fault;
  std::vector<TextureReloadHook> texture_reloads;
  std::vector<ReadbackHook> readbacks;

  // The arguments after "--".
  std::vector<std::string> game_args;
};

// Parses argv[1..argc-1]. Unknown options, missing or malformed values and
// test-hook options in a build without them are errors.
[[nodiscard]] std::expected<AppConfig, Error> ParseAppConfig(
    std::span<const char* const> args);
[[nodiscard]] std::expected<AppConfig, Error> ParseAppConfig(int argc,
                                                             char** argv);

// The names of the boot steps, in order (App::Create's Booting sequence);
// --fail-init-step accepts an index into this list (1-based) or a name.
[[nodiscard]] std::span<const std::string_view> InitStepNames() noexcept;

}  // namespace se

#endif  // SE_RUNTIME_APP_CONFIG_H_
