// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/user_settings.h"

#include <algorithm>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "se_core/error.h"
#include "se_platform/fs.h"
#include "src/json_reader.h"

namespace se {
namespace {

std::unexpected<Error> Bad(std::string message, std::string_view source) {
  return std::unexpected(
      Error{ErrorCode::kParse, std::move(message), std::string(source), 0, 0});
}

std::expected<double, Error> Volume(const json_reader::Node& section,
                                    std::string_view key,
                                    std::string_view source) {
  const json_reader::Node* value = section.Find(key);
  if (value == nullptr) return 100.0;
  if (value->kind != json_reader::Kind::kNumber) {
    return Bad(std::format("sound-settings/{} is not a number", key), source);
  }
  return std::clamp(value->number, 0.0, 100.0);
}

}  // namespace

std::expected<UserSettings, Error> ParseUserSettings(std::string_view json,
                                                     std::string_view source) {
  auto root = json_reader::Parse(json, source);
  if (!root) return std::unexpected(std::move(root.error()));
  if (root->kind != json_reader::Kind::kObject) {
    return Bad("the settings file is not an object", source);
  }
  UserSettings settings;
  if (const json_reader::Node* controls = root->Find("control-settings")) {
    if (controls->kind != json_reader::Kind::kObject) {
      return Bad("control-settings is not an object", source);
    }
    if (const json_reader::Node* keys = controls->Find("key-bindings")) {
      if (keys->kind != json_reader::Kind::kObject) {
        return Bad("control-settings/key-bindings is not an object", source);
      }
      for (const auto& [name, value] : keys->members) {
        if (value.kind != json_reader::Kind::kString) {
          return Bad(std::format("control-settings/key-bindings/{} is not a "
                                 "string",
                                 name),
                     source);
        }
        settings.key_bindings.emplace_back(name, value.text);
      }
    }
  }
  if (const json_reader::Node* sound = root->Find("sound-settings")) {
    if (sound->kind != json_reader::Kind::kObject) {
      return Bad("sound-settings is not an object", source);
    }
    auto music = Volume(*sound, "music-volume", source);
    if (!music) return std::unexpected(std::move(music.error()));
    auto effects = Volume(*sound, "effects-volume", source);
    if (!effects) return std::unexpected(std::move(effects.error()));
    settings.music_volume = *music;
    settings.effects_volume = *effects;
  }
  return settings;
}

std::expected<UserSettings, Error> LoadUserSettings(const std::string& path) {
  auto text = fs::ReadTextFile(path);
  if (!text) return std::unexpected(std::move(text.error()));
  return ParseUserSettings(*text, path);
}

std::expected<NameTable, Error> ParseNameTable(std::string_view json,
                                               std::string_view source) {
  auto root = json_reader::Parse(json, source);
  if (!root) return std::unexpected(std::move(root.error()));
  if (root->kind != json_reader::Kind::kObject) {
    return Bad("the table is not an object", source);
  }
  NameTable table;
  for (const auto& [name, value] : root->members) {
    if (value.kind != json_reader::Kind::kString) {
      return Bad(std::format("'{}' is not a path string", name), source);
    }
    table.emplace_back(name, value.text);
  }
  return table;
}

}  // namespace se
