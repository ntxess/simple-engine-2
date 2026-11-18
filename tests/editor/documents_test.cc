// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's several documents (M11b "Documents", "Commands and the
// carry-overs"; ADR-117, ADR-118): the DocumentSet's ids, activation and
// freeze; commands applied to the document they were made for; coalesce
// scopes that end when their panel stops drawing, their document is closed
// or deactivated, and at Play; a document's own file changed on disk.

#include "se_editor/documents.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "editor/support/document_world.h"
#include "editor/support/rampage_editor_game.h"
#include "game_rampage/module.h"
#include "se_core/thread_role.h"
#include "se_editor/commands.h"
#include "se_editor/edit_document.h"
#include "se_editor/edit_queue.h"
#include "se_editor/prefab_commands.h"
#include "se_editor/recovery.h"
#include "se_platform/fs.h"
#include "se_sim/game_module.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

using testing::kEnemy1;
using testing::kPlayer;

std::string TransformOf(const EditDocument& doc, Guid guid) {
  auto bundle = SerializeEntity(doc.Get(), guid);
  REQUIRE(bundle.has_value());
  for (const ComponentJson& component : *bundle) {
    if (component.component == "Transform") return component.json;
  }
  return {};
}

std::unique_ptr<EditorCommand> MoveTo(const EditDocument& doc, Guid guid,
                                      int x) {
  return std::make_unique<SetComponentCommand>(
      guid, "Transform", TransformOf(doc, guid),
      "{\"pos\":{\"x\":" + std::to_string(x) +
          ",\"y\":250},\"rot\":0,\"scale\":{\"x\":1,\"y\":1},"
          "\"teleport_tick\":0}");
}

struct TwoDocuments {
  TwoDocuments() SE_REQUIRES(g_main)
      : module(rampage::RampageModule()),
        game(testing::RampageEditorGame()),
        documents(kAutosaveQuietNs) {
    a = documents.Add(testing::OpenSandbox(module.Registry(), game));
    b = documents.Add(testing::OpenSandbox(module.Registry(), game));
  }
  EditDocument& A() { return *documents.Find(a)->document; }
  EditDocument& B() { return *documents.Find(b)->document; }

  ModuleInstance module;
  EditorGame game;
  DocumentSet documents;
  DocumentId a = kNoDocument;
  DocumentId b = kNoDocument;
};

bool Drew(std::string_view) { return true; }
bool DidNotDraw(std::string_view) { return false; }

TEST_CASE("The document set: ids are never reused, one is active",
          "[documents]") {
  const testing::MainThreadScope main_thread;
  TwoDocuments t;
  REQUIRE(t.documents.size() == 2);
  REQUIRE(t.a != t.b);
  // The last added is active; activation switches.
  REQUIRE(t.documents.ActiveId() == t.b);
  REQUIRE(t.documents.Activate(t.a));
  REQUIRE(t.documents.ActiveDocument() == &t.A());
  // Frozen (Play): no open, close or switch.
  t.documents.SetFrozen(true);
  REQUIRE_FALSE(t.documents.Activate(t.b));
  REQUIRE(t.documents.Close(t.b) == nullptr);
  REQUIRE(t.documents.Add(testing::OpenSandbox(t.module.Registry(), t.game)) ==
          kNoDocument);
  t.documents.SetFrozen(false);
  // A closed id is never given again; closing the active one activates a
  // neighbour.
  REQUIRE(t.documents.Close(t.a) != nullptr);
  REQUIRE(t.documents.ActiveId() == t.b);
  const DocumentId c =
      t.documents.Add(testing::OpenSandbox(t.module.Registry(), t.game));
  REQUIRE(c != t.a);
  REQUIRE(c > t.b);
  // A replaced document gets a new id at the same place.
  const DocumentId d = t.documents.Replace(
      t.b, testing::OpenSandbox(t.module.Registry(), t.game));
  REQUIRE(d > c);
  REQUIRE(t.documents.Find(t.b) == nullptr);
  REQUIRE(t.documents.At(0)->id == d);
  REQUIRE(t.documents.FindByName("1")->id == c);
  REQUIRE(t.documents.FindByPath(testing::SandboxScenePath()) != nullptr);
}

