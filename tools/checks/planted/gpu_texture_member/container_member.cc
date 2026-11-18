// Planted for checks.planted.gpu_texture_member: a container of resolved
// textures as a member.

#include <SDL3/SDL_gpu.h>

#include <vector>

namespace game {

struct Atlas {
  std::vector<SDL_GPUTexture*> pages;
};

}  // namespace game
