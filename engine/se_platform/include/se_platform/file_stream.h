// Copyright 2026 The simple-engine-2 Authors.
//
// Files read and written in steps (docs/milestone-specs.md M9): the
// background lane reads at most 1 MB and writes at most 1 MB per step, so
// it needs open files rather than se::fs::ReadFile's whole-file calls.
//
//   FileReader   opened for reading. On Windows with FILE_SHARE_READ |
//                FILE_SHARE_WRITE | FILE_SHARE_DELETE (ADR-080): another
//                process may rename over, replace or delete the file while
//                the engine reads it, so an editor saving a texture never
//                fails because the engine had it open. SDL's IOStream
//                opens with FILE_SHARE_READ only, so the Windows reader
//                uses CreateFileW; elsewhere it is an SDL_IOStream.
//   FileWriter   creates or truncates a file; Flush() reaches the device
//                (FlushFileBuffers; SDL_FlushIO).
//   TryReplace   renames a file over another; on Windows with POSIX
//                semantics (FileRenameInfoEx), so it replaces a file that
//                FILE_SHARE_DELETE readers have open (MoveFileExW on a
//                volume without it). kBusy reports a sharing violation
//                (another process holds the destination or the source open
//                without FILE_SHARE_DELETE; Windows), which the caller may
//                retry; other failures are Errors.
//
// Every call may run on any thread. The fault and delay injector
// (fs_faults.h) throttles FileWriter::Write and FileReader::Read and fakes
// sharing violations in TryReplace in test builds.

#ifndef SE_PLATFORM_FILE_STREAM_H_
#define SE_PLATFORM_FILE_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "se_core/error.h"

namespace se::fs {

class FileReader {
 public:
  // A missing file is kNotFound.
  [[nodiscard]] static std::expected<FileReader, Error> Open(
      std::string_view path);

  FileReader(FileReader&& other) noexcept;
  FileReader& operator=(FileReader&& other) noexcept;
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;
  ~FileReader();

  // The file's size when it was opened.
  [[nodiscard]] std::uint64_t Size() const noexcept { return size_; }
  // Bytes read so far.
  [[nodiscard]] std::uint64_t Position() const noexcept { return position_; }
  [[nodiscard]] bool AtEnd() const noexcept { return position_ >= size_; }
  [[nodiscard]] const std::string& Path() const noexcept { return path_; }

  // Reads up to |dest|.size() bytes at the current position; returns how
  // many (0 at the end of the file). The fault injector's read delay
  // applies.
  [[nodiscard]] std::expected<std::size_t, Error> Read(
      std::span<std::byte> dest);
  // The same, without the injector (se::fs::ReadFile).
  [[nodiscard]] std::expected<std::size_t, Error> ReadUnthrottled(
      std::span<std::byte> dest);

  void Close() noexcept;

 private:
  FileReader() = default;

  void* handle_ = nullptr;
  std::uint64_t size_ = 0;
  std::uint64_t position_ = 0;
  std::string path_;
};

class FileWriter {
 public:
  // Creates |path| or truncates it.
  [[nodiscard]] static std::expected<FileWriter, Error> Create(
      std::string_view path);

  FileWriter(FileWriter&& other) noexcept;
  FileWriter& operator=(FileWriter&& other) noexcept;
  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;
  // Closes without flushing to the device.
  ~FileWriter();

  [[nodiscard]] std::expected<void, Error> Write(
      std::span<const std::byte> bytes);
  // Flushes to the device and closes.
  [[nodiscard]] std::expected<void, Error> FlushAndClose();
  [[nodiscard]] std::uint64_t Written() const noexcept { return written_; }
  [[nodiscard]] const std::string& Path() const noexcept { return path_; }

 private:
  FileWriter() = default;
  void Close() noexcept;

  void* handle_ = nullptr;
  std::uint64_t written_ = 0;
  std::string path_;
};

enum class ReplaceOutcome : std::uint8_t {
  kReplaced,
  // A sharing violation: retry later.
  kBusy,
};

// Renames |from| to |to|, replacing |to| if it exists (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH; SDL_RenamePath).
[[nodiscard]] std::expected<ReplaceOutcome, Error> TryReplace(
    std::string_view from, std::string_view to);

// A temporary file name in |path|'s directory, unique within the process
// and, through the performance counter, across processes:
// "<path>.<counter>-<serial>.tmp".
[[nodiscard]] std::string UniqueTempPath(std::string_view path);

}  // namespace se::fs

#endif  // SE_PLATFORM_FILE_STREAM_H_
