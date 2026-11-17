// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/file_stream.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace se::fs {
namespace {

std::atomic<std::uint64_t> g_temp_serial{0};

#if defined(_WIN32)

std::wstring Widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
  }
  return wide;
}

std::unexpected<Error> WinError(std::string_view what, std::string_view path,
                                DWORD code) {
  const ErrorCode kind =
      code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
          ? ErrorCode::kNotFound
          : ErrorCode::kIo;
  return std::unexpected(Error{kind,
                               std::format("{} (Win32 error {})", what,
                                           static_cast<std::uint32_t>(code)),
                               std::string(path), 0, 0});
}

HANDLE AsHandle(void* handle) noexcept { return static_cast<HANDLE>(handle); }

// FileRenameInfoEx's flags (winbase.h, Windows 10 1709 and later).
constexpr DWORD kRenameReplaceIfExists = 0x1;
constexpr DWORD kRenamePosixSemantics = 0x2;
constexpr auto kFileRenameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

// Renames |from| over |to| with POSIX semantics: the destination is
// replaced even while a reader that allowed FILE_SHARE_DELETE (every
// engine read, ADR-080) has it open; that reader keeps the old contents.
// False with |code| set on failure.
bool PosixReplace(const std::wstring& from, const std::wstring& to,
                  DWORD& code) {
  const HANDLE source =
      CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (source == INVALID_HANDLE_VALUE) {
    code = GetLastError();
    return false;
  }
  // A full path with Windows separators.
  std::wstring target = to;
  for (wchar_t& c : target) {
    if (c == L'/') c = L'\\';
  }
  const std::size_t name_bytes = target.size() * sizeof(wchar_t);
  std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO, FileName) +
                                name_bytes + sizeof(wchar_t));
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  // The Flags member of FileRenameInfoEx shares the first field with
  // FileRenameInfo's ReplaceIfExists.
  const DWORD flags = kRenameReplaceIfExists | kRenamePosixSemantics;
  std::memcpy(buffer.data(), &flags, sizeof(flags));
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(name_bytes);
  std::memcpy(info->FileName, target.data(), name_bytes);
  const BOOL renamed = SetFileInformationByHandle(
      source, kFileRenameInfoEx, info, static_cast<DWORD>(buffer.size()));
  code = renamed != FALSE ? ERROR_SUCCESS : GetLastError();
  CloseHandle(source);
  return renamed != FALSE;
}

#else

std::unexpected<Error> SdlError(std::string_view what, std::string_view path,
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

SDL_IOStream* AsStream(void* handle) noexcept {
  return static_cast<SDL_IOStream*>(handle);
}

#endif

}  // namespace

// --- FileReader ---

std::expected<FileReader, Error> FileReader::Open(std::string_view path) {
  FileReader reader;
  reader.path_ = std::string(path);
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(Widen(path).c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return WinError("cannot open for reading", path, GetLastError());
  }
  reader.handle_ = handle;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == FALSE) {
    return WinError("cannot read the file size", path, GetLastError());
  }
  reader.size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
  SDL_IOStream* stream = SDL_IOFromFile(reader.path_.c_str(), "rb");
  if (stream == nullptr) {
    const ErrorCode code = Exists(path) ? ErrorCode::kIo : ErrorCode::kNotFound;
    return SdlError("cannot open for reading", path, code);
  }
  reader.handle_ = stream;
  const Sint64 size = SDL_GetIOSize(stream);
  if (size < 0) return SdlError("cannot read the file size", path);
  reader.size_ = static_cast<std::uint64_t>(size);
#endif
  return reader;
}

FileReader::FileReader(FileReader&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      size_(other.size_),
      position_(other.position_),
      path_(std::move(other.path_)) {}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = std::exchange(other.handle_, nullptr);
    size_ = other.size_;
    position_ = other.position_;
    path_ = std::move(other.path_);
  }
  return *this;
}

FileReader::~FileReader() { Close(); }

void FileReader::Close() noexcept {
  if (handle_ == nullptr) return;
#if defined(_WIN32)
  CloseHandle(AsHandle(handle_));
#else
  SDL_CloseIO(AsStream(handle_));
#endif
  handle_ = nullptr;
}

std::expected<std::size_t, Error> FileReader::Read(std::span<std::byte> dest) {
  if (handle_ != nullptr && !dest.empty() && !AtEnd()) {
    internal::BeforeReadStep();
  }
  return ReadUnthrottled(dest);
}

std::expected<std::size_t, Error> FileReader::ReadUnthrottled(
    std::span<std::byte> dest) {
  if (handle_ == nullptr) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument,
                                 "the reader is closed", path_, 0, 0});
  }
  if (dest.empty() || AtEnd()) return std::size_t{0};
#if defined(_WIN32)
  const DWORD wanted = static_cast<DWORD>(
      std::min<std::size_t>(dest.size(), std::numeric_limits<DWORD>::max()));
  DWORD got = 0;
  if (::ReadFile(AsHandle(handle_), dest.data(), wanted, &got, nullptr) ==
      FALSE) {
    return WinError("read failed", path_, GetLastError());
  }
  position_ += got;
  return static_cast<std::size_t>(got);
