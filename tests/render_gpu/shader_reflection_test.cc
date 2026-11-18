// Copyright 2026 The simple-engine-2 Authors.
//
// The resource counts se_render declares to SDL_CreateGPUShader must match
// the blobs (docs/milestone-specs.md M7, "Shaders"). This test reflects
// each blob: SPIR-V by its decorations (SDL's sets: vertex samplers in set
// 0 and uniforms in set 1, fragment samplers in set 2 and uniforms in set
// 3), DXIL by the PSV0 part's resource table (spaces 0/1 and 2/3 likewise).
// A planted mismatch proves the comparison fails.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "se_render/shaders.h"

namespace se {
namespace {

struct Counts {
  std::uint32_t samplers = 0;
  std::uint32_t storage_textures = 0;
  std::uint32_t storage_buffers = 0;
  std::uint32_t uniform_buffers = 0;
  // Resources outside the sets (spaces) SDL expects for the stage.
  std::uint32_t misplaced = 0;
  bool parsed = false;
};

std::uint32_t Word(std::span<const std::uint8_t> bytes, std::size_t index) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + index * 4, 4);
  return value;
}

// SPIR-V: variables in UniformConstant (sampled images, storage images)
// and Uniform/StorageBuffer storage, with their DescriptorSet decorations.
Counts ReflectSpirv(std::span<const std::uint8_t> blob, bool vertex) {
  constexpr std::uint32_t kMagic = 0x07230203;
  constexpr std::uint32_t kOpDecorate = 71;
  constexpr std::uint32_t kOpTypeImage = 25;
  constexpr std::uint32_t kOpTypeSampledImage = 27;
  constexpr std::uint32_t kOpTypePointer = 32;
  constexpr std::uint32_t kOpVariable = 59;
  constexpr std::uint32_t kDecorationDescriptorSet = 34;
  constexpr std::uint32_t kStorageUniformConstant = 0;
  constexpr std::uint32_t kStorageUniform = 2;
  constexpr std::uint32_t kStorageStorageBuffer = 12;
  Counts counts;
  if (blob.size() < 20 || blob.size() % 4 != 0 || Word(blob, 0) != kMagic) {
    return counts;
  }
  const std::size_t words = blob.size() / 4;
  std::map<std::uint32_t, std::uint32_t> sets;
  std::map<std::uint32_t, std::uint32_t> pointee;
  std::map<std::uint32_t, std::uint32_t> opcode_of;
  std::map<std::uint32_t, std::uint32_t> image_sampled;
  struct Variable {
    std::uint32_t id;
    std::uint32_t type;
    std::uint32_t storage;
  };
  std::vector<Variable> variables;
  for (std::size_t i = 5; i < words;) {
    const std::uint32_t first = Word(blob, i);
    const std::uint32_t count = first >> 16;
    const std::uint32_t op = first & 0xFFFF;
    if (count == 0 || i + count > words) return counts;
    if (op == kOpDecorate && count >= 4 &&
        Word(blob, i + 2) == kDecorationDescriptorSet) {
      sets[Word(blob, i + 1)] = Word(blob, i + 3);
    } else if (op == kOpTypePointer && count >= 4) {
      pointee[Word(blob, i + 1)] = Word(blob, i + 3);
    } else if ((op == kOpTypeImage || op == kOpTypeSampledImage) &&
               count >= 2) {
      opcode_of[Word(blob, i + 1)] = op;
      // OpTypeImage's Sampled operand: 1 sampled, 2 storage.
      if (op == kOpTypeImage && count >= 8) {
        image_sampled[Word(blob, i + 1)] = Word(blob, i + 7);
      }
    } else if (op == kOpVariable && count >= 4) {
      variables.push_back(
          {Word(blob, i + 2), Word(blob, i + 1), Word(blob, i + 3)});
    }
    i += count;
  }
  const std::uint32_t texture_set = vertex ? 0 : 2;
  const std::uint32_t uniform_set = vertex ? 1 : 3;
  for (const Variable& variable : variables) {
    const auto set = sets.find(variable.id);
    if (set == sets.end()) continue;
    const std::uint32_t type = pointee[variable.type];
    if (variable.storage == kStorageUniformConstant) {
      if (opcode_of[type] == kOpTypeSampledImage) {
        ++counts.samplers;
      } else if (opcode_of[type] == kOpTypeImage && image_sampled[type] == 2) {
        ++counts.storage_textures;
      }
      if (set->second != texture_set) ++counts.misplaced;
    } else if (variable.storage == kStorageUniform) {
      ++counts.uniform_buffers;
      if (set->second != uniform_set) ++counts.misplaced;
    } else if (variable.storage == kStorageStorageBuffer) {
      ++counts.storage_buffers;
      if (set->second != texture_set) ++counts.misplaced;
    }
  }
  counts.parsed = true;
  return counts;
}

