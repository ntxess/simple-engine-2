// Copyright 2026 The simple-engine-2 Authors.
//
// What rampage's content files need besides themselves (M13b): the runtime
// loads a state's scene as a text through the background lane, then every
// file the text names (GameSetup::text_dependencies), so the state's World
// is built without touching a disk on the owner thread.

#ifndef GAME_RAMPAGE_CONTENT_H_
#define GAME_RAMPAGE_CONTENT_H_

#include <string>
#include <string_view>
#include <vector>

namespace se::rampage {

// The prefab references of the se-scene-v1 text |text| (the file |path|),
// as paths relative to the content root: |asset_dir|/<ref>, each once, in
// the order of their first use. Empty for a text that is not a scene (a
// prefab, a malformed file: the scene's load reports its error).
[[nodiscard]] std::vector<std::string> SceneDependencies(
    std::string_view path, std::string_view text, std::string_view asset_dir);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_CONTENT_H_
