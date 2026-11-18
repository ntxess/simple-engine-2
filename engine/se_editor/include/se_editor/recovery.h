// Copyright 2026 The simple-engine-2 Authors.
//
// Recovery and autosave (docs/milestone-specs.md M10a "Recovery and
// autosave"; docs/design/mt-design.md "Editor", faults, hangs and
// quitting; ADR-098). A dirty document is written to its recovery file
// after 2 s without input, as a Background job of the background lane (a
// unique temp file in the same directory, then a rename that never goes
// over a newer revision); WM_QUERYENDSESSION and SDL_EVENT_TERMINATING
// write every dirty document's recovery file synchronously on main
// (se::fs::WriteFileAtomic). A saved document's recovery file is removed.
// At the next start the editor offers the recovery files it finds for the
// document it opens.
//
// A recovery file is <pref>recovery/<name>-<16 hex digits>.serecovery, the
// digits a hash of the document's path, holding
//   {"format": "se-recovery-v1", "document": "<path>", "name": "<name>",
//    "revision": N, "kind": "scene"|"prefab", "ref": "<prefab reference>",
//    "scene": "<the document file's text>"}
// The text is the document's file as Save writes it (M11b: a scene keeps
// its prefab references, a prefab is a prefab file). "kind" and "ref" were
// added by M11b; a file without them is a scene (no migration needed).
// The Play-start bytes go to <pref>recovery/play-start.scene.json when Play
// starts (the session's start, kept for bundles and hang dumps).

#ifndef SE_EDITOR_RECOVERY_H_
#define SE_EDITOR_RECOVERY_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_editor/edit_document.h"

namespace se::editor {

inline constexpr std::uint64_t kAutosaveQuietNs = 2'000'000'000;
inline constexpr std::string_view kRecoveryFormat = "se-recovery-v1";
inline constexpr std::string_view kRecoverySuffix = ".serecovery";

struct RecoveryFile {
  // Where the recovery file is.
  std::string path;
  // The document it recovers, and its name.
  std::string document;
  std::string name;
  std::uint64_t revision = 0;
  // The document's kind, and a prefab document's reference.
  DocumentKind kind = DocumentKind::kScene;
  std::string ref;
  // The document's file text at that revision.
  std::string scene;
};

// <pref>recovery/ (with a trailing '/').
[[nodiscard]] std::string RecoveryDirectory(std::string_view pref);
// The recovery file of |document| (a path; empty: an unsaved document,
// keyed by |name|).
[[nodiscard]] std::string RecoveryPathFor(std::string_view pref,
                                          std::string_view document,
                                          std::string_view name);

[[nodiscard]] std::string EncodeRecovery(const RecoveryFile& file);
[[nodiscard]] std::expected<RecoveryFile, Error> DecodeRecovery(
    std::string_view text, std::string path);

// Every recovery file under <pref>recovery/ that decodes, in name order.
[[nodiscard]] std::vector<RecoveryFile> FindRecoveryFiles(
    std::string_view pref);

// The synchronous write of the session-end path: temp file, flush,
// rename.
[[nodiscard]] std::expected<void, Error> WriteRecoveryNow(
    const RecoveryFile& file);

// When a dirty document autosaves: after |quiet_ns| without input, once
// per revision.
class AutosaveTimer {
 public:
  explicit AutosaveTimer(std::uint64_t quiet_ns = kAutosaveQuietNs) noexcept
      : quiet_ns_(quiet_ns) {}

  // True when a document at |revision| (dirty) should autosave at
  // |now_ns|, input having last arrived at |last_input_ns|.
  [[nodiscard]] bool Due(std::uint64_t now_ns, std::uint64_t last_input_ns,
                         std::uint64_t revision, bool dirty) const noexcept;
  // The autosave of |revision| was submitted.
  void Submitted(std::uint64_t revision) noexcept { submitted_ = revision; }
  // The document was saved, closed or reopened: nothing is pending.
  void Reset(std::uint64_t revision) noexcept { submitted_ = revision; }
  [[nodiscard]] std::uint64_t SubmittedRevision() const noexcept {
    return submitted_;
  }

 private:
  std::uint64_t quiet_ns_;
  std::uint64_t submitted_ = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_RECOVERY_H_
