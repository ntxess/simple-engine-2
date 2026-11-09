// Planted for checks.planted.gpu_texture_member: members initialized by a
// call, and one with a thread-safety annotation (ADR-069).

#include <SDL3/SDL_gpu.h>

SDL_GPUTexture* MakeTexture();

class Preview {
 private:
  SDL_GPUTexture* made_ = MakeTexture();
  SDL_GPUTexture* braced_{MakeTexture()};
  SDL_GPUTexture* guarded_ SE_GUARDED_BY(mutex_) = nullptr;
};
