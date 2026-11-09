// Planted for checks.planted.gpu_texture_member: an editor panel caching
// the texture it showed (the legacy editor's pattern, red team C2).

#include <SDL3/SDL_gpu.h>

class PreviewPanel {
 public:
  SDL_GPUTexture* Preview() const { return preview_; }
  void SetPreview(SDL_GPUTexture* texture) { preview_ = texture; }

 private:
  SDL_GPUTexture *preview_{nullptr};
};
