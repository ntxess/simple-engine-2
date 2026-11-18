// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/fs.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_platform/file_stream.h"

namespace se::fs {
namespace {

std::unexpected<Error> IoError(std::string_view what, std::string_view path,
                               ErrorCode code = ErrorCode::kIo) {
  std::string message(what);
  const char* sdl = SDL_GetError();
  if (sdl != nullptr && *sdl != '\0') {
    message += ": ";
    message += sdl;
  }
  return std::unexpected(
      Error{code, std::move(message), std::string(path), 0, 0});
}

bool IsSeparator(char c) noexcept { return c == '/' || c == '\\'; }

bool HasDrive(std::string_view path) noexcept {
  return path.size() >= 2 && path[1] == ':' &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z'));
}

// Frees SDL allocations at scope exit.
class SdlFree {
 public:
  explicit SdlFree(void* pointer) noexcept : pointer_(pointer) {}
  SdlFree(const SdlFree&) = delete;
  SdlFree& operator=(const SdlFree&) = delete;
  ~SdlFree() { SDL_free(pointer_); }

 private:
  void* pointer_;
};

// Writes |bytes| to |path| through SDL_IOStream and flushes it to the
// device.
std::expected<void, Error> WriteAndFlush(const std::string& path,
                                         std::span<const std::byte> bytes) {
  SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "wb");
  if (io == nullptr) return IoError("cannot open for writing", path);
  const std::size_t written =
      bytes.empty() ? 0 : SDL_WriteIO(io, bytes.data(), bytes.size());
  if (written != bytes.size()) {
    auto error = IoError("write failed", path);
    SDL_CloseIO(io);
    return error;
  }
  if (!SDL_FlushIO(io)) {
    auto error = IoError("flush failed", path);
    SDL_CloseIO(io);
    return error;
  }
  if (!SDL_CloseIO(io)) return IoError("close failed", path);
  return {};
}

std::atomic<std::uint64_t> g_temp_serial{0};

}  // namespace

std::expected<std::string, Error> BasePath() {
  const char* base = SDL_GetBasePath();
  if (base == nullptr) return IoError("SDL_GetBasePath failed", "");
  std::string path = Normalize(base);
  if (path.empty() || path.back() != '/') path += '/';
  return path;
}

std::expected<std::string, Error> PrefPath(std::string_view org,
                                           std::string_view app) {
  const std::string org_text(org);
  const std::string app_text(app);
  char* pref = SDL_GetPrefPath(org_text.c_str(), app_text.c_str());
  if (pref == nullptr) {
    return IoError("SDL_GetPrefPath failed", org_text + "/" + app_text);
  }
  const SdlFree release(pref);
  std::string path = Normalize(pref);
  if (path.empty() || path.back() != '/') path += '/';
  return path;
}

bool IsAbsolute(std::string_view path) noexcept {
  if (!path.empty() && IsSeparator(path[0])) return true;
  return HasDrive(path) && path.size() >= 3 && IsSeparator(path[2]);
}

std::string Normalize(std::string_view path) {
  if (path.empty()) return {};
  std::string prefix;
  std::size_t pos = 0;
  if (HasDrive(path)) {
    prefix.assign(path.substr(0, 2));
    pos = 2;
  }
  bool rooted = false;
  if (pos < path.size() && IsSeparator(path[pos])) {
    rooted = true;
    // Keep a UNC "//server" prefix.
    if (pos == 0 && path.size() > 1 && IsSeparator(path[1])) {
      prefix = "/";
      pos = 1;
    }
  }
  std::vector<std::string_view> segments;
  while (pos <= path.size()) {
    std::size_t end = pos;
    while (end < path.size() && !IsSeparator(path[end])) ++end;
    const std::string_view segment = path.substr(pos, end - pos);
    if (segment.empty() || segment == ".") {
      // Repeated separators and "." add nothing.
    } else if (segment == "..") {
      if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
      } else if (!rooted) {
        segments.push_back(segment);
      }
    } else {
      segments.push_back(segment);
    }
    pos = end + 1;
  }
  std::string out = prefix;
  if (rooted) out += '/';
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) out += '/';
    out += segments[i];
  }
  if (out.empty()) out = ".";
  return out;
}

std::string Join(std::string_view base, std::string_view relative) {
  if (relative.empty()) return Normalize(base);
  if (base.empty() || IsAbsolute(relative)) return Normalize(relative);
  std::string joined(base);
  joined += '/';
  joined += relative;
  return Normalize(joined);
}

std::string Parent(std::string_view path) {
  const std::string normal = Normalize(path);
  const std::size_t slash = normal.find_last_of('/');
  if (slash == std::string::npos) {
    return HasDrive(normal) ? normal.substr(0, 2) : std::string();
  }
  // Keep the root of "/x" and "C:/x".
  const bool root_slash = slash == 0 || (slash == 2 && HasDrive(normal));
  return normal.substr(0, root_slash ? slash + 1 : slash);
}

