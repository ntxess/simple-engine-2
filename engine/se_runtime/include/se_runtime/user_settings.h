// Copyright 2026 The simple-engine-2 Authors.
//
// The user settings the runtime reads from the game's settings file
// (GameSetup::settings_file, config/config.json for rampage; docs/milestone-
// specs.md M8): the key bindings ("control-settings" / "key-bindings": an
// object of strings, the legacy names key-up, key-left, ..., key-6) and the
// sound volumes ("sound-settings": "music-volume" and "effects-volume",
// 0-100). Everything else in the file belongs to other readers and is
// ignored. Also the name tables the runtime loads at boot (config/sound.json:
// an object of name: path strings, like config/texture.json).

#ifndef SE_RUNTIME_USER_SETTINGS_H_
#define SE_RUNTIME_USER_SETTINGS_H_

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se {

struct UserSettings {
  // control-settings / key-bindings, in file order: {"key-up", "w"}.
  std::vector<std::pair<std::string, std::string>> key_bindings;
  // sound-settings, clamped to [0, 100]; 100 when absent.
  double music_volume = 100.0;
  double effects_volume = 100.0;
};

// Parses a settings document; |source| names it in errors. A section of the
// wrong type, a binding that is not a string or a volume that is not a
// number is an Error with the source.
[[nodiscard]] std::expected<UserSettings, Error> ParseUserSettings(
    std::string_view json, std::string_view source);

// Reads and parses |path| (se::fs).
[[nodiscard]] std::expected<UserSettings, Error> LoadUserSettings(
    const std::string& path);

// A name table: {"name": "path", ...}, in file order.
using NameTable = std::vector<std::pair<std::string, std::string>>;
[[nodiscard]] std::expected<NameTable, Error> ParseNameTable(
    std::string_view json, std::string_view source);

}  // namespace se

#endif  // SE_RUNTIME_USER_SETTINGS_H_
