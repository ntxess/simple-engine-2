// Copyright 2026 The simple-engine-2 Authors.
//
// Handle<T>{index, generation}: a typed reference into a slot table
// (docs/design/mt-design.md "Audio And Assets"). The index is the slot
// plus one, so the zero handle is null; the generation changes when the
// slot is freed, so a handle to a removed entry is stale rather than
// pointing at its successor. M7's GpuResources names its texture slots
// with Handle<Texture>; M9's AssetRegistry names its typed stores with
// Handle<TextureAsset>, Handle<SoundAsset> and so on (ADR-081).

#ifndef SE_CORE_HANDLE_H_
#define SE_CORE_HANDLE_H_

#include <cstdint>

namespace se {

template <class T>
struct Handle {
  // Slot index + 1; 0 is the null handle.
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  [[nodiscard]] constexpr bool IsNull() const noexcept { return index == 0; }
  friend constexpr bool operator==(const Handle&, const Handle&) = default;
};

// The tag of GpuResources' texture slots (se_render).
struct Texture {};
using TextureHandle = Handle<Texture>;

}  // namespace se

#endif  // SE_CORE_HANDLE_H_
