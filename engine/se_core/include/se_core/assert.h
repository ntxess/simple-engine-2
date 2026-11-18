// Copyright 2026 The simple-engine-2 Authors.
//
// Engine assertions. SE_ASSERT is active when SE_ASSERTS is 1 (Debug and Dev
// builds). A failed assertion reports the expression and location, records it
// for crash capture and aborts. It never calls DebugBreak: a debugger stops on
// the abort, and an unattended process produces a dump instead of hanging.

#ifndef SE_CORE_ASSERT_H_
#define SE_CORE_ASSERT_H_

#ifndef SE_ASSERTS
#define SE_ASSERTS 1
#endif

namespace se {

// Reports a failed assertion and aborts the process.
[[noreturn]] void AssertFailed(const char* expression, const char* message,
                               const char* file, int line) noexcept;

}  // namespace se

#if SE_ASSERTS
#define SE_ASSERT(expr)          \
  ((expr) ? static_cast<void>(0) \
          : ::se::AssertFailed(#expr, nullptr, __FILE__, __LINE__))
#define SE_ASSERT_MSG(expr, msg) \
  ((expr) ? static_cast<void>(0) \
          : ::se::AssertFailed(#expr, (msg), __FILE__, __LINE__))
#else
#define SE_ASSERT(expr) static_cast<void>(0)
#define SE_ASSERT_MSG(expr, msg) static_cast<void>(0)
#endif

// Marks code that cannot be reached. Checked in every build.
#define SE_UNREACHABLE() \
  ::se::AssertFailed("unreachable", nullptr, __FILE__, __LINE__)

#endif  // SE_CORE_ASSERT_H_
