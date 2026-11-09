// Planted for checks.planted.gpu_texture_member: a struct outside se_render
// keeps a resolved texture across frames.

#include <SDL3/SDL_gpu.h>

struct CachedSprite {
  int frame = 0;
  SDL_GPUTexture* texture = nullptr;
};
