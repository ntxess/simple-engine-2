// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/content.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game_rampage/json.h"
#include "game_rampage/scene.h"

namespace se::rampage {

std::vector<std::string> SceneDependencies(std::string_view /*path*/,
                                           std::string_view text,
                                           std::string_view asset_dir) {
  std::vector<std::string> paths;
  auto doc = json::Parse(text);
  if (!doc || !doc->IsObject()) return paths;
  const json::Value* format = doc->Find("format");
  if (format == nullptr || !format->IsString() ||
      format->Text() != kSceneFormat) {
    return paths;
  }
  const json::Value* entities = doc->Find("entities");
  if (entities == nullptr || !entities->IsArray()) return paths;
  for (const json::Value& entity : entities->Items()) {
    const json::Value* prefab = entity.Find("prefab");
    if (prefab == nullptr || !prefab->IsString() || prefab->Text().empty()) {
      continue;
    }
    std::string path(asset_dir);
    if (!path.empty()) path += '/';
    path += prefab->Text();
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

}  // namespace se::rampage
