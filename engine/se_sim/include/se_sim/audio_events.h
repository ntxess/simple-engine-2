// Copyright 2026 The simple-engine-2 Authors.
//
// AudioEvents: the sounds a World's systems ask for during a tick
// (docs/milestone-specs.md M8; ADR-077). An engine resource in the World's
// ctx; systems push AudioEvent records (a sound key, the group, a cosmetic
// flag, a position for pan), and the SimDriver's AudioSink drains them
// after each tick: the runtime's AudioBridge schedules them, the null sink
// (headless runs, tests) drops them. Audio events never change simulation
// state: the ctx is not part of the World hash, and nothing reads the
// events back.
//
//   if (AudioEvents* audio = FindAudioEvents(world)) {
//     audio->Push({.sound = SoundKeyOf("hit"), .position = pos,
//                  .positional = true});
//   }

#ifndef SE_SIM_AUDIO_EVENTS_H_
#define SE_SIM_AUDIO_EVENTS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "se_core/hash.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_core/world.h"

namespace se {

enum class AudioGroup : std::uint8_t { kMusic, kEffects };

struct AudioEvent {
  // SoundKeyOf(name) of a sound in the game's sound table.
  std::uint32_t sound = 0;
  AudioGroup group = AudioGroup::kEffects;
  // Cosmetic sounds may be dropped when they would start more than 100 ms
  // late; gameplay sounds always play.
  bool cosmetic = false;
  // Pan from |position| (world units) when set.
  bool positional = false;
  Vec2 position{};

  friend bool operator==(const AudioEvent&, const AudioEvent&) = default;
};

static_assert(std::is_trivially_copyable_v<AudioEvent>,
              "audio events are copied into rings and snapshots");

// The key of a sound name (the sound table's names).
[[nodiscard]] constexpr std::uint32_t SoundKeyOf(
    std::string_view name) noexcept {
  return Fnv1a32(name);
}

class AudioEvents {
 public:
  // Events kept per tick; more are dropped and counted.
  static constexpr std::size_t kCapacity = 256;

  AudioEvents() { events_.reserve(kCapacity); }

  void Push(const AudioEvent& event) {
    if (events_.size() >= kCapacity) {
      ++dropped_;
      return;
    }
    events_.push_back(event);
  }
  [[nodiscard]] std::span<const AudioEvent> Events() const noexcept {
    return events_;
  }
  [[nodiscard]] bool Empty() const noexcept { return events_.empty(); }
  void Clear() noexcept { events_.clear(); }
  // Events beyond kCapacity in one tick, so far.
  [[nodiscard]] std::uint64_t Dropped() const noexcept { return dropped_; }

  // An opaque token the AudioSink keeps with the World (the AudioBridge's
  // voice scope): it is destroyed with the World, which ends the World's
  // voices.
  void SetOwnerToken(std::shared_ptr<void> token) noexcept {
    owner_token_ = std::move(token);
  }
  [[nodiscard]] const std::shared_ptr<void>& OwnerToken() const noexcept {
    return owner_token_;
  }

 private:
  std::vector<AudioEvent> events_;
  std::uint64_t dropped_ = 0;
  std::shared_ptr<void> owner_token_;
};

// The World's AudioEvents, or nullptr when it has none.
[[nodiscard]] inline AudioEvents* FindAudioEvents(World& world)
    SE_REQUIRES(g_sim_owner) {
  return world.Reg().ctx().find<AudioEvents>();
}

// Gives |world| its AudioEvents (once; a game state does it when it builds
// the World).
inline AudioEvents& AddAudioEvents(World& world) SE_REQUIRES(g_sim_owner) {
  auto& ctx = world.Reg().ctx();
  if (AudioEvents* events = ctx.find<AudioEvents>()) return *events;
  return ctx.emplace<AudioEvents>();
}

}  // namespace se

#endif  // SE_SIM_AUDIO_EVENTS_H_
