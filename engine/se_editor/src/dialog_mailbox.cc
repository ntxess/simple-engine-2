// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/dialog_mailbox.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/thread_role.h"

namespace se::editor {
namespace {

// One open dialog's SDL userdata: a reference to the mailbox.
struct Ticket {
  std::shared_ptr<DialogMailbox> mailbox;
  std::uint64_t id = 0;
  DialogKind kind = DialogKind::kOpen;
};

constexpr SDL_DialogFileFilter kSceneFilters[] = {
    {"Scene files", "json"},
};

}  // namespace

std::shared_ptr<DialogMailbox> DialogMailbox::Create() {
  return std::shared_ptr<DialogMailbox>(new DialogMailbox());
}

DialogMailbox::~DialogMailbox() = default;

DialogMailbox::Opened DialogMailbox::Open(DialogKind kind) {
  AssertMainThread();
  const std::uint64_t id = next_id_++;
  pending_.fetch_add(1, std::memory_order_acq_rel);
  auto* ticket = new Ticket{shared_from_this(), id, kind};
  return Opened{id, ticket};
}

void SDLCALL DialogMailbox::Callback(void* userdata,
                                     const char* const* filelist,
                                     int /*filter*/) {
  // Owning the ticket keeps the mailbox alive until this returns, whoever
  // else has let go of it.
  const std::unique_ptr<Ticket> ticket(static_cast<Ticket*>(userdata));
  if (ticket == nullptr || ticket->mailbox == nullptr) return;
  ticket->mailbox->Deliver(ticket->id, ticket->kind, filelist);
}

void DialogMailbox::Deliver(std::uint64_t id, DialogKind kind,
                            const char* const* filelist) {
  DialogResult result;
  result.id = id;
  result.kind = kind;
  if (filelist == nullptr) {
    const char* error = SDL_GetError();
    result.error = error != nullptr ? error : "the dialog failed";
  } else if (filelist[0] == nullptr) {
    result.cancelled = true;
  } else {
    for (const char* const* path = filelist; *path != nullptr; ++path) {
      result.paths.emplace_back(*path);
    }
  }
  {
    const std::scoped_lock lock(mutex_);
    results_.push_back(std::move(result));
  }
  delivered_.fetch_add(1, std::memory_order_acq_rel);
  if (closed_.load(std::memory_order_acquire)) {
    late_.fetch_add(1, std::memory_order_acq_rel);
  }
  // Published before the count drops, so a main thread that sees no
  // pending dialog also sees the result.
  pending_.fetch_sub(1, std::memory_order_acq_rel);
  // Main may be waiting for events (Idle): it acts at its next iterate.
  // WakeMain pushes only inside ADR-047's window.
  WakeMain();
}

std::vector<DialogResult> DialogMailbox::Take() {
  AssertMainThread();
  std::vector<DialogResult> taken;
  const std::scoped_lock lock(mutex_);
  taken.swap(results_);
  return taken;
}

std::uint64_t DialogMailbox::ShowOpen(SDL_Window* window,
                                      std::string_view location) {
  const Opened opened = Open(DialogKind::kOpen);
  const std::string where(location);
  SDL_ShowOpenFileDialog(&DialogMailbox::Callback, opened.userdata, window,
                         kSceneFilters, 1,
                         where.empty() ? nullptr : where.c_str(),
                         /*allow_many=*/false);
  return opened.id;
}

std::uint64_t DialogMailbox::ShowSave(SDL_Window* window,
                                      std::string_view location) {
  const Opened opened = Open(DialogKind::kSave);
  const std::string where(location);
  SDL_ShowSaveFileDialog(&DialogMailbox::Callback, opened.userdata, window,
                         kSceneFilters, 1,
                         where.empty() ? nullptr : where.c_str());
  return opened.id;
}

}  // namespace se::editor
