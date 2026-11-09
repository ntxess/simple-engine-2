// Planted for checks.planted.gpu_thread: an se_render wrapper that calls
// SDL_GPU but asserts only the engine's main thread, not SDL's (ADR-069).

#include <SDL3/SDL_gpu.h>

#include "se_core/thread_role.h"

namespace se::present {

bool WaitForSwapchain(SDL_GPUDevice* device, SDL_Window* window) {
  SE_ASSERT_MAIN_THREAD();
  return SDL_WaitForGPUSwapchain(device, window);
}

}  // namespace se::present