// Review fix F7: one file is one document, however its path is spelt (a
// "." segment, a ".." segment, and on Windows another case).
TEST_CASE("A file opens as one document whatever its path's spelling",
          "[documents]") {
  const testing::MainThreadScope main_thread;
  TwoDocuments t;
  const std::string path = testing::SandboxScenePath();
  const std::string dir = fs::Parent(path);
  const std::string file = path.substr(dir.size() + 1);
  REQUIRE(t.documents.FindByPath(dir + "/./" + file) != nullptr);
  const std::string leaf = dir.substr(fs::Parent(dir).size() + 1);
  REQUIRE(t.documents.FindByPath(dir + "/../" + leaf + "/" + file) != nullptr);
  REQUIRE(fs::Canonical(dir + "/./" + file) == fs::Canonical(path));
#if defined(_WIN32)
  std::string upper = path;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  REQUIRE(upper != path);
  REQUIRE(t.documents.FindByPath(upper) != nullptr);
  REQUIRE(fs::Canonical(upper) == fs::Canonical(path));
#endif
  REQUIRE(t.documents.FindByPath(dir + "/no_such.scene.json") == nullptr);
}

TEST_CASE("Commands apply to the document they were made for", "[documents]") {
  const testing::MainThreadScope main_thread;
  TwoDocuments t;
  const InlineOwnerScope owner(OnMainThread());
  EditQueue queue;
  const std::uint64_t a0 = t.A().Hash();
  const std::uint64_t b0 = t.B().Hash();
  // Made while A is active; B becomes active before the apply step.
  REQUIRE(t.documents.Activate(t.a));
  queue.Submit(t.a, "Inspector", MoveTo(t.A(), kPlayer, 301));
  REQUIRE(t.documents.Activate(t.b));
  const EditQueue::Applied first = queue.Apply(t.documents, Drew);
  REQUIRE(first.applied == 1);
  REQUIRE(t.A().Hash() != a0);
  REQUIRE(t.A().UndoCount() == 1);
  REQUIRE(t.B().Hash() == b0);
  REQUIRE(t.B().UndoCount() == 0);

  // A command for a document closed before the apply step is dropped.
  queue.Submit(t.a, "Inspector", MoveTo(t.A(), kEnemy1, 302));
  REQUIRE(t.documents.Close(t.a) != nullptr);
  const EditQueue::Applied second = queue.Apply(t.documents, Drew);
  REQUIRE(second.dropped == 1);
  REQUIRE(second.applied == 0);
  REQUIRE(t.B().Hash() == b0);
}