// DXIL: the container's PSV0 part lists every resource binding as
// {type, space, lower bound, upper bound, ...}.
Counts ReflectDxil(std::span<const std::uint8_t> blob, bool vertex) {
  Counts counts;
  if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4) != 0) {
    return counts;
  }
  std::uint32_t parts = 0;
  std::memcpy(&parts, blob.data() + 28, 4);
  for (std::uint32_t p = 0; p < parts; ++p) {
    std::uint32_t offset = 0;
    std::memcpy(&offset, blob.data() + 32 + std::size_t{p} * 4, 4);
    if (offset + 8 > blob.size()) return counts;
    if (std::memcmp(blob.data() + offset, "PSV0", 4) != 0) continue;
    std::size_t at = offset + 8;
    std::uint32_t info_size = 0;
    std::memcpy(&info_size, blob.data() + at, 4);
    at += 4 + info_size;
    std::uint32_t resources = 0;
    std::memcpy(&resources, blob.data() + at, 4);
    at += 4;
    std::uint32_t bind_size = 16;
    if (resources > 0) {
      std::memcpy(&bind_size, blob.data() + at, 4);
      at += 4;
    }
    // PSVResourceType: 1 sampler, 2 CBV, 3 typed SRV, 4 raw SRV, 5
    // structured SRV, 6-9 UAVs.
    std::uint32_t textures = 0;
    std::uint32_t sampler_states = 0;
    const std::uint32_t texture_space = vertex ? 0 : 2;
    const std::uint32_t uniform_space = vertex ? 1 : 3;
    for (std::uint32_t r = 0; r < resources; ++r) {
      if (at + 16 > blob.size()) return counts;
      std::uint32_t fields[4] = {};
      std::memcpy(fields, blob.data() + at, 16);
      at += bind_size;
      const std::uint32_t type = fields[0];
      const std::uint32_t space = fields[1];
      const std::uint32_t bindings = fields[3] - fields[2] + 1;
      if (type == 1) {
        sampler_states += bindings;
        if (space != texture_space) ++counts.misplaced;
      } else if (type == 2) {
        counts.uniform_buffers += bindings;
        if (space != uniform_space) ++counts.misplaced;
      } else if (type == 3) {
        textures += bindings;
        if (space != texture_space) ++counts.misplaced;
      } else if (type == 4 || type == 5) {
        counts.storage_buffers += bindings;
      } else if (type >= 6) {
        ++counts.misplaced;
      }
    }
    // SDL pairs texture t[n] with sampler s[n]: a count is a pair.
    counts.samplers = textures;
    if (sampler_states != textures) ++counts.misplaced;
    counts.parsed = true;
    return counts;
  }
  return counts;
}

std::string Compare(const ShaderInfo& info, const Counts& counts) {
  if (!counts.parsed) return std::string(info.name) + ": not parsed";
  std::string problems;
  if (counts.samplers != info.num_samplers) problems += " samplers";
  if (counts.storage_textures != info.num_storage_textures) {
    problems += " storage textures";
  }
  if (counts.storage_buffers != info.num_storage_buffers) {
    problems += " storage buffers";
  }
  if (counts.uniform_buffers != info.num_uniform_buffers) {
    problems += " uniform buffers";
  }
  if (counts.misplaced != 0) problems += " sets/spaces";
  return problems.empty() ? std::string()
                          : std::string(info.name) + ":" + problems;
}

TEST_CASE("Every SPIR-V blob declares the counts SDL is given", "[shaders]") {
  REQUIRE(AllShaders().size() == 4);
  for (const ShaderInfo& info : AllShaders()) {
    INFO(info.name);
    const bool vertex = info.stage == SDL_GPU_SHADERSTAGE_VERTEX;
    CHECK(Compare(info, ReflectSpirv(info.spirv, vertex)).empty());
  }
}

TEST_CASE("Every DXIL blob declares the counts SDL is given", "[shaders]") {
  if (!kHasDxil) SKIP("DXIL is compiled on Windows only");
  for (const ShaderInfo& info : AllShaders()) {
    INFO(info.name);
    const bool vertex = info.stage == SDL_GPU_SHADERSTAGE_VERTEX;
    REQUIRE_FALSE(info.dxil.empty());
    CHECK(Compare(info, ReflectDxil(info.dxil, vertex)).empty());
  }
}

TEST_CASE("Planted: a declared count that differs from the blob is caught",
          "[shaders]") {
  ShaderInfo wrong = GetShader(ShaderId::kSpriteFrag);
  wrong.num_samplers += 1;
  CHECK_FALSE(Compare(wrong, ReflectSpirv(wrong.spirv, false)).empty());
  ShaderInfo no_uniform = GetShader(ShaderId::kSpriteVert);
  no_uniform.num_uniform_buffers = 0;
  CHECK_FALSE(
      Compare(no_uniform, ReflectSpirv(no_uniform.spirv, true)).empty());
  if (kHasDxil) {
    CHECK_FALSE(Compare(wrong, ReflectDxil(wrong.dxil, false)).empty());
  }
}

}  // namespace
}  // namespace se
