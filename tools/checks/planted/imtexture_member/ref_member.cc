// Planted for checks.planted.imtexture_member: an ImTextureRef (which
// carries an ImTextureID) kept as a struct member.

#include "imgui.h"

struct ImageSlot {
  ImTextureRef texture;
  ImVec2 size;
};
