// The water surface of a map: one sheet at the water's elevation, coloured the way the original game's
// high-quality water is, as far as this stage can (see water.cpp for what is still missing).
#pragma once
#include <string>

#include "gfx.h"
#include "scmap.h"
#include "textures.h"
#include "view.h"

namespace render {

class Water {
 public:
  Water(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}

  // `map` must stay alive while the water is used. Maps without water load fine and draw nothing.
  bool Load(const MapData* map, std::string* error);
  // Call after the ground and everything else under the surface has been drawn.
  void Draw(const View& view);

 private:
  gfx::Device* device_;
  TextureCache* textures_;
  const MapData* map_ = nullptr;
  gfx::Pipeline pipeline_;
  gfx::Buffer vertices_;
  gfx::Texture waterMap_, waves_[4], sky_;
};

}  // namespace render
