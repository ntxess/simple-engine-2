// Copyright 2026 The simple-engine-2 Authors.
//
// The backend of builds without crashpad (SE_CRASH=OFF, and Linux until the
// docs/gates.md entry "Linux crash capture" is closed): Start and
// DumpWithoutCrash return kUnsupported, RegisterStream validates its
// arguments and keeps nothing, so callers need no build switches.

#include <expected>
#include <memory>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_crash/crash.h"
#include "src/crash_internal.h"

namespace se::crash {

class CrashGuard::Impl {};

CrashGuard::CrashGuard(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CrashGuard::CrashGuard(CrashGuard&& other) noexcept = default;
CrashGuard& CrashGuard::operator=(CrashGuard&& other) noexcept = default;
CrashGuard::~CrashGuard() = default;

std::expected<CrashGuard, Error> Start(const CrashConfig& /*config*/) {
  return Fail(ErrorCode::kUnsupported,
              "crash capture is not built into this binary (SE_CRASH=OFF or "
              "a platform without the crashpad backend; docs/gates.md)");
}

bool Running() noexcept { return false; }

std::expected<void, Error> RegisterStream(StreamType type, const void* data,
                                          std::size_t size) {
  return internal::ReserveStream(type, data, size);
}

std::expected<void, Error> DumpWithoutCrash(std::string_view /*reason*/) {
  return Fail(ErrorCode::kUnsupported,
              "crash capture is not built into this binary");
}

}  // namespace se::crash
