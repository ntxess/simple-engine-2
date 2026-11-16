// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's Play fault boundaries (ADR-099; the ADR-006 list):
// play_fault_boundary.cc is the only file of se_editor that catches. A
// standard-library, EnTT or glaze throw out of a play tick, or out of the
// play World's RenderList build, ends that tick or that build and comes
// back as the exception's text; the PlaySession then enters Play.Faulted.
// The thrown-through state is left as it is (the World is frozen, never
// stepped again), which is what the fault screen shows.

#ifndef ENGINE_SE_EDITOR_SRC_PLAY_FAULT_BOUNDARY_H_
#define ENGINE_SE_EDITOR_SRC_PLAY_FAULT_BOUNDARY_H_

#include <cstddef>
#include <string>

#include "se_core/breadcrumb.h"

namespace se::editor {

struct ContainedFault {
  bool faulted = false;
  // what() of the exception, or a note that it was not a std::exception.
  std::string what;
  // The calling thread's breadcrumb at the catch: tick, phase and system
  // of the throw (the Schedule sets them as it goes).
  BreadcrumbFields breadcrumb;
};

// Runs run(context) and catches whatever it throws.
[[nodiscard]] ContainedFault RunContained(void (*run)(void* context),
                                          void* context);

template <class F>
[[nodiscard]] ContainedFault RunContained(F& body) {
  return RunContained([](void* context) { (*static_cast<F*>(context))(); },
                      static_cast<void*>(&body));
}

// A standard-library throw (the inject-throw test hooks; engine code never
// writes `throw`).
void ThrowInjected(const char* site);

}  // namespace se::editor

#endif  // ENGINE_SE_EDITOR_SRC_PLAY_FAULT_BOUNDARY_H_
