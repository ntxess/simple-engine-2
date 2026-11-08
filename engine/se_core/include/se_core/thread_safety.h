// Copyright 2026 The simple-engine-2 Authors.
//
// Clang thread-safety analysis attributes
// (https://clang.llvm.org/docs/ThreadSafetyAnalysis.html). They expand to
// nothing on other compilers. The clangcl-dev preset turns the analysis into
// errors (-Werror=thread-safety), so a call that needs a capability the caller
// does not hold fails to compile.

#ifndef SE_CORE_THREAD_SAFETY_H_
#define SE_CORE_THREAD_SAFETY_H_

#if defined(__clang__)
#define SE_THREAD_ANNOTATION_(x) __attribute__((x))
#else
#define SE_THREAD_ANNOTATION_(x)
#endif

// Declares a type whose objects are capabilities (roles, locks).
#define SE_CAPABILITY(name) SE_THREAD_ANNOTATION_(capability(name))

// Declares an RAII type that holds a capability for its lifetime.
#define SE_SCOPED_CAPABILITY SE_THREAD_ANNOTATION_(scoped_lockable)

// The caller must hold the capabilities.
#define SE_REQUIRES(...) SE_THREAD_ANNOTATION_(requires_capability(__VA_ARGS__))

// The function acquires the capabilities and holds them on return.
#define SE_ACQUIRE(...) SE_THREAD_ANNOTATION_(acquire_capability(__VA_ARGS__))

// The function releases the capabilities.
#define SE_RELEASE(...) SE_THREAD_ANNOTATION_(release_capability(__VA_ARGS__))

// The caller must not hold the capabilities.
#define SE_EXCLUDES(...) SE_THREAD_ANNOTATION_(locks_excluded(__VA_ARGS__))

// The function checks at run time that the capability is held; afterwards the
// analysis treats it as held.
#define SE_ASSERT_CAPABILITY(x) SE_THREAD_ANNOTATION_(assert_capability(x))

// Turns the analysis off for one function. Use only with a comment saying why.
#define SE_NO_THREAD_SAFETY_ANALYSIS \
  SE_THREAD_ANNOTATION_(no_thread_safety_analysis)

#endif  // SE_CORE_THREAD_SAFETY_H_
