// Copyright 2026 The simple-engine-2 Authors.
//
// SE_LOG_FATAL with a running log worker (a death test): the fatal record
// reaches the worker's stderr and file sinks before the process aborts.
// CTest runs the two modes in order (tests/os/CMakeLists.txt):
//
//   se_log_fatal_test fatal <log file>   logs a fatal record and aborts
//   se_log_fatal_test check <log file>   exits 0 when the file holds it

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_log/log_worker.h"
#include "se_os/thread.h"
#include "support/test_roles.h"

namespace se {
namespace {

// CTest fails a test that a signal ended, whatever its output, so the abort
// becomes an ordinary exit status (as in tests/death/death_main.cc).
constexpr int kAbortExitStatus = 3;

void ExitOnAbort(int /*signal*/) { std::_Exit(kAbortExitStatus); }

int Fatal(const std::string& path) {
  std::signal(SIGABRT, &ExitOnAbort);
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
  std::remove(path.c_str());
  const testing::MainThreadScope main;
  const RoleScope role({.role = ThreadRole::kMain, .name = "main"});
  auto worker = LogWorker::Start({.file_path = path,
                                  .stderr_enabled = true,
                                  .stderr_level = LogLevel::kInfo});
  if (!worker) {
    std::fprintf(stderr, "cannot start the log worker: %s\n",
                 worker.error().message.c_str());
    return 2;
  }
  SE_LOG_INFO("before the fatal record");
  SE_LOG_FATAL("fatal marker {}", 42);
}

int Check(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  bool before = false;
  bool fatal = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(" I main ") != std::string::npos &&
        line.find("before the fatal record") != std::string::npos) {
      before = true;
    }
    if (line.find(" F main ") != std::string::npos &&
        line.find("fatal marker 42") != std::string::npos) {
      fatal = true;
    }
  }
  if (!before || !fatal) {
    std::fprintf(stderr, "%s lacks the records (before %d, fatal %d)\n",
                 path.c_str(), before ? 1 : 0, fatal ? 1 : 0);
    return 1;
  }
  std::fprintf(stderr, "both records are in %s\n", path.c_str());
  return 0;
}

}  // namespace
}  // namespace se

int main(int argc, char** argv) {
  // Test code may catch (ADR-006): an unexpected exception is a failure.
  try {
    if (argc == 3 && std::string_view(argv[1]) == "fatal") {
      return se::Fatal(argv[2]);
    }
    if (argc == 3 && std::string_view(argv[1]) == "check") {
      return se::Check(argv[2]);
    }
  } catch (...) {
    std::fprintf(stderr, "unexpected exception\n");
    return 4;
  }
  std::fprintf(stderr, "usage: se_log_fatal_test fatal|check <log file>\n");
  return 2;
}
