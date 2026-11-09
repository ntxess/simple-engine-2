// Planted for checks.planted.gpu_texture_member: a typedef of a texture
// pointer, aliased again, as a class member (ADR-069).

#include <SDL3/SDL_gpu.h>

typedef SDL_GPUTexture* RawTexture;

namespace game {

using CachedTexture = RawTexture;

class Cache {
 public:
  CachedTexture Last() const { return last_; }

 private:
  CachedTexture last_;
};

}  // namespace game
