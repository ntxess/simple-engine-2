// Copyright 2026 The simple-engine-2 Authors.
//
// Thread-role helpers for tests: a scope that registers the calling thread as
// the main thread (and so holds g_main for the analysis) and releases it.

#ifndef TESTS_SUPPORT_TEST_ROLES_H_
#define TESTS_SUPPORT_TEST_ROLES_H_

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::testing {

class SE_SCOPED_CAPABILITY MainThreadScope {
 public:
  MainThreadScope() noexcept SE_ACQUIRE(g_main) { RegisterMainThread(); }
  ~MainThreadScope() SE_RELEASE() { UnregisterMainThread(); }

  MainThreadScope(const MainThreadScope&) = delete;
  MainThreadScope& operator=(const MainThreadScope&) = delete;
};

}  // namespace se::testing

#endif  // TESTS_SUPPORT_TEST_ROLES_H_
