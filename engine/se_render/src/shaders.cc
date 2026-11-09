// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/shaders.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "se_core/assert.h"
#include "se_core/error.h"
#include "shaders/generated/blit.frag.spv.h"
#include "shaders/generated/blit.vert.spv.h"
#include "shaders/generated/sprite.frag.spv.h"
#include "shaders/generated/sprite.vert.spv.h"
#include "src/gpu_assert.h"

#if defined(SE_RENDER_HAS_DXIL) && SE_RENDER_HAS_DXIL
// Generated at build time into the build tree (se_render's CMakeLists.txt).
#include "shaders/blit.frag.dxil.h"
#include "shaders/blit.vert.dxil.h"
#include "shaders/sprite.frag.dxil.h"
#include "shaders/sprite.vert.dxil.h"
#endif

namespace se {
namespace {

template <std::size_t N>
constexpr std::span<const std::uint8_t> Blob(const std::uint8_t (&bytes)[N]) {
  return std::span<const std::uint8_t>(bytes, N);
}

#if defined(SE_RENDER_HAS_DXIL) && SE_RENDER_HAS_DXIL
#define SE_DXIL_BLOB(name) Blob(shaders::name)
#else
#define SE_DXIL_BLOB(name) std::span<const std::uint8_t>()
#endif

// The resource counts follow the HLSL: the sprite vertex shader reads one
// uniform block (b0, space1); both fragment shaders sample one texture
// (t0/s0, space2).
const std::array<ShaderInfo, 4> kShaders = {{
    {.id = ShaderId::kSpriteVert,
     .name = "sprite.vert",
     .stage = SDL_GPU_SHADERSTAGE_VERTEX,
     .num_samplers = 0,
     .num_storage_textures = 0,
     .num_storage_buffers = 0,
     .num_uniform_buffers = 1,
     .spirv = Blob(shaders::kSpriteVertSpirv),
     .dxil = SE_DXIL_BLOB(kSpriteVertDxil)},
    {.id = ShaderId::kSpriteFrag,
     .name = "sprite.frag",
     .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
     .num_samplers = 1,
     .num_storage_textures = 0,
     .num_storage_buffers = 0,
     .num_uniform_buffers = 0,
     .spirv = Blob(shaders::kSpriteFragSpirv),
     .dxil = SE_DXIL_BLOB(kSpriteFragDxil)},
    {.id = ShaderId::kBlitVert,
     .name = "blit.vert",
     .stage = SDL_GPU_SHADERSTAGE_VERTEX,
     .num_samplers = 0,
     .num_storage_textures = 0,
     .num_storage_buffers = 0,
     .num_uniform_buffers = 0,
     .spirv = Blob(shaders::kBlitVertSpirv),
     .dxil = SE_DXIL_BLOB(kBlitVertDxil)},
    {.id = ShaderId::kBlitFrag,
     .name = "blit.frag",
     .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
     .num_samplers = 1,
     .num_storage_textures = 0,
     .num_storage_buffers = 0,
     .num_uniform_buffers = 0,
     .spirv = Blob(shaders::kBlitFragSpirv),
     .dxil = SE_DXIL_BLOB(kBlitFragDxil)},
}};

#undef SE_DXIL_BLOB

}  // namespace

std::span<const ShaderInfo> AllShaders() noexcept { return kShaders; }

const ShaderInfo& GetShader(ShaderId id) noexcept {
  return kShaders[static_cast<std::size_t>(id)];
}

std::expected<SDL_GPUShader*, Error> CreateShader(SDL_GPUDevice* device,
                                                  ShaderId id) {
  SE_ASSERT_GPU_THREAD();
  const ShaderInfo& info = GetShader(id);
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  SDL_GPUShaderCreateInfo create{};
  create.entrypoint = "main";
  create.stage = info.stage;
  create.num_samplers = info.num_samplers;
  create.num_storage_textures = info.num_storage_textures;
  create.num_storage_buffers = info.num_storage_buffers;
  create.num_uniform_buffers = info.num_uniform_buffers;
  if (!info.dxil.empty() && (formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
    create.format = SDL_GPU_SHADERFORMAT_DXIL;
    create.code = info.dxil.data();
    create.code_size = info.dxil.size();
  } else if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
    create.format = SDL_GPU_SHADERFORMAT_SPIRV;
    create.code = info.spirv.data();
    create.code_size = info.spirv.size();
  } else {
    return Fail(ErrorCode::kUnsupported,
                std::string("no shader format for ") + std::string(info.name) +
                    ": the device takes neither DXIL (in this build) nor "
                    "SPIR-V");
  }
  SDL_GPUShader* shader = SDL_CreateGPUShader(device, &create);
  if (shader == nullptr) {
    return Fail(ErrorCode::kUnsupported, "SDL_CreateGPUShader(" +
                                             std::string(info.name) +
                                             "): " + SDL_GetError());
  }
  return shader;
}

}  // namespace se
