// Copyright 2026 The simple-engine-2 Authors.
//
// Dear ImGui's compile-time configuration. se_imgui sets
// IMGUI_USER_CONFIG to this header for itself and for every target that
// links it, so ImGui's sources and the engine see one configuration
// (ADR-095):
//   - GImGui is thread_local: the override imgui.cpp documents in its
//     "CONTEXT AND MEMORY ALLOCATORS" section. The context the editor
//     creates on main is null on every other thread, so an ImGui call off
//     main dereferences null (or fails IM_ASSERT's "No current context")
//     at once instead of racing with main (docs/design/mt-design.md
//     "Platform", ImGui's global context);
//   - IM_ASSERT is on in every build, Release included, and fails through
//     se::AssertFailed (a report, the crash annotation, abort);
//     IM_DEBUG_BREAK fails the same way (the engine never breaks into a
//     debugger, docs/style.md);
//   - ImGui calls neither Win32 nor the shell itself: the SDL3 backend
//     installs the clipboard, IME and open-URL handlers;
//   - obsolete functions are compiled out.

#ifndef SE_IMGUI_IMCONFIG_SE_H_
#define SE_IMGUI_IMCONFIG_SE_H_

#include "se_core/assert.h"

struct ImGuiContext;

// The current ImGui context of the calling thread (imgui_context.cc).
extern thread_local ImGuiContext* g_se_imgui_context;

// ImGui's name for its context pointer.
#define GImGui g_se_imgui_context  // NOLINT(readability-identifier-naming)

#define IM_ASSERT(expr)          \
  ((expr) ? static_cast<void>(0) \
          : ::se::AssertFailed(#expr, "Dear ImGui", __FILE__, __LINE__))
#define IM_DEBUG_BREAK() \
  ::se::AssertFailed("IM_DEBUG_BREAK", "Dear ImGui", __FILE__, __LINE__)

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

#endif  // SE_IMGUI_IMCONFIG_SE_H_
