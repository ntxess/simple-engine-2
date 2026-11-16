// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/recovery.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_platform/fs.h"
#include "src/json_value.h"

namespace se::editor {
namespace {

// A file-name-safe form of a document name.
std::string Stem(std::string_view name) {
  std::string stem;
  for (const char ch : name) {
    const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    stem.push_back(safe ? ch : '_');
    if (stem.size() >= 32) break;
  }
  return stem.empty() ? std::string("document") : stem;
}

json::Value Text(std::string_view text) {
  json::Value value;
  value.kind = json::Kind::kString;
  value.text = std::string(text);
  return value;
}

}  // namespace

std::string RecoveryDirectory(std::string_view pref) {
  return fs::Join(pref, "recovery") + "/";
}

std::string RecoveryPathFor(std::string_view pref, std::string_view document,
                            std::string_view name) {
  const std::string key =
      document.empty() ? std::string(name) : fs::Normalize(document);
  Hasher hasher(0x5ECE0BE5);
  hasher.AddString(key);
  return std::format("{}{}-{:016x}{}", RecoveryDirectory(pref), Stem(name),
                     hasher.Finish(), kRecoverySuffix);
}

std::string EncodeRecovery(const RecoveryFile& file) {
  json::Value root = json::Value::Object();
  root.Set("format", Text(kRecoveryFormat));
  root.Set("document", Text(file.document));
  root.Set("name", Text(file.name));
  root.Set("revision", json::Value::Number(static_cast<double>(file.revision)));
  root.Set("scene", Text(file.scene));
  return json::Write(root);
}

std::expected<RecoveryFile, Error> DecodeRecovery(std::string_view text,
                                                  std::string path) {
  auto root = json::Parse(text);
  if (!root) {
    Error error = std::move(root.error());
    error.path = path;
    return std::unexpected(std::move(error));
  }
  const json::Value* format = root->Find("format");
  const json::Value* document = root->Find("document");
  const json::Value* name = root->Find("name");
  const json::Value* revision = root->Find("revision");
  const json::Value* scene = root->Find("scene");
  if (format == nullptr || format->text != kRecoveryFormat ||
      document == nullptr || name == nullptr || revision == nullptr ||
      scene == nullptr || scene->kind != json::Kind::kString) {
    return std::unexpected(
        Error{ErrorCode::kParse, "not an se-recovery-v1 file", path, 0, 0});
  }
  RecoveryFile file;
  file.path = std::move(path);
  file.document = document->text;
  file.name = name->text;
  file.revision = static_cast<std::uint64_t>(revision->AsDouble().value_or(0));
  file.scene = scene->text;
  return file;
}

std::vector<RecoveryFile> FindRecoveryFiles(std::string_view pref) {
  std::vector<RecoveryFile> files;
  const std::string directory = RecoveryDirectory(pref);
  auto names = fs::ListDirectory(directory);
  if (!names) return files;
  for (const std::string& name : *names) {
    if (!name.ends_with(kRecoverySuffix)) continue;
    const std::string path = directory + name;
    auto text = fs::ReadTextFile(path);
    if (!text) continue;
    auto file = DecodeRecovery(*text, path);
    if (file) files.push_back(std::move(*file));
  }
  return files;
}

std::expected<void, Error> WriteRecoveryNow(const RecoveryFile& file) {
  if (auto made = fs::CreateDirectories(fs::Parent(file.path)); !made) {
    return made;
  }
  const std::string text = EncodeRecovery(file);
  return fs::WriteFileAtomic(file.path, std::as_bytes(std::span(text)));
}

bool AutosaveTimer::Due(std::uint64_t now_ns, std::uint64_t last_input_ns,
                        std::uint64_t revision, bool dirty) const noexcept {
  if (!dirty || revision == submitted_) return false;
  return now_ns >= last_input_ns && now_ns - last_input_ns >= quiet_ns_;
}

}  // namespace se::editor
