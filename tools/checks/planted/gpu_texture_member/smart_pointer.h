// Planted for checks.planted.gpu_texture_member: a smart pointer owning a
// texture as a member (ADR-069).

#include <SDL3/SDL_gpu.h>

#include <memory>

struct TextureDeleter {
  void operator()(SDL_GPUTexture* texture) const;
};

struct Holder2 {
  std::unique_ptr<SDL_GPUTexture, TextureDeleter> texture;
};
