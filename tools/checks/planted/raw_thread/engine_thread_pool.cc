// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.raw_thread): work submitted to the Win32
// thread pool.

#include <windows.h>

namespace se {

void QueuesWork(PTP_WORK_CALLBACK callback) {
  PTP_WORK work = CreateThreadpoolWork(callback, nullptr, nullptr);
  SubmitThreadpoolWork(work);
}

}  // namespace se
