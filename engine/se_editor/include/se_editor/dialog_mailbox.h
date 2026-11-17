// Copyright 2026 The simple-engine-2 Authors.
//
// DialogMailbox: native file dialogs (docs/milestone-specs.md M10a "File
// dialogs"; docs/design/mt-design.md "Editor", file dialogs, and
// "Platform"; ADR-098). SDL_ShowOpenFileDialog and SDL_ShowSaveFileDialog
// run their callback on SDL's dialog thread (or on main, by platform). The
// callback only copies the chosen paths into the mailbox, calls WakeMain
// (which pushes only inside ADR-047's window) and returns; main takes the
// results at its next iterate and acts on them.
//
// The mailbox is shared (std::shared_ptr): each open dialog's SDL userdata
// is a ticket holding a reference, so a callback that arrives after the
// editor started quitting, or after the mailbox's owner dropped it,
// touches nothing but the mailbox, which lives until its last ticket
// ends. While a dialog is open (Pending() > 0) the editor's quit waits.

#ifndef SE_EDITOR_DIALOG_MAILBOX_H_
#define SE_EDITOR_DIALOG_MAILBOX_H_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::editor {

enum class DialogKind : std::uint8_t { kOpen, kSave };

struct DialogResult {
  // The id Open() returned.
  std::uint64_t id = 0;
  DialogKind kind = DialogKind::kOpen;
  // Chosen paths; empty when the user cancelled or the dialog failed.
  std::vector<std::string> paths;
  bool cancelled = false;
  // SDL's error when the dialog failed.
  std::string error;
};

class DialogMailbox : public std::enable_shared_from_this<DialogMailbox> {
 public:
  [[nodiscard]] static std::shared_ptr<DialogMailbox> Create();

  DialogMailbox(const DialogMailbox&) = delete;
  DialogMailbox& operator=(const DialogMailbox&) = delete;
  ~DialogMailbox();

  // Main: a dialog of |kind| opens. Returns its id and the userdata to give
  // SDL with Callback (a ticket holding a reference to the mailbox,
  // released by the callback).
  struct Opened {
    std::uint64_t id = 0;
    void* userdata = nullptr;
  };
  [[nodiscard]] Opened Open(DialogKind kind) SE_REQUIRES(g_main);
  // SDL_DialogFileCallback: any thread, at most once per ticket.
  static void SDLCALL Callback(void* userdata, const char* const* filelist,
                               int filter);

  // Main: the results that arrived, oldest first.
  [[nodiscard]] std::vector<DialogResult> Take() SE_REQUIRES(g_main);
  // Dialogs opened whose callback has not arrived.
  [[nodiscard]] std::uint32_t Pending() const noexcept {
    return pending_.load(std::memory_order_acquire);
  }
  // The owner stops acting on results (the editor quits); late callbacks
  // still land (and are counted), nothing acts on them.
  void Close() noexcept { closed_.store(true, std::memory_order_release); }
  [[nodiscard]] bool Closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
  }
  // Callbacks that arrived, and those that arrived after Close().
  [[nodiscard]] std::uint64_t Delivered() const noexcept {
    return delivered_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t DeliveredAfterClose() const noexcept {
    return late_.load(std::memory_order_acquire);
  }

  // Main: shows SDL's open or save dialog for |window| (may be null) with
  // one filter ("Scene files", *.json). Returns the dialog's id.
  std::uint64_t ShowOpen(SDL_Window* window, std::string_view location)
      SE_REQUIRES(g_main);
  std::uint64_t ShowSave(SDL_Window* window, std::string_view location)
      SE_REQUIRES(g_main);

 private:
  DialogMailbox() = default;
  void Deliver(std::uint64_t id, DialogKind kind, const char* const* filelist);

  std::mutex mutex_;
  std::vector<DialogResult> results_;  // under mutex_
  std::uint64_t next_id_ = 1;          // main only
  std::atomic<std::uint32_t> pending_{0};
  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> delivered_{0};
  std::atomic<std::uint64_t> late_{0};
};

}  // namespace se::editor

#endif  // SE_EDITOR_DIALOG_MAILBOX_H_
