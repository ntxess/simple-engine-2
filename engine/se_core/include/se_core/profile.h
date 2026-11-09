// Copyright 2026 The simple-engine-2 Authors.
//
// Profiling macros (docs/design/mt-design.md "Tooling"):
//
//   SE_PROFILE_ZONE("name");               a zone with a literal name
//   SE_PROFILE_ZONE_DYNAMIC(view);         a zone named at run time (a
//                                          schedule system, a fork site)
//   SE_PROFILE_FRAME();                    the display frame
//   SE_PROFILE_FRAME_NAMED("tick");        a named frame set (literal)
//   SE_PROFILE_PLOT("k_max", value);       a plot (literal name)
//   SE_PROFILE_MESSAGE(view);              a message on the timeline
//   SE_PROFILE_THREAD_NAME("name");        names the calling thread
//
// They compile to nothing unless SE_TRACY is on (msvc-dev, msvc-release and
// clangcl-dev). With it on they use Tracy (TracyClient, linked privately by
// se_core; only profile.cc includes it), and every one of them is inert
// until a runtime app calls profile::Start() (--profile): Tracy is built
// with TRACY_DELAYED_INIT and TRACY_MANUAL_LIFETIME, so tests, tools and
// benches never open a socket or start a Tracy thread (ADR-029). Thread
// names given before the start are applied by the thread's first profiled
// event after it.

#ifndef SE_CORE_PROFILE_H_
#define SE_CORE_PROFILE_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef SE_TRACY
#define SE_TRACY 0
#endif

namespace se::profile {

// True while the profiler runs.
[[nodiscard]] bool Started() noexcept;
// True if the profiler ever ran in this process.
[[nodiscard]] bool EverStarted() noexcept;
// Starts Tracy's client (a runtime app's --profile). Returns false when
// the build has no Tracy or the profiler already ran.
bool Start() noexcept;
// Stops it; every thread that profiles must have finished (the App calls
// it after joining its threads).
void Stop() noexcept;
// True in SE_TRACY builds.
[[nodiscard]] constexpr bool Available() noexcept { return SE_TRACY != 0; }

// Layout-compatible with tracy::SourceLocationData (checked in
// profile.cc), so a zone with a literal name needs no allocation.
struct SourceLocation {
  const char* name;
  const char* function;
  const char* file;
  std::uint32_t line;
  std::uint32_t color;
};

// A profiler zone for the scope of the object. Inert when the profiler is
// not running.
class Zone {
 public:
  static constexpr std::size_t kStorageSize = 32;

  explicit Zone(const SourceLocation* location) noexcept;
  Zone(std::string_view name, const char* file, std::uint32_t line,
       const char* function) noexcept;
  ~Zone();

  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;

 private:
  // Storage for a tracy::ScopedZone (unused without SE_TRACY).
  [[maybe_unused]] alignas(8) std::byte storage_[kStorageSize];
  [[maybe_unused]] bool active_ = false;
};

void FrameMark() noexcept;
// |name| must be a string literal (Tracy keys frame sets by its address).
void FrameMarkNamed(const char* name) noexcept;
// |name| must be a string literal.
void Plot(const char* name, double value) noexcept;
void Plot(const char* name, std::int64_t value) noexcept;
void Message(std::string_view text) noexcept;
// Records the calling thread's name for Tracy (copied).
void SetThreadName(std::string_view name) noexcept;

}  // namespace se::profile

#if SE_TRACY
#define SE_PROFILE_CONCAT_INNER(a, b) a##b
#define SE_PROFILE_CONCAT(a, b) SE_PROFILE_CONCAT_INNER(a, b)
#define SE_PROFILE_ZONE(name)                                                 \
  static constexpr ::se::profile::SourceLocation SE_PROFILE_CONCAT(           \
      kSeProfileLocation, __LINE__){(name), __FUNCTION__, __FILE__,           \
                                    static_cast<std::uint32_t>(__LINE__), 0}; \
  const ::se::profile::Zone SE_PROFILE_CONCAT(se_profile_zone_, __LINE__)(    \
      &SE_PROFILE_CONCAT(kSeProfileLocation, __LINE__))
#define SE_PROFILE_ZONE_DYNAMIC(name)                                      \
  const ::se::profile::Zone SE_PROFILE_CONCAT(se_profile_zone_, __LINE__)( \
      (name), __FILE__, static_cast<std::uint32_t>(__LINE__), __FUNCTION__)
#define SE_PROFILE_FRAME() ::se::profile::FrameMark()
#define SE_PROFILE_FRAME_NAMED(name) ::se::profile::FrameMarkNamed(name)
#define SE_PROFILE_PLOT(name, value) ::se::profile::Plot((name), (value))
#define SE_PROFILE_MESSAGE(text) ::se::profile::Message(text)
#define SE_PROFILE_THREAD_NAME(name) ::se::profile::SetThreadName(name)
#else
#define SE_PROFILE_ZONE(name) static_cast<void>(0)
#define SE_PROFILE_ZONE_DYNAMIC(name) static_cast<void>(0)
#define SE_PROFILE_FRAME() static_cast<void>(0)
#define SE_PROFILE_FRAME_NAMED(name) static_cast<void>(0)
#define SE_PROFILE_PLOT(name, value) static_cast<void>(0)
#define SE_PROFILE_MESSAGE(text) static_cast<void>(0)
#define SE_PROFILE_THREAD_NAME(name) static_cast<void>(0)
#endif

#endif  // SE_CORE_PROFILE_H_
