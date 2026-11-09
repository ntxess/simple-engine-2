// Copyright 2026 The simple-engine-2 Authors.
//
// The semantic projection of a rampage World, in the exact .seproj format of
// the legacy reference (core/util/SemanticProjection.cpp), so the two trees'
// projections compare as text (tools/refcmp/refcmp.py). Per tick:
//
//   t <tick>
//   <serial> <x> <y> <rotation> <HP> [<stat>=<value> ...]
//
// one line per entity in spawn-serial order: the Transform position, the
// rotation in degrees normalised to [0, 360), HP, then the other stats
// sorted by name; values printed with %.9g, "-" when absent. Whitespace and
// '=' in a stat name become '_'. The tick hash is FNV-1a 64 of the block; a
// .seproj file stores it on the tick line.

#ifndef GAME_RAMPAGE_PROJECTION_H_
#define GAME_RAMPAGE_PROJECTION_H_

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

#include "se_core/world.h"

namespace se::rampage {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

[[nodiscard]] std::uint64_t Fnv1a64(std::uint64_t hash,
                                    std::string_view bytes) noexcept;

// Appends the block of the World's current tick (the hashed form, with the
// bare "t <tick>" line).
void WriteProjection(const World& world, std::string& out);

// Writes a .seproj file: a header, each tick's block with its hash on the
// tick line, and "# run <ticks> <hash>" at the end.
class ProjectionFile {
 public:
  ProjectionFile() = default;
  // Closes the file without the "# run" trailer; call Close() for it.
  ~ProjectionFile() = default;
  ProjectionFile(const ProjectionFile&) = delete;
  ProjectionFile& operator=(const ProjectionFile&) = delete;

  // Without a file only the hashes are computed.
  bool Open(const std::string& path, std::uint32_t rate);
  void Write(const World& world);
  void Close();

  [[nodiscard]] std::uint64_t TickHash() const noexcept { return tick_hash_; }
  [[nodiscard]] std::uint64_t RunHash() const noexcept { return run_hash_; }
  [[nodiscard]] std::uint64_t Ticks() const noexcept { return ticks_; }

 private:
  std::ofstream file_;
  std::string block_;
  std::uint64_t tick_hash_ = kFnvOffsetBasis;
  std::uint64_t run_hash_ = kFnvOffsetBasis;
  std::uint64_t ticks_ = 0;
};

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_PROJECTION_H_
