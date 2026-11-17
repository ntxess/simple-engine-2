// The allowed ImGui forms for checks.planted.gpu_texture_member_allowed:
// an ImTextureID asked for every frame, never stored (M10a, ADR-095).

#include "imgui.h"

class TextureSource {
 public:
  virtual ImTextureID Id(int handle) = 0;
  ImVec2 Size(int handle) const;
  void Show(ImTextureID id, ImVec2 size);

 private:
  int handle_ = 0;
  ImVec2 size_{};
};
