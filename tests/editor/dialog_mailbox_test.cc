// Copyright 2026 The simple-engine-2 Authors.
//
// DialogMailbox (M10a): SDL's dialog callback from another thread copies
// the paths in and returns; main takes them at its next iterate. A
// callback after the quit started (the mailbox closed) or after the owner
// let go of the mailbox touches nothing but the mailbox (TSan in WSL).

#include "se_editor/dialog_mailbox.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/test_roles.h"

namespace se::editor {
namespace {

// Plays SDL's dialog thread: the callback with |paths| as SDL hands them
// over (a null-terminated list that lives until the callback returns).
void CallbackOnThread(void* userdata, const std::vector<std::string>& paths) {
  std::vector<const char*> list;
  list.reserve(paths.size() + 1);
  for (const std::string& path : paths) list.push_back(path.c_str());
  list.push_back(nullptr);
  const char* const* files = list.data();
  std::thread dialog(
      [userdata, files] { DialogMailbox::Callback(userdata, files, 0); });
  dialog.join();
}

TEST_CASE("A dialog's callback from another thread reaches main", "[dialogs]") {
  const testing::MainThreadScope main_thread;
  std::shared_ptr<DialogMailbox> mailbox = DialogMailbox::Create();
  const DialogMailbox::Opened opened = mailbox->Open(DialogKind::kOpen);
  REQUIRE(mailbox->Pending() == 1);
  CallbackOnThread(opened.userdata, {"a.scene.json", "b.scene.json"});
  REQUIRE(mailbox->Pending() == 0);
  const std::vector<DialogResult> results = mailbox->Take();
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].id == opened.id);
  REQUIRE(results[0].kind == DialogKind::kOpen);
  REQUIRE(results[0].paths ==
          std::vector<std::string>{"a.scene.json", "b.scene.json"});
  REQUIRE(mailbox->Take().empty());

  // Cancelled: an empty list; failed: no list.
  const DialogMailbox::Opened cancel = mailbox->Open(DialogKind::kSave);
  CallbackOnThread(cancel.userdata, {});
  const DialogMailbox::Opened failed = mailbox->Open(DialogKind::kSave);
  DialogMailbox::Callback(failed.userdata, nullptr, 0);
  const std::vector<DialogResult> more = mailbox->Take();
  REQUIRE(more.size() == 2);
  REQUIRE(more[0].cancelled);
  REQUIRE_FALSE(more[1].cancelled);
  REQUIRE(more[1].paths.empty());
}

TEST_CASE("A callback after the quit started only fills the mailbox",
          "[dialogs]") {
  const testing::MainThreadScope main_thread;
  std::shared_ptr<DialogMailbox> mailbox = DialogMailbox::Create();
  const DialogMailbox::Opened opened = mailbox->Open(DialogKind::kSave);
  mailbox->Close();
  CallbackOnThread(opened.userdata, {"late.scene.json"});
  REQUIRE(mailbox->Pending() == 0);
  REQUIRE(mailbox->Delivered() == 1);
  REQUIRE(mailbox->DeliveredAfterClose() == 1);
}

TEST_CASE("A callback after the owner let go keeps the mailbox alive",
          "[dialogs]") {
  const testing::MainThreadScope main_thread;
  std::shared_ptr<DialogMailbox> mailbox = DialogMailbox::Create();
  const std::weak_ptr<DialogMailbox> watch = mailbox;
  const DialogMailbox::Opened opened = mailbox->Open(DialogKind::kOpen);
  mailbox.reset();
  // The ticket holds it until the callback returns.
  REQUIRE_FALSE(watch.expired());
  CallbackOnThread(opened.userdata, {"orphan.scene.json"});
  REQUIRE(watch.expired());
}

TEST_CASE("Callbacks race the main thread's takes", "[dialogs]") {
  const testing::MainThreadScope main_thread;
  std::shared_ptr<DialogMailbox> mailbox = DialogMailbox::Create();
  constexpr int kDialogs = 64;
  std::vector<void*> tickets;
  tickets.reserve(kDialogs);
  for (int i = 0; i < kDialogs; ++i) {
    tickets.push_back(mailbox->Open(DialogKind::kOpen).userdata);
  }
  std::thread dialogs([&tickets] {
    const char* list[] = {"x.scene.json", nullptr};
    for (void* ticket : tickets) DialogMailbox::Callback(ticket, list, 0);
  });
  std::size_t taken = 0;
  while (taken < static_cast<std::size_t>(kDialogs)) {
    taken += mailbox->Take().size();
  }
  dialogs.join();
  REQUIRE(taken == static_cast<std::size_t>(kDialogs));
  REQUIRE(mailbox->Pending() == 0);
}

}  // namespace
}  // namespace se::editor