TEST_CASE("A coalesce scope ends when its panel stops drawing", "[documents]") {
  const testing::MainThreadScope main_thread;
  TwoDocuments t;
  const InlineOwnerScope owner(OnMainThread());
  EditQueue queue;
  REQUIRE(t.documents.Activate(t.a));

  SECTION("its panel did not draw in the frame (a hidden tab)") {
    queue.BeginCoalesce(t.a, "Inspector", "Edit Transform");
    queue.Submit(t.a, "Inspector", MoveTo(t.A(), kPlayer, 310));
    static_cast<void>(queue.Apply(t.documents, Drew));
    REQUIRE(queue.OpenScope().has_value());
    REQUIRE(t.A().Coalescing());
    queue.Submit(t.a, "Inspector", MoveTo(t.A(), kPlayer, 311));
    static_cast<void>(queue.Apply(t.documents, Drew));
    REQUIRE(t.A().Coalescing());
    // The next frame the Inspector's tab is hidden: its Draw did not run.
    static_cast<void>(queue.Apply(t.documents, DidNotDraw));
    REQUIRE_FALSE(queue.OpenScope().has_value());
    REQUIRE_FALSE(t.A().Coalescing());
    REQUIRE(queue.ScopesEnded() == 1);
    // One undo step for the drag.
    REQUIRE(t.A().UndoCount() == 1);
    // The two sets of one component were absorbed into one command.
    REQUIRE(t.A().UndoLabel() == "Set Transform");
  }

  SECTION("its document is deactivated or closed, and at Play") {
    queue.BeginCoalesce(t.a, "Viewport", "Move player");
    queue.Submit(t.a, "Viewport", MoveTo(t.A(), kPlayer, 320));
    static_cast<void>(queue.Apply(t.documents, Drew));
    REQUIRE(t.A().Coalescing());
    queue.EndScopeOf(t.documents, t.b, "another document");
    REQUIRE(t.A().Coalescing());
    queue.EndScopeOf(t.documents, t.a, "its document was deactivated");
    REQUIRE_FALSE(t.A().Coalescing());
    queue.BeginCoalesce(t.a, "Viewport", "Move player");
    static_cast<void>(queue.Apply(t.documents, Drew));
    REQUIRE(t.A().Coalescing());
    queue.EndScope(t.documents, "Play started");
    REQUIRE_FALSE(t.A().Coalescing());
    REQUIRE(queue.ScopesEnded() == 2);
  }

  SECTION("the script's scope (no panel) spans frames") {
    queue.BeginCoalesce(t.a, "", "Drag");
    queue.Submit(t.a, "", MoveTo(t.A(), kPlayer, 330));
    static_cast<void>(queue.Apply(t.documents, DidNotDraw));
    REQUIRE(t.A().Coalescing());
    queue.EndCoalesce(t.a, "");
    static_cast<void>(queue.Apply(t.documents, DidNotDraw));
    REQUIRE_FALSE(t.A().Coalescing());
    REQUIRE(queue.ScopesEnded() == 0);
  }
}

TEST_CASE("A changed document file reloads when clean and asks when dirty",
          "[documents]") {
  const testing::MainThreadScope main_thread;
  TwoDocuments t;
  const InlineOwnerScope owner(OnMainThread());
  EditDocument& doc = t.A();
  const std::string file = doc.FileText();
  REQUIRE(file == testing::SandboxText());
  // The same content (our own save, a touch) is no change.
  REQUIRE(ClassifyFileText(doc, file) == FileTextAction::kUnchanged);
  // Another program wrote the file: a clean document reloads.
  std::string changed = file;
  const std::size_t at = changed.find("\"enemy 1\"");
  REQUIRE(at != std::string::npos);
  changed.replace(at, 9, "\"enemy X\"");
  REQUIRE(ClassifyFileText(doc, changed) == FileTextAction::kReload);
  auto reopened = ReopenDocument(doc, t.game, changed);
  REQUIRE(reopened.has_value());
  REQUIRE((*reopened)->FileText() == changed);
  REQUIRE((*reopened)->UndoCount() == 0);
  REQUIRE_FALSE((*reopened)->Dirty());
  REQUIRE((*reopened)->ContentHash() != doc.ContentHash());
  // A dirty document asks.
  REQUIRE(doc.Apply(MoveTo(doc, kPlayer, 340)).has_value());
  REQUIRE(ClassifyFileText(doc, changed) == FileTextAction::kAsk);
}

TEST_CASE("Recovery files keep the document's kind; v1 files are scenes",
          "[documents][recovery]") {
  RecoveryFile file;
  file.document = "D:/content/assets/prefabs/enemy.prefab.json";
  file.name = "enemy";
  file.revision = 7;
  file.kind = DocumentKind::kPrefab;
  file.ref = "prefabs/enemy.prefab.json";
  file.scene = R"({"format": "se-prefab-v1", "name": "enemy"})";
  auto decoded = DecodeRecovery(EncodeRecovery(file), "x.serecovery");
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->kind == DocumentKind::kPrefab);
  REQUIRE(decoded->ref == file.ref);
  REQUIRE(decoded->scene == file.scene);
  // An M10 file (no kind): a scene.
  auto old = DecodeRecovery(
      R"({"format": "se-recovery-v1", "document": "a.scene.json", )"
      R"("name": "a", "revision": 3, "scene": "{}"})",
      "y.serecovery");
  REQUIRE(old.has_value());
  REQUIRE(old->kind == DocumentKind::kScene);
  REQUIRE(old->ref.empty());
}

}  // namespace
}  // namespace se::editor
