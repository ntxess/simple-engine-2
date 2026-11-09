// Planted for checks.planted.gpu_texture_member: an SDL struct that holds
// a texture pointer, kept as a member (ADR-069).

#include <SDL3/SDL_gpu.h>

struct SavedPass {
  SDL_GPUColorTargetInfo target{};
  int frame = 0;
};
