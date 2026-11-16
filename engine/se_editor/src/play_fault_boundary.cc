// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: RunContained catches what a play tick or the play
// World's RenderList build throws (ADR-099, extending ADR-006's list), so
// that a faulting game rule leaves the editor alive in Play.Faulted
// instead of taking the callbacks' fault path. Nothing else here catches.

#include "src/play_fault_boundary.h"

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/log.h"

namespace se::editor {
namespace {

constexpr std::size_t kFaultTextSize = 256;

// Copies into a fixed buffer first (ADR-038): a fault from exhausted
// memory must not need memory to be caught.
void CopyText(char (&out)[kFaultTextSize], const char* text) noexcept {
  std::size_t n = 0;
  if (text != nullptr) {
    while (n + 1 < kFaultTextSize && text[n] != '\0') {
      out[n] = text[n];
      ++n;
    }
  }
  out[n] = '\0';
}

}  // namespace

ContainedFault RunContained(void (*run)(void* context), void* context) {
  char what[kFaultTextSize] = {};
  bool faulted = false;
  BreadcrumbFields crumb;
  try {
    run(context);
  } catch (const std::exception& error) {
    crumb = ThreadBreadcrumb().Fields();
    CopyText(what, error.what());
    faulted = true;
  } catch (...) {
    crumb = ThreadBreadcrumb().Fields();
    CopyText(what, "an exception that is not a std::exception");
    faulted = true;
  }
  ContainedFault fault;
  if (!faulted) return fault;
  fault.faulted = true;
  fault.what = what;
  fault.breadcrumb = crumb;
  return fault;
}

void ThrowInjected(const char* site) {
  const std::vector<int> empty;
  SE_LOG_WARN("editor: injecting a throw in {}", site);
  const int never = empty.at(1);
  SE_LOG_ERROR("the injected throw did not happen ({})", never);
}

}  // namespace se::editor
