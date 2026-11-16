// Planted for checks.planted.imtexture_member: ImTextureIDs in a container
// member, through an alias.

#include <vector>

#include "imgui.h"

using PanelTexture = ImTextureID;

class Thumbnails {
 private:
  std::vector<PanelTexture> ids_;
};