std::expected<PathInfo, Error> Stat(std::string_view path) {
  const std::string text(path);
  SDL_PathInfo info{};
  PathInfo result;
  if (!SDL_GetPathInfo(text.c_str(), &info)) {
    // SDL reports a missing path as a failure; it is not an error here.
    return result;
  }
  switch (info.type) {
    case SDL_PATHTYPE_FILE:
      result.type = PathType::kFile;
      break;
    case SDL_PATHTYPE_DIRECTORY:
      result.type = PathType::kDirectory;
      break;
    case SDL_PATHTYPE_OTHER:
      result.type = PathType::kOther;
      break;
    case SDL_PATHTYPE_NONE:
      result.type = PathType::kNone;
      break;
  }
  result.size = info.size;
  result.modify_time_ns = info.modify_time;
  return result;
}

bool Exists(std::string_view path) {
  const auto info = Stat(path);
  return info && info->type != PathType::kNone;
}

std::expected<std::vector<std::string>, Error> ListDirectory(
    std::string_view directory) {
  const std::string text(directory);
  int count = 0;
  // "*" never matches a separator, so only the directory's own entries.
  char** names = SDL_GlobDirectory(text.c_str(), "*", 0, &count);
  if (names == nullptr) {
    return IoError("cannot list directory", text, ErrorCode::kNotFound);
  }
  const SdlFree release(static_cast<void*>(names));
  std::vector<std::string> entries;
  entries.reserve(static_cast<std::size_t>(std::max(count, 0)));
  for (int i = 0; i < count; ++i) {
    entries.emplace_back(names[i]);
  }
  std::ranges::sort(entries);
  return entries;
}

std::expected<void, Error> CreateDirectories(std::string_view directory) {
  const std::string text(directory);
  if (!SDL_CreateDirectory(text.c_str())) {
    return IoError("cannot create directory", text);
  }
  return {};
}

std::expected<std::vector<std::byte>, Error> ReadFile(std::string_view path) {
  // Through FileReader: on Windows the file is opened with
  // FILE_SHARE_DELETE, as every engine read is (ADR-080).
  auto reader = FileReader::Open(path);
  if (!reader) {
    Error error = std::move(reader.error());
    error.message = "cannot read file: " + error.message;
    if (error.path.empty()) error.path = std::string(path);
    return std::unexpected(std::move(error));
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(reader->Size()));
  std::size_t filled = 0;
  while (filled < bytes.size()) {
    auto got = reader->ReadUnthrottled(
        std::span<std::byte>(bytes.data() + filled, bytes.size() - filled));
    if (!got) return std::unexpected(std::move(got.error()));
    if (*got == 0) break;
    filled += *got;
  }
  // A file that shrank while it was read ends where it ended.
  bytes.resize(filled);
  return bytes;
}

std::expected<std::string, Error> ReadTextFile(std::string_view path) {
  auto bytes = ReadFile(path);
  if (!bytes) return std::unexpected(std::move(bytes.error()));
  std::string text(bytes->size(), '\0');
  if (!bytes->empty()) std::memcpy(text.data(), bytes->data(), bytes->size());
  return text;
}

std::expected<void, Error> WriteFile(std::string_view path,
                                     std::span<const std::byte> bytes) {
  return WriteAndFlush(std::string(path), bytes);
}

std::expected<void, Error> WriteFileAtomic(std::string_view path,
                                           std::span<const std::byte> bytes) {
  const std::string target(path);
  // Unique within the process (the serial) and across processes (the
  // performance counter at the first write differs between runs).
  const std::uint64_t serial =
      g_temp_serial.fetch_add(1, std::memory_order_relaxed);
  const std::string temp = std::format("{}.{:x}-{}.tmp", target,
                                       SDL_GetPerformanceCounter(), serial);
  if (auto written = WriteAndFlush(temp, bytes); !written) {
    SDL_RemovePath(temp.c_str());
    return written;
  }
  if (!SDL_RenamePath(temp.c_str(), target.c_str())) {
    auto error = IoError("cannot rename the temporary file over", target);
    SDL_RemovePath(temp.c_str());
    return error;
  }
  return {};
}

std::expected<void, Error> Rename(std::string_view from, std::string_view to) {
  const std::string from_text(from);
  const std::string to_text(to);
  if (!SDL_RenamePath(from_text.c_str(), to_text.c_str())) {
    return IoError("cannot rename " + from_text + " to", to_text);
  }
  return {};
}

std::expected<void, Error> Remove(std::string_view path) {
  const std::string text(path);
  if (!Exists(text)) return {};
  if (!SDL_RemovePath(text.c_str())) return IoError("cannot remove", text);
  return {};
}

std::expected<std::string, Error> FindRootContaining(std::string_view start,
                                                     std::string_view marker,
                                                     int max_levels) {
  std::string directory = Normalize(start);
  for (int level = 0; level <= max_levels; ++level) {
    if (Exists(Join(directory, marker))) return directory;
    std::string parent = Parent(directory);
    if (parent.empty() || parent == directory) break;
    directory = std::move(parent);
  }
  return std::unexpected(Error{ErrorCode::kNotFound,
                               std::format("no directory above {} contains {}",
                                           Normalize(start), marker),
                               std::string(start), 0, 0});
}

}  // namespace se::fs
