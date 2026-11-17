// Planted for checks.planted.imtexture_member: an ImTextureID kept by a
// panel across frames (M10a, ADR-095).

#include "imgui.h"

class CachedImagePanel {
 private:
  ImTextureID cached_ = ImTextureID_Invalid;
};
