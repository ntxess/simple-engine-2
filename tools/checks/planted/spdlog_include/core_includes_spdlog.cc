// Copyright 2026 The simple-engine-2 Authors.
//
// Planted violation (checks.planted.spdlog_include): a module other than
// se_log includes spdlog, whose sinks only the log worker may touch.

#include <spdlog/spdlog.h>

namespace se {

void LogsAroundTheWorker() { spdlog::info("bypassing the rings"); }

}  // namespace se
