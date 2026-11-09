// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: the terminate handler (ADR-033). It rethrows the
// active exception, if there is one, only to read its message, records the
// reason in the "se.terminate" annotation and ends the process through the
// previous handler or std::abort, which the crash handler turns into a dump.
// Nothing here may throw past the handler.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

#include "se_crash/crash.h"
#include "src/crash_internal.h"

namespace se::crash::internal {
namespace {

std::atomic<std::terminate_handler> g_previous{nullptr};
std::atomic<bool> g_installed{false};

// Appends |text| to |out| at |used|, truncating at the buffer's end.
void Append(std::array<char, kMaxReasonLength>& out, std::size_t& used,
            std::string_view text) noexcept {
  for (const char c : text) {
    if (used == out.size()) {
      return;
    }
    out[used++] = c;
  }
}

[[noreturn]] void OnTerminate() noexcept {
  // Static: the handler may run on a thread with little stack left.
  static std::array<char, kMaxReasonLength> reason{};
  std::size_t used = 0;
  Append(reason, used, "std::terminate ");
  if (const std::exception_ptr active = std::current_exception();
      active != nullptr) {
    try {
      std::rethrow_exception(active);
    } catch (const std::exception& e) {
      Append(reason, used, "with exception: ");
      Append(reason, used, e.what());
    } catch (...) {
      Append(reason, used, "with an exception of a non-std type");
    }
  } else {
    Append(reason, used, "without an active exception");
  }
  try {
    RecordTerminateReason(std::string_view(reason.data(), used));
  } catch (...) {
    // The dump follows without the annotation; say so where we can.
    std::fputs("se_crash: the terminate reason was not recorded\n", stderr);
  }
  if (const std::terminate_handler previous = g_previous.load();
      previous != nullptr) {
    previous();
  }
  std::abort();
}

}  // namespace

void InstallTerminateHandler() noexcept {
  if (g_installed.exchange(true)) {
    return;
  }
  g_previous.store(std::set_terminate(&OnTerminate));
}

void RestoreTerminateHandler() noexcept {
  if (!g_installed.exchange(false)) {
    return;
  }
  std::set_terminate(g_previous.exchange(nullptr));
}

}  // namespace se::crash::internal