#else
  const std::size_t got =
      SDL_ReadIO(AsStream(handle_), dest.data(), dest.size());
  if (got == 0 && SDL_GetIOStatus(AsStream(handle_)) == SDL_IO_STATUS_ERROR) {
    return SdlError("read failed", path_);
  }
  position_ += got;
  return got;
#endif
}

// --- FileWriter ---

std::expected<FileWriter, Error> FileWriter::Create(std::string_view path) {
  FileWriter writer;
  writer.path_ = std::string(path);
#if defined(_WIN32)
  const HANDLE handle =
      CreateFileW(Widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return WinError("cannot open for writing", path, GetLastError());
  }
  writer.handle_ = handle;
#else
  SDL_IOStream* stream = SDL_IOFromFile(writer.path_.c_str(), "wb");
  if (stream == nullptr) return SdlError("cannot open for writing", path);
  writer.handle_ = stream;
#endif
  return writer;
}

FileWriter::FileWriter(FileWriter&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      written_(other.written_),
      path_(std::move(other.path_)) {}

FileWriter& FileWriter::operator=(FileWriter&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = std::exchange(other.handle_, nullptr);
    written_ = other.written_;
    path_ = std::move(other.path_);
  }
  return *this;
}

FileWriter::~FileWriter() { Close(); }

void FileWriter::Close() noexcept {
  if (handle_ == nullptr) return;
#if defined(_WIN32)
  CloseHandle(AsHandle(handle_));
#else
  SDL_CloseIO(AsStream(handle_));
#endif
  handle_ = nullptr;
}

std::expected<void, Error> FileWriter::Write(std::span<const std::byte> bytes) {
  if (handle_ == nullptr) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument,
                                 "the writer is closed", path_, 0, 0});
  }
  internal::BeforeWriteStep();
  while (!bytes.empty()) {
#if defined(_WIN32)
    const DWORD wanted = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
    DWORD done = 0;
    if (::WriteFile(AsHandle(handle_), bytes.data(), wanted, &done, nullptr) ==
            FALSE ||
        done == 0) {
      return WinError("write failed", path_, GetLastError());
    }
    const std::size_t wrote = done;
#else
    const std::size_t wrote =
        SDL_WriteIO(AsStream(handle_), bytes.data(), bytes.size());
    if (wrote == 0) return SdlError("write failed", path_);
#endif
    written_ += wrote;
    bytes = bytes.subspan(wrote);
  }
  return {};
}

std::expected<void, Error> FileWriter::FlushAndClose() {
  if (handle_ == nullptr) {
    return std::unexpected(Error{ErrorCode::kInvalidArgument,
                                 "the writer is closed", path_, 0, 0});
  }
#if defined(_WIN32)
  if (FlushFileBuffers(AsHandle(handle_)) == FALSE) {
    auto error = WinError("flush failed", path_, GetLastError());
    Close();
    return error;
  }
  const BOOL closed = CloseHandle(AsHandle(handle_));
  handle_ = nullptr;
  if (closed == FALSE) return WinError("close failed", path_, GetLastError());
#else
  if (!SDL_FlushIO(AsStream(handle_))) {
    auto error = SdlError("flush failed", path_);
    Close();
    return error;
  }
  const bool closed = SDL_CloseIO(AsStream(handle_));
  handle_ = nullptr;
  if (!closed) return SdlError("close failed", path_);
#endif
  return {};
}

// --- Rename ---

std::expected<ReplaceOutcome, Error> TryReplace(std::string_view from,
                                                std::string_view to) {
  if (internal::RefuseReplace(to)) return ReplaceOutcome::kBusy;
#if defined(_WIN32)
  const std::wstring wide_from = Widen(from);
  const std::wstring wide_to = Widen(to);
  DWORD code = ERROR_SUCCESS;
  if (PosixReplace(wide_from, wide_to, code)) return ReplaceOutcome::kReplaced;
  if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED ||
      code == ERROR_INVALID_FUNCTION) {
    // The volume or the system has no POSIX rename (FAT, before Windows
    // 10 1709): the classic replace, which fails over an open file.
    if (MoveFileExW(wide_from.c_str(), wide_to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
        FALSE) {
      return ReplaceOutcome::kReplaced;
    }
    code = GetLastError();
  }
  // Another process has the destination (or the source) open without
  // FILE_SHARE_DELETE: Windows reports a sharing violation, or access
  // denied when the destination is open; both clear when it closes.
  if (code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED ||
      code == ERROR_LOCK_VIOLATION) {
    return ReplaceOutcome::kBusy;
  }
  return WinError(std::format("cannot rename {} over", from), to, code);
#else
  const std::string from_text(from);
  const std::string to_text(to);
  if (!SDL_RenamePath(from_text.c_str(), to_text.c_str())) {
    return SdlError(std::format("cannot rename {} over", from_text), to_text);
  }
  return ReplaceOutcome::kReplaced;
#endif
}

void ReleaseThreadState() noexcept { SDL_CleanupTLS(); }

std::string UniqueTempPath(std::string_view path) {
  const std::uint64_t serial =
      g_temp_serial.fetch_add(1, std::memory_order_relaxed);
  return std::format("{}.{:x}-{}.tmp", path, SDL_GetPerformanceCounter(),
                     serial);
}

}  // namespace se::fs
