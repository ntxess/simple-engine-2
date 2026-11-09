// Copyright 2026 The simple-engine-2 Authors.
//
// Internals shared by se_crash's backends (crashpad and the stub).

#ifndef ENGINE_SE_CRASH_SRC_CRASH_INTERNAL_H_
#define ENGINE_SE_CRASH_SRC_CRASH_INTERNAL_H_

#include <cstddef>
#include <expected>
#include <string_view>

#include "se_core/error.h"
#include "se_crash/crash.h"

namespace se::crash::internal {

// Checks a RegisterStream request and reserves its type: kInvalidArgument
// for a type outside the first-party range, a null or empty region, or a
// type already reserved. Thread-safe.
[[nodiscard]] std::expected<void, Error> ReserveStream(StreamType type,
                                                       const void* data,
                                                       std::size_t size);

// Releases a type that ReserveStream reserved, when the backend could not
// register the stream after all.
void ReleaseStream(StreamType type) noexcept;

// Installs the terminate handler (terminate_handler.cc): it records why
// std::terminate was called with RecordTerminateReason, then calls the
// handler that was installed before, or std::abort, which the crash handler
// turns into a dump.
void InstallTerminateHandler() noexcept;

// Puts back the terminate handler that InstallTerminateHandler replaced.
void RestoreTerminateHandler() noexcept;

// Records |reason| in the "se.terminate" annotation. Defined by the backend;
// called on the terminating thread, so it neither allocates nor locks.
// crashpad's checks in it may log, so it is not noexcept.
void RecordTerminateReason(std::string_view reason);

}  // namespace se::crash::internal

#endif  // ENGINE_SE_CRASH_SRC_CRASH_INTERNAL_H_
