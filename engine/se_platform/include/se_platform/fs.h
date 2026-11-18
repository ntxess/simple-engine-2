// Copyright 2026 The simple-engine-2 Authors.
//
// se::fs: the engine's file system access, on SDL's filesystem and
// SDL_IOStream API (the engine does not use <filesystem>; docs/style.md).
//
// Paths are UTF-8 strings. Join and Normalize work lexically and write '/'
// separators, which every supported platform accepts. Every fallible call
// returns std::expected; the Error names the path. Nothing here needs the
// main thread (SDL's filesystem functions are thread-safe), so the M9
// background lane may call it.

#ifndef SE_PLATFORM_FS_H_
#define SE_PLATFORM_FS_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se::fs {

enum class PathType : std::uint8_t { kNone, kFile, kDirectory, kOther };

struct PathInfo {
  // kNone when the path does not exist (not an error).
  PathType type = PathType::kNone;
  std::uint64_t size = 0;
  // The last modification, in nanoseconds since the Unix epoch (SDL_Time);
  // the background lane's hot-reload scan compares it (M9).
  std::int64_t modify_time_ns = 0;
};

// The directory of the executable (the content root's default), with a
// trailing '/'.
[[nodiscard]] std::expected<std::string, Error> BasePath();

// The per-user writable directory for |org| and |app|, created if needed,
// with a trailing '/'.
[[nodiscard]] std::expected<std::string, Error> PrefPath(std::string_view org,
                                                         std::string_view app);

// |base|/|relative|, normalised; |relative| when it is absolute.
[[nodiscard]] std::string Join(std::string_view base,
                               std::string_view relative);

// '/' separators, no repeated separators, "." segments removed and ".."
// segments folded into their parent where there is one. A drive prefix
// ("C:") and a leading '/' are kept; a trailing '/' is dropped except on a
// root.
[[nodiscard]] std::string Normalize(std::string_view path);

// The existing file or directory |path| names, as the file system spells
// it and normalised: absolute, symbolic links resolved and, on Windows, in
// the file's own case, so two spellings of one file give one string (a
// path that differs only in case names the same file there). Normalize(path)
// when it does not exist or cannot be resolved. Not lexical: it asks the
// file system.
[[nodiscard]] std::string Canonical(std::string_view path);

// True for "/x", "\\x" and "C:/x" style paths.
[[nodiscard]] bool IsAbsolute(std::string_view path) noexcept;

// The path without its last segment ("a/b/c" -> "a/b"; "c" -> "").
[[nodiscard]] std::string Parent(std::string_view path);

[[nodiscard]] std::expected<PathInfo, Error> Stat(std::string_view path);
[[nodiscard]] bool Exists(std::string_view path);

// The names in |directory| (not recursive, no "." or ".."), sorted.
[[nodiscard]] std::expected<std::vector<std::string>, Error> ListDirectory(
    std::string_view directory);

// Creates |directory| and any missing parents.
[[nodiscard]] std::expected<void, Error> CreateDirectories(
    std::string_view directory);

[[nodiscard]] std::expected<std::vector<std::byte>, Error> ReadFile(
    std::string_view path);
[[nodiscard]] std::expected<std::string, Error> ReadTextFile(
    std::string_view path);

// Replaces |path| with |bytes| (written through SDL_IOStream, then
// flushed to the device).
[[nodiscard]] std::expected<void, Error> WriteFile(
    std::string_view path, std::span<const std::byte> bytes);

// Writes |bytes| to a unique temporary file in |path|'s directory,
// flushes it to the device and renames it over |path|, so a reader sees
// the old contents or the new ones, never a partial file (M9's lane, M10's
// recovery files). The temporary file is removed when a step fails.
[[nodiscard]] std::expected<void, Error> WriteFileAtomic(
    std::string_view path, std::span<const std::byte> bytes);

// Renames |from| to |to|, replacing |to| if it exists.
[[nodiscard]] std::expected<void, Error> Rename(std::string_view from,
                                                std::string_view to);

// Removes a file or an empty directory. A path that does not exist is not
// an error.
[[nodiscard]] std::expected<void, Error> Remove(std::string_view path);

// The first of |start| and its parents (at most |max_levels| up) that
// contains |marker| (a relative path), normalised and without a trailing
// '/'.
[[nodiscard]] std::expected<std::string, Error> FindRootContaining(
    std::string_view start, std::string_view marker, int max_levels = 6);

}  // namespace se::fs

#endif  // SE_PLATFORM_FS_H_
