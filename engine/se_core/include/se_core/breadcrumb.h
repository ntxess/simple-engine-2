// Copyright 2026 The simple-engine-2 Authors.
//
// Per-thread breadcrumbs: what each thread is doing, packed in one 64-bit
// word so a watchdog or crash handler can read it with one relaxed load.
//
//   bits 60-63  role    (ThreadRole, 4 bits)
//   bits 56-59  phase   (schedule Phase, 4 bits)
//   bits 44-55  system  (schedule system index, 12 bits)
//   bits 28-43  chunk   (fork chunk index, 16 bits)
//   bits  0-27  tick    (sim tick modulo 2^28, about 25 days at 120 Hz)
//
// Each thread writes only its own breadcrumb (relaxed stores); M4 registers
// them as crash-dump streams.

#ifndef SE_CORE_BREADCRUMB_H_
#define SE_CORE_BREADCRUMB_H_

#include <atomic>
#include <cstdint>

namespace se {

enum class ThreadRole : std::uint8_t;

// The unpacked fields of a breadcrumb.
struct BreadcrumbFields {
  std::uint8_t role = 0;
  std::uint8_t phase = 0;
  std::uint16_t system = 0;
  std::uint16_t chunk = 0;
  std::uint32_t tick = 0;

  friend bool operator==(const BreadcrumbFields&,
                         const BreadcrumbFields&) = default;
};

inline constexpr int kBreadcrumbRoleShift = 60;
inline constexpr int kBreadcrumbPhaseShift = 56;
inline constexpr int kBreadcrumbSystemShift = 44;
inline constexpr int kBreadcrumbChunkShift = 28;
inline constexpr std::uint64_t kBreadcrumbRoleMask = 0xF;
inline constexpr std::uint64_t kBreadcrumbPhaseMask = 0xF;
inline constexpr std::uint64_t kBreadcrumbSystemMask = 0xFFF;
inline constexpr std::uint64_t kBreadcrumbChunkMask = 0xFFFF;
inline constexpr std::uint64_t kBreadcrumbTickMask = 0xFFFFFFF;

// Packs the fields; each is truncated to its width.
[[nodiscard]] constexpr std::uint64_t PackBreadcrumb(
    const BreadcrumbFields& f) noexcept {
  return ((f.role & kBreadcrumbRoleMask) << kBreadcrumbRoleShift) |
         ((f.phase & kBreadcrumbPhaseMask) << kBreadcrumbPhaseShift) |
         ((f.system & kBreadcrumbSystemMask) << kBreadcrumbSystemShift) |
         ((f.chunk & kBreadcrumbChunkMask) << kBreadcrumbChunkShift) |
         (f.tick & kBreadcrumbTickMask);
}

[[nodiscard]] constexpr BreadcrumbFields UnpackBreadcrumb(
    std::uint64_t packed) noexcept {
  BreadcrumbFields f;
  f.role = static_cast<std::uint8_t>((packed >> kBreadcrumbRoleShift) &
                                     kBreadcrumbRoleMask);
  f.phase = static_cast<std::uint8_t>((packed >> kBreadcrumbPhaseShift) &
                                      kBreadcrumbPhaseMask);
  f.system = static_cast<std::uint16_t>((packed >> kBreadcrumbSystemShift) &
                                        kBreadcrumbSystemMask);
  f.chunk = static_cast<std::uint16_t>((packed >> kBreadcrumbChunkShift) &
                                       kBreadcrumbChunkMask);
  f.tick = static_cast<std::uint32_t>(packed & kBreadcrumbTickMask);
  return f;
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to the cache line on purpose
#endif

// One thread's breadcrumb, on its own cache line.
class alignas(64) Breadcrumb {
 public:
  Breadcrumb() = default;
  Breadcrumb(const Breadcrumb&) = delete;
  Breadcrumb& operator=(const Breadcrumb&) = delete;

  void SetRole(ThreadRole role) noexcept;
  void SetPhase(std::uint8_t phase) noexcept;
  void SetSystem(std::uint16_t system) noexcept;
  void SetChunk(std::uint32_t chunk) noexcept;
  void SetTick(std::uint64_t tick) noexcept;

  [[nodiscard]] std::uint64_t Packed() const noexcept {
    return packed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] BreadcrumbFields Fields() const noexcept {
    return UnpackBreadcrumb(Packed());
  }

 private:
  // Replaces the bits selected by |mask| << |shift| with |value|. Only the
  // owning thread writes, so a load and a store suffice.
  void SetField(std::uint64_t value, std::uint64_t mask, int shift) noexcept;

  std::atomic<std::uint64_t> packed_{0};
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

// The calling thread's breadcrumb.
[[nodiscard]] Breadcrumb& ThreadBreadcrumb() noexcept;

}  // namespace se

#endif  // SE_CORE_BREADCRUMB_H_
