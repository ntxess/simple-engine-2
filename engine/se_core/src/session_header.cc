// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/session_header.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string_view>

#include "se_core/dump_stream.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"

namespace se {
namespace {

constexpr std::align_val_t kBlockAlignment{64};
constexpr std::size_t kBlockSize =
    sizeof(DumpStreamHeader) + sizeof(SessionHeaderRecord);
// The words of last_tick and last_hash.
constexpr std::size_t kLastTickWord =
    offsetof(SessionHeaderRecord, last_tick) / 8;
constexpr std::size_t kLastHashWord =
    offsetof(SessionHeaderRecord, last_hash) / 8;

template <std::size_t N>
void CopyText(char (&field)[N], std::string_view text) noexcept {
  std::memset(field, 0, N);
  std::copy_n(text.begin(), std::min(text.size(), N), field);
}

}  // namespace

SessionHeaderStream::SessionHeaderStream()
    : block_(::operator new(kBlockSize, kBlockAlignment)),
      header_(new (block_) DumpStreamHeader{}),
      words_(reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(block_) +
                                              sizeof(DumpStreamHeader))) {
  std::memset(words_, 0, sizeof(SessionHeaderRecord));
  InitDumpStreamHeader(*header_, DumpStreamKind::kSessionHeader,
                       sizeof(SessionHeaderRecord), 1, "session-header");
}

SessionHeaderStream::~SessionHeaderStream() {
  ::operator delete(block_, kBlockAlignment);
}

void SessionHeaderStream::Store(const SessionHeaderRecord& record) noexcept {
  std::array<std::uint64_t, kWords> words;
  std::memcpy(words.data(), &record, sizeof(SessionHeaderRecord));
  const std::uint64_t stamp =
      std::atomic_ref<std::uint64_t>(words_[0]).load(std::memory_order_relaxed);
  std::atomic_ref<std::uint64_t>(words_[0]).store(stamp + 1,
                                                  std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (std::size_t i = 1; i < kWords; ++i) {
    std::atomic_ref<std::uint64_t>(words_[i]).store(words[i],
                                                    std::memory_order_relaxed);
  }
  std::atomic_ref<std::uint64_t>(words_[0]).store(stamp + 2,
                                                  std::memory_order_release);
}

void SessionHeaderStream::Begin(const SessionHeaderInfo& info) noexcept {
  SessionHeaderRecord record;
  record.session = current_.session + 1;
  record.seed = info.seed;
  record.scene_hash = info.scene_hash;
  record.math_hash = info.math_hash;
  record.cpu_features = info.cpu_features;
  record.start_tick = info.start_tick;
  record.first_frame = info.first_frame;
  record.tick_rate = info.tick_rate;
  CopyText(record.module, info.module);
  CopyText(record.build, info.build);
  CopyText(record.platform, info.platform);
  CopyText(record.compiler, info.compiler);
  CopyText(record.scene, info.scene);
  current_ = record;
  Store(current_);
  // The writer, as the other streams name theirs.
  AtomicStore(header_->os_thread_id, CurrentOsThreadId(),
              std::memory_order_relaxed);
  AtomicStore(header_->role, static_cast<std::uint8_t>(CurrentRole()),
              std::memory_order_relaxed);
  AtomicStore(header_->head, std::uint64_t{1}, std::memory_order_release);
}

void SessionHeaderStream::PublishTick(Tick tick, std::uint64_t hash) noexcept {
  current_.last_tick = tick;
  current_.last_hash = hash;
  const std::uint64_t stamp =
      std::atomic_ref<std::uint64_t>(words_[0]).load(std::memory_order_relaxed);
  std::atomic_ref<std::uint64_t>(words_[0]).store(stamp + 1,
                                                  std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::atomic_ref<std::uint64_t>(words_[kLastTickWord])
      .store(tick, std::memory_order_relaxed);
  std::atomic_ref<std::uint64_t>(words_[kLastHashWord])
      .store(hash, std::memory_order_relaxed);
  std::atomic_ref<std::uint64_t>(words_[0]).store(stamp + 2,
                                                  std::memory_order_release);
}

std::uint64_t SessionHeaderStream::Sessions() const noexcept {
  SessionHeaderRecord record;
  return Read(record) ? record.session : 0;
}

bool SessionHeaderStream::Read(SessionHeaderRecord& out) const noexcept {
  if (AtomicLoad(header_->head, std::memory_order_acquire) == 0) return false;
  const std::uint64_t before = AtomicLoad(words_[0], std::memory_order_acquire);
  if (before % 2 != 0) return false;
  std::array<std::uint64_t, kWords> words;
  words[0] = before;
  for (std::size_t i = 1; i < kWords; ++i) {
    words[i] = AtomicLoad(words_[i], std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (AtomicLoad(words_[0], std::memory_order_relaxed) != before) return false;
  std::memcpy(&out, words.data(), sizeof(SessionHeaderRecord));
  return true;
}

std::span<const std::byte> SessionHeaderStream::StreamBytes() const noexcept {
  return {static_cast<const std::byte*>(block_), kBlockSize};
}

}  // namespace se
