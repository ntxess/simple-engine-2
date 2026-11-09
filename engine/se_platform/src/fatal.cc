// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/fatal.h"

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first.
#include <crtdbg.h>
#else
#include <unistd.h>
#endif

namespace se {
namespace {

// A NUL-terminated copy of a few pieces of text, truncated to the buffer:
// no allocation, so it is safe where nothing may throw.
template <std::size_t Capacity>
class FixedText {
 public:
  FixedText() noexcept { buffer_[0] = '\0'; }
  void Append(std::string_view text) noexcept {
    const std::size_t room = Capacity - 1 - size_;
    const std::size_t count = text.size() < room ? text.size() : room;
    std::copy_n(text.begin(), count, buffer_.begin() + size_);
    size_ += count;
    buffer_[size_] = '\0';
  }
  [[nodiscard]] const char* CStr() const noexcept { return buffer_.data(); }
  [[nodiscard]] std::string_view View() const noexcept {
    return std::string_view(buffer_.data(), size_);
  }

 private:
  std::array<char, Capacity> buffer_{};
  std::size_t size_ = 0;
};

}  // namespace

bool MessageBoxesSuppressed() noexcept {
  const char* value = SDL_getenv("SE_NO_MESSAGE_BOX");
  return value != nullptr && std::string_view(value) == "1";
}

void ConfigureFaultDialogs() noexcept {
  if (!MessageBoxesSuppressed()) return;
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

void ShowFatalMessage(std::string_view title, std::string_view text,
                      SDL_Window* parent) noexcept {
  std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(title.size()),
               title.data(), static_cast<int>(text.size()), text.data());
  std::fflush(stderr);
  if (MessageBoxesSuppressed()) return;
  FixedText<256> title_text;
  title_text.Append(title);
  FixedText<4096> body;
  body.Append(text);
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_text.CStr(), body.CStr(),
                           parent);
}

void ShowFaultMessage(std::string_view title, std::string_view where,
                      std::string_view what, SDL_Window* parent) noexcept {
  FixedText<4096> text;
  text.Append("A fault in ");
  text.Append(where);
  text.Append(": ");
  text.Append(what);
  text.Append("\nThe application will close.");
  ShowFatalMessage(title, text.View(), parent);
}

void TerminateProcessNow(int exit_code) noexcept {
  std::fflush(stdout);
  std::fflush(stderr);
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
#endif
  _exit(exit_code);
}

}  // namespace se
