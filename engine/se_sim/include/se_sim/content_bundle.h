// Copyright 2026 The simple-engine-2 Authors.
//
// Content bundles (docs/milestone-specs.md M13a): every text a session's
// states loaded through their ContentSource, each version once, as the
// start bytes of a replay or a dump of a multi-state session. The format
// is one JSON object (ADR-123):
//
//   {"format": "se-content-bundle-v1", "start_state": "title",
//    "texts": [{"path": "assets/scenes/title.scene.json",
//               "hash": "0123456789abcdef", "text": "..."}, ...]}
//
// "path" is content-relative, "hash" is ContentTextHash(text) as 16
// lower-case hex digits (a JSON number would lose bits above 2^53 in most
// readers), the texts are in the order the session first loaded them, and
// "start_state" is the state the session started in (empty: unknown).
//
// ContentBundleSource serves a bundle: each path's version is its first
// in the bundle until a kAssetSwap entry of the session's SessionLog
// selects another by hash (the runtime logs one, against the tick before
// which the state is created, whenever a state loads a version that
// differs from the one the path served before). As a StateContentGate it
// is always ready and, before a state is created, selects every swap due
// by the tick the session runs next, so a replay reads what the live
// session read.

#ifndef SE_SIM_CONTENT_BUNDLE_H_
#define SE_SIM_CONTENT_BUNDLE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_sim/content_source.h"
#include "se_sim/game_state.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"

namespace se {

inline constexpr std::string_view kContentBundleFormat = "se-content-bundle-v1";

// The hash a bundle and a kAssetSwap entry carry of a text
// (ContentHash of its bytes).
[[nodiscard]] std::uint64_t ContentTextHash(std::string_view text) noexcept;

// The kAssetSwap target of a content-relative path: a fixed hash of it.
[[nodiscard]] Guid ContentPathGuid(std::string_view path) noexcept;

struct ContentText {
  std::string path;
  std::uint64_t hash = 0;
  std::string text;

  friend bool operator==(const ContentText&, const ContentText&) = default;
};

class ContentBundle {
 public:
  // Adds a version of |path|; false when the bundle already holds it.
  bool Add(std::string_view path, std::string_view text);
  [[nodiscard]] std::span<const ContentText> Texts() const noexcept {
    return texts_;
  }
  [[nodiscard]] bool Empty() const noexcept { return texts_.empty(); }
  // The newest version the bundle holds for |path|, or null.
  [[nodiscard]] const ContentText* Latest(std::string_view path) const;

  [[nodiscard]] const std::string& StartState() const noexcept {
    return start_state_;
  }
  void SetStartState(std::string_view state) { start_state_ = state; }

  // The se-content-bundle-v1 JSON text, as bytes.
  [[nodiscard]] std::vector<std::byte> Write() const;
  // Errors carry the line and column of a malformed bundle.
  [[nodiscard]] static std::expected<ContentBundle, Error> Read(
      std::span<const std::byte> bytes);
  // Whether |bytes| look like a bundle (a JSON object whose "format" is
  // se-content-bundle-v1); cheap, for start bytes of either kind.
  [[nodiscard]] static bool IsBundle(std::span<const std::byte> bytes);

  friend bool operator==(const ContentBundle&, const ContentBundle&) = default;

 private:
  std::string start_state_;
  std::vector<ContentText> texts_;
};

class ContentBundleSource final : public ContentSource,
                                  public StateContentGate {
 public:
  explicit ContentBundleSource(ContentBundle bundle);

  // The selected version of |path|.
  [[nodiscard]] std::expected<std::string_view, Error> Text(
      std::string_view path) const override;
  // Selects the version of the path with |path_guid| whose hash is |hash|
  // (a kAssetSwap entry); false when the bundle has no such version.
  bool Select(Guid path_guid, std::uint64_t hash);
  // Selects every kAssetSwap entry of |log| due by |next_tick| (applied
  // or pending) that it has not selected yet, in the log's order.
  void SelectDue(const SessionLog& log, Tick next_tick);

  // The gate replays set on their stack: before each state is created it
  // selects the swaps of |log| due by |driver|'s next tick. Never holds a
  // state. Both must outlive their use here (null: no selection).
  void Follow(const SessionLog* log, const SimDriver* driver) noexcept {
    log_ = log;
    driver_ = driver;
  }
  [[nodiscard]] bool Ready(std::string_view state_id) override
      SE_REQUIRES(g_sim_owner);

  [[nodiscard]] const ContentBundle& Bundle() const noexcept { return bundle_; }
  // Swaps selected so far, and those that named no version of the bundle.
  [[nodiscard]] std::uint32_t Selections() const noexcept {
    return selections_;
  }
  [[nodiscard]] std::uint32_t UnknownSwaps() const noexcept {
    return unknown_swaps_;
  }

 private:
  ContentBundle bundle_;
  // Per text of the bundle: whether it is its path's selected version.
  std::vector<bool> selected_;
  // The (apply_before, seq) of the last swap SelectDue took.
  Tick swap_tick_ = 0;
  std::uint32_t swap_seq_ = 0;
  bool any_swap_ = false;
  const SessionLog* log_ = nullptr;
  const SimDriver* driver_ = nullptr;
  std::uint32_t selections_ = 0;
  std::uint32_t unknown_swaps_ = 0;
};

}  // namespace se

#endif  // SE_SIM_CONTENT_BUNDLE_H_
