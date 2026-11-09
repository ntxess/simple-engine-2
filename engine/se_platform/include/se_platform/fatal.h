// Copyright 2026 The simple-engine-2 Authors.
//
// Reporting a fatal error to the user, and ending the process.
//
// SE_NO_MESSAGE_BOX=1 (which the tests set) suppresses every message box:
// ShowFatalMessage only logs, and ConfigureFaultDialogs turns off the CRT's
// abort dialog and Windows Error Reporting's UI, so a failing test never
// waits for a click. Without it, faults show SDL's message box.

#ifndef SE_PLATFORM_FATAL_H_
#define SE_PLATFORM_FATAL_H_

#include <SDL3/SDL_video.h>

#include <string_view>

namespace se {

// True when SE_NO_MESSAGE_BOX is set to 1.
[[nodiscard]] bool MessageBoxesSuppressed() noexcept;

// With message boxes suppressed: no CRT abort message box, no Windows Error
// Reporting dialog, no critical-error dialogs. Call once at startup.
void ConfigureFaultDialogs() noexcept;

// Shows an error message box over |parent| (may be null) unless message
// boxes are suppressed. Any thread SDL allows; the runtime calls it on
// main.
void ShowFatalMessage(std::string_view title, std::string_view text,
                      SDL_Window* parent) noexcept;

// ShowFatalMessage with "A fault in <where>: <what>" and a line saying the
// application will close. Texts are truncated to a fixed buffer: nothing
// here allocates.
void ShowFaultMessage(std::string_view title, std::string_view where,
                      std::string_view what, SDL_Window* parent) noexcept;

// Ends the process with |exit_code| at once, skipping static destructors
// and DLL teardown, which have no deadline (release builds after the final
// flush; docs/design/mt-design.md "Shutdown Order" step 18).
[[noreturn]] void TerminateProcessNow(int exit_code) noexcept;

}  // namespace se

#endif  // SE_PLATFORM_FATAL_H_
