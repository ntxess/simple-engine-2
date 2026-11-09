// Planted for checks.planted.gpu_texture_member: a texture pointer kept
// through a using alias (ADR-069).

#include <SDL3/SDL_gpu.h>

using TexturePtr = SDL_GPUTexture*;

struct Holder {
  TexturePtr texture = nullptr;
};
