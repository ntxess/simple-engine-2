// Copyright 2026 The simple-engine-2 Authors.
//
// The engine's error type. Engine code never throws (ADR-006): fallible
// functions return std::expected<T, se::Error>.

#ifndef SE_CORE_ERROR_H_
#define SE_CORE_ERROR_H_

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace se {

enum class ErrorCode : std::uint16_t {
  kInvalidArgument,
  kParse,
  kNotFound,
  kVersion,
  kIo,
  kCorrupt,
  kUnsupported,
};

struct Error {
  ErrorCode code = ErrorCode::kInvalidArgument;
  std::string message;
  // Where the error was found, when it concerns a file.
  std::string path;
  std::int32_t line = 0;
  std::int32_t column = 0;
};

// Shorthand for the failure branch: return se::Fail(ErrorCode::kParse, "...").
inline std::unexpected<Error> Fail(ErrorCode code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message), {}, 0, 0});
}

}  // namespace se

#endif  // SE_CORE_ERROR_H_
