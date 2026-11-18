// Copyright 2026 The simple-engine-2 Authors.
//
// The renderer's shaders (docs/milestone-specs.md M7, "Shaders"; ADR-059).
// HLSL sources live in engine/se_render/shaders/. The SPIR-V blobs are
// committed headers that tools/shaders/build_shaders.py generates with
// glslang (the WSL freshness test regenerates and compares them); the DXIL
// blobs are compiled at build time with the Windows SDK's dxc.exe, so only
// Windows builds have them (kHasDxil).
//
// CreateShader picks the format the device reports and declares the
// resource counts of ShaderInfo, which tests/render_gpu reflects from each
// blob (SPIR-V decorations, the DXIL container's PSV0 resource table).

#ifndef SE_RENDER_SHADERS_H_
#define SE_RENDER_SHADERS_H_

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se {

#if defined(SE_RENDER_HAS_DXIL) && SE_RENDER_HAS_DXIL
inline constexpr bool kHasDxil = true;
#else
inline constexpr bool kHasDxil = false;
#endif

enum class ShaderId : std::uint8_t {
  kSpriteVert,
  kSpriteFrag,
  kBlitVert,
  kBlitFrag,
};

struct ShaderInfo {
  ShaderId id = ShaderId::kSpriteVert;
  // The source's base name, "sprite.vert".
  std::string_view name;
  SDL_GPUShaderStage stage = SDL_GPU_SHADERSTAGE_VERTEX;
  // What SDL_CreateGPUShader is told; must match the blobs.
  std::uint32_t num_samplers = 0;
  std::uint32_t num_storage_textures = 0;
  std::uint32_t num_storage_buffers = 0;
  std::uint32_t num_uniform_buffers = 0;
  std::span<const std::uint8_t> spirv;
  // Empty without kHasDxil.
  std::span<const std::uint8_t> dxil;
};

// Every shader, in ShaderId order.
[[nodiscard]] std::span<const ShaderInfo> AllShaders() noexcept;
[[nodiscard]] const ShaderInfo& GetShader(ShaderId id) noexcept;

// Creates |id| for |device| in the first format the device supports, DXIL
// before SPIR-V. Main thread only (SDL_GPU does no thread checks).
[[nodiscard]] std::expected<SDL_GPUShader*, Error> CreateShader(
    SDL_GPUDevice* device, ShaderId id) SE_REQUIRES(g_main);

}  // namespace se

#endif  // SE_RENDER_SHADERS_H_
