// Copyright 2026 The simple-engine-2 Authors.
//
// Out-of-process crash capture (docs/milestone-specs.md, M4b; ADR-033).
//
// The crashpad handler, a separate process started by Start(), writes every
// dump: for a fatal fault (access violation, abort, std::terminate, stack
// overflow on Windows) and on request (DumpWithoutCrash). Nothing is written
// in process, nothing is ever uploaded: no URL is given and uploads are off
// in the database settings.
//
//   auto guard = se::crash::Start({.database_dir = dir,
//                                  .handler_path = HandlerPathIn(exe_dir)});
//   if (!guard) { /* log guard.error().message; run without capture */ }
//   (void)se::crash::RegisterStream(StreamType::kBreadcrumbs, table, size);
//
// Registered streams are memory regions the handler reads at dump time, so a
// dump shows their live contents. Each starts with the fixed header of
// se_core/dump_stream.h (docs/milestone-specs.md, "Dump stream layout"), so
// tools/minidump/read_dump.py decodes them without the build.
//
// Builds without crashpad (SE_CRASH=OFF, and Linux until the gates.md entry
// "Linux crash capture" is closed) get the same API: Start returns an Error,
// RegisterStream validates its arguments and keeps nothing, DumpWithoutCrash
// returns an Error.

#ifndef SE_CRASH_CRASH_H_
#define SE_CRASH_CRASH_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "se_core/error.h"

namespace se::crash {

// Minidump stream types of the first-party streams: 0x5345xxxx ("SE"),
// above the range Windows reserves (0-0xFFFF) and apart from crashpad's
// (0x4350xxxx) and sentry's (0x5379xxxx). The values are part of the dump
// format: tools/minidump/read_dump.py knows them.
enum class StreamType : std::uint32_t {
  kBreadcrumbs = 0x53450001,     // the ThreadRegistry table (kind 1)
  kInputFrameRing = 0x53450002,  // the World owner's InputFrame ring (kind 3)
  kSessionLogTail = 0x53450003,  // the SessionLog tail (kind 4, later)
  kFlightRecorder = 0x53450004,  // the pacing flight recorder (kind 5, M6)
};

inline constexpr std::uint32_t kFirstStreamType = 0x53450001;
inline constexpr std::uint32_t kLastStreamType = 0x5345FFFF;

// Log rings (kind 2) have one stream type per ThreadRegistry slot:
// kLogRingStreamBase + slot. The shared ring of threads the engine did not
// start uses slot kLibraryLogRingSlot.
inline constexpr std::uint32_t kLogRingStreamBase = 0x53450100;
inline constexpr std::uint32_t kMaxLogRingSlots = 256;
inline constexpr std::uint32_t kLibraryLogRingSlot = 64;

// The stream type of the log ring of ThreadRegistry slot |slot|, which must
// be below kMaxLogRingSlots.
[[nodiscard]] constexpr StreamType LogRingStream(std::uint32_t slot) noexcept {
  return static_cast<StreamType>(kLogRingStreamBase + slot);
}

// True for the values Start's streams may use: kFirstStreamType to
// kLastStreamType.
[[nodiscard]] constexpr bool IsFirstPartyStream(StreamType type) noexcept {
  const auto value = static_cast<std::uint32_t>(type);
  return value >= kFirstStreamType && value <= kLastStreamType;
}

struct CrashConfig {
  // The crashpad database, created if missing; its parent directory must
  // exist. Dumps are written to its reports/ directory. UTF-8.
  std::string database_dir;
  // The crashpad_handler executable, normally HandlerPathIn(ExecutableDir()):
  // the build copies it next to every executable that starts crash capture
  // (se_crash_copy_handler in engine/se_crash/CMakeLists.txt). UTF-8.
  std::string handler_path;
  // Process annotations written into every dump (crashpad's simple
  // annotations), for example the build and the command line.
  std::map<std::string, std::string> annotations;
};

// Crash capture while it lives. Fatal faults reach the handler from Start()
// to process exit: crashpad cannot uninstall its exception filter, and the
// handler process ends with this one. Destroying the guard ends
// DumpWithoutCrash (it then returns an Error) and restores the terminate
// handler that was installed before Start(). Keep it alive until the last
// shutdown step; at most one exists per process.
class CrashGuard {
 public:
  CrashGuard(CrashGuard&& other) noexcept;
  CrashGuard& operator=(CrashGuard&& other) noexcept;
  CrashGuard(const CrashGuard&) = delete;
  CrashGuard& operator=(const CrashGuard&) = delete;
  ~CrashGuard();

  class Impl;

 private:
  friend std::expected<CrashGuard, Error> Start(const CrashConfig& config);
  explicit CrashGuard(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

// Starts the out-of-process handler and installs the fault handlers and the
// terminate handler. The first boot step, before logging and SDL. Failure is
// an Error, never fatal: kInvalidArgument for an empty path, kNotFound for a
// missing handler executable, kIo when the database cannot be opened or the
// handler does not start, kUnsupported in builds without crashpad. The
// handler is launched at most once per process (crashpad's state cannot be
// set up twice): after a launch, failed or not, Start returns
// kInvalidArgument. After a failure the process's fault handlers are as they
// were.
[[nodiscard]] std::expected<CrashGuard, Error> Start(const CrashConfig& config);

// True between a successful Start() and the guard's destruction.
[[nodiscard]] bool Running() noexcept;

// Registers |size| bytes at |data| as a dump stream of |type|. The handler
// reads the region at dump time, so it must stay valid until the process
// exits (the ThreadRegistry's rings and breadcrumbs are never freed for this
// reason). Streams may be registered before Start(). Errors
// (kInvalidArgument): a type outside the first-party range, a null or empty
// region, a type already registered. Thread-safe: registration takes a
// mutex, which only thread start and boot code use.
[[nodiscard]] std::expected<void, Error> RegisterStream(StreamType type,
                                                        const void* data,
                                                        std::size_t size);

// Asks the handler for a dump of the running process without crashing it
// and returns once the handler has written it. |reason| goes into the dump
// as the "se.dump_reason" annotation (at most kMaxReasonLength bytes).
// Callable from any thread (the watchdog's hang dumps, the callbacks' fault
// path). Requests are serialised, one dump at a time: while a dump is being
// written the handler suspends every thread of the process, so a second
// requester waiting for the first adds no stall of its own. Returns an Error
// when crash capture is not running.
[[nodiscard]] std::expected<void, Error> DumpWithoutCrash(
    std::string_view reason);

// The longest reason or terminate message kept in an annotation.
inline constexpr std::size_t kMaxReasonLength = 255;

// The handler executable's file name on this platform.
#if defined(_WIN32)
inline constexpr std::string_view kHandlerFileName = "crashpad_handler.exe";
#else
inline constexpr std::string_view kHandlerFileName = "crashpad_handler";
#endif

// |dir| joined with kHandlerFileName.
[[nodiscard]] std::string HandlerPathIn(std::string_view dir);

}  // namespace se::crash

#endif  // SE_CRASH_CRASH_H_
