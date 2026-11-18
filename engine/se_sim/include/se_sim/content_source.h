// Copyright 2026 The simple-engine-2 Authors.
//
// ContentSource (docs/milestone-specs.md M13, "The content contract"): the
// files a GameState builds its Worlds from (scenes, prefabs), by path
// relative to the content root. The runtime's source is the AssetRuntime's
// text assets: a state is created only once its files are loaded and its
// assets resident (the stack's content gate, game_state.h), so Text() never
// touches a disk on the owner thread. Tools and tests read synchronously
// through CachedContentSource; replays of multi-state sessions read a
// content bundle (content_bundle.h).
//
// Paths are compared as given: callers use the content-relative form with
// forward slashes ("assets/scenes/sandbox.scene.json"). A returned view
// stays valid while the source lives and, for the runtime's source, while
// the state that loaded the text is on the stack (ADR-122).

#ifndef SE_SIM_CONTENT_SOURCE_H_
#define SE_SIM_CONTENT_SOURCE_H_

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "se_core/error.h"

namespace se {

// The files a GameState builds its Worlds from (scenes, prefabs), by path
// relative to the content root. The runtime's source is the AssetRuntime's
// text assets: a state is created only once its files are loaded and its
// assets resident (the stack's content gate), so Text() never touches a
// disk on the owner thread. Tools and tests read synchronously.
class ContentSource {
 public:
  virtual ~ContentSource() = default;
  // The text of |path|; an Error when it is not loaded or does not exist.
  [[nodiscard]] virtual std::expected<std::string_view, Error> Text(
      std::string_view path) const = 0;
};

// Reads through |read| on first use and keeps the text (se_replay, tests).
// A content bundle (content_bundle.h) is the other source tools use.
class CachedContentSource final : public ContentSource {
 public:
  using Reader =
      std::function<std::expected<std::string, Error>(std::string_view)>;
  explicit CachedContentSource(Reader read);
  [[nodiscard]] std::expected<std::string_view, Error> Text(
      std::string_view path) const override;

 private:
  Reader read_;
  mutable std::map<std::string, std::string, std::less<>> texts_;
};

}  // namespace se

#endif  // SE_SIM_CONTENT_SOURCE_H_
