// The allowed forms for checks.planted.gpu_texture_member_allowed: none of
// these stores a texture in a class (ADR-069).

#include <SDL3/SDL_gpu.h>

#include <functional>
#include <vector>

typedef void (*TextureVisitor)(SDL_GPUTexture* texture);
using TextureCallback = void (*)(SDL_GPUTexture*);
using TextureFormat = SDL_GPUTextureFormat;

class TextureUser {
 public:
  SDL_GPUTexture* Resolve(int handle) const;
  [[nodiscard]] SDL_GPUTexture* Current() const SE_REQUIRES(g_main);
  static SDL_GPUTexture* Fallback() noexcept;
  void Draw(SDL_GPUTexture* texture, SDL_GPUColorTargetInfo target);
  virtual SDL_GPUTexture* Target() = 0;
  bool operator==(const TextureUser& other) const;
  explicit operator bool() const;
  int Width() const {
    SDL_GPUTexture* local = Resolve(0);
    return local != nullptr ? 1 : 0;
  }

 private:
  TextureVisitor visitor_ = nullptr;
  TextureCallback callback_ = nullptr;
  void (*draw_)(SDL_GPUTexture*) = nullptr;
  std::function<void(SDL_GPUTexture*)> on_texture_;
  TextureFormat format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTextureFormat other_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTextureCreateInfo info_{};
  std::vector<int> handles_;
};
