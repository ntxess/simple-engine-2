// Copyright 2026 The simple-engine-2 Authors.
//
// ParallelView<C...>: the only World access inside a fork chunk. World::
// Parallel() builds it on the owner thread, capturing the storages of C...
// before the fork, so chunks never touch the registry (EnTT is not
// thread-safe; World::Reg() asserts not-in-fork).
//
//   auto view = world.Parallel<Transform, Mover>(site);
//   jobs.ParallelFor(site, view.size(), [&](jobs::ChunkRange r) {
//     view.ForChunk(r, [&](entt::entity e) {
//       view.Write<Transform>(e).pos = Orbit(view.Read<Mover>(e), t);
//     });
//   });
//
// The view walks the first component's storage in packed order; chunk
// [begin, end) covers packed positions begin..end-1, so chunk boundaries
// depend only on the count and the grain. Debug builds (SE_ASSERTS) check
// that every Read is of a type the ForkSite declares, every Write of a
// declared written type, that writes (and reads of written types) stay
// inside the running chunk's range, and that no structural change happened
// since the view was built.

#ifndef SE_CORE_PARALLEL_VIEW_H_
#define SE_CORE_PARALLEL_VIEW_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/registry.hpp>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include "se_core/assert.h"
#include "se_core/jobs.h"

namespace se {
namespace parallel_view_internal {

// The chunk the calling thread runs through ForChunk, and the view's lead
// storage (copies of a view share it).
inline thread_local const jobs::ChunkRange* t_chunk = nullptr;
inline thread_local const void* t_lead = nullptr;

class ChunkScope {
 public:
  ChunkScope(const jobs::ChunkRange* chunk, const void* lead) noexcept
      : previous_chunk_(t_chunk), previous_lead_(t_lead) {
    t_chunk = chunk;
    t_lead = lead;
  }
  ~ChunkScope() {
    t_chunk = previous_chunk_;
    t_lead = previous_lead_;
  }
  ChunkScope(const ChunkScope&) = delete;
  ChunkScope& operator=(const ChunkScope&) = delete;

 private:
  const jobs::ChunkRange* previous_chunk_;
  const void* previous_lead_;
};

inline bool Declares(std::span<const entt::id_type> ids, entt::id_type id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace parallel_view_internal

template <class... C>
class ParallelView {
  static_assert(sizeof...(C) >= 1, "a ParallelView needs a component");

 public:
  // Captures the storages; call on the owner thread (World::Parallel).
  ParallelView(entt::registry& registry, const jobs::ForkSite& site)
      : storages_(&registry.storage<C>()...),
        site_(&site),
        sizes_{registry.storage<C>().size()...} {}

  // Entities in the first component's storage: the count for ParallelFor.
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(Lead().size());
  }

  // Calls body(entity) for each entity at packed positions [begin, end) of
  // the first storage that has every component of the view.
  template <class F>
  void ForChunk(jobs::ChunkRange range, F&& body) const {
    auto&& visit = std::forward<F>(body);
    CheckUnchanged();
    const entt::sparse_set& lead = Lead();
    SE_ASSERT_MSG(range.begin <= range.end && range.end <= lead.size(),
                  "chunk outside the ParallelView");
    const parallel_view_internal::ChunkScope scope(&range, &lead);
    for (std::uint32_t i = range.begin; i < range.end; ++i) {
      const entt::entity entity = lead.data()[i];
      if ((std::get<entt::storage_for_t<C>*>(storages_)->contains(entity) &&
           ...)) {
        visit(entity);
      }
    }
  }

  template <class T>
  [[nodiscard]] const T& Read(entt::entity entity) const {
    static_assert(!std::is_empty_v<T>, "a tag has no data to read");
    CheckAccess<T>(entity, /*write=*/false);
    return std::get<entt::storage_for_t<T>*>(storages_)->get(entity);
  }

  template <class T>
  [[nodiscard]] T& Write(entt::entity entity) const {
    static_assert(!std::is_empty_v<T>, "a tag has no data to write");
    CheckAccess<T>(entity, /*write=*/true);
    return std::get<entt::storage_for_t<T>*>(storages_)->get(entity);
  }

 private:
  [[nodiscard]] const entt::sparse_set& Lead() const noexcept {
    return *std::get<0>(storages_);
  }

  template <class T>
  void CheckAccess(entt::entity entity, bool write) const {
#if SE_ASSERTS
    namespace internal = parallel_view_internal;
    constexpr entt::id_type kId = entt::type_hash<T>::value();
    const bool writes = internal::Declares(site_->writes, kId);
    const bool reads = writes || internal::Declares(site_->reads, kId);
    if (write) {
      SE_ASSERT_MSG(writes,
                    "ParallelView write of a component the ForkSite does not "
                    "declare in writes");
    } else {
      SE_ASSERT_MSG(reads,
                    "ParallelView read of a component the ForkSite does not "
                    "declare");
    }
    CheckUnchanged();
    if (writes) {
      // Writes, and reads of a written type, stay inside the chunk.
      const entt::sparse_set& lead = Lead();
      SE_ASSERT_MSG(internal::t_chunk != nullptr && internal::t_lead == &lead,
                    "ParallelView access to a written type outside ForChunk");
      SE_ASSERT_MSG(lead.contains(entity), "entity not in the ParallelView");
      const std::size_t index = lead.index(entity);
      SE_ASSERT_MSG(
          index >= internal::t_chunk->begin && index < internal::t_chunk->end,
          "ParallelView access outside the running chunk's range");
    }
#else
    static_cast<void>(entity);
    static_cast<void>(write);
#endif
  }

  void CheckUnchanged() const {
#if SE_ASSERTS
    std::size_t i = 0;
    const bool unchanged =
        ((std::get<entt::storage_for_t<C>*>(storages_)->size() ==
          sizes_[i++]) &&
         ...);
    SE_ASSERT_MSG(unchanged,
                  "a structural change invalidated this ParallelView");
#endif
  }

  std::tuple<entt::storage_for_t<C>*...> storages_;
  [[maybe_unused]] const jobs::ForkSite* site_;
  // Storage sizes at capture, to detect structural changes (debug checks).
  [[maybe_unused]] std::array<std::size_t, sizeof...(C)> sizes_;
};

}  // namespace se

#endif  // SE_CORE_PARALLEL_VIEW_H_
