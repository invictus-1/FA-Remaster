// The water surface of a map: one sheet at the water's elevation, coloured the way the original game's
// high-quality water is, as far as this stage can (see water.cpp for what is still missing).
#pragma once
#include <string>

#include "camera.h"
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
  // The modern look's water (water.cpp): into a pass that has the scene's colour ONLY, with the
  // picture as it was before the water (`behind`, one sample a pixel) and the scene's depth to read.
  // Without several samples a pixel it draws as Draw does.
  void DrawModern(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth);

 private:
  gfx::Device* device_;
  TextureCache* textures_;
  const MapData* map_ = nullptr;
  void DrawWith(const View& view, const Camera* camera, gfx::Texture behind, gfx::Texture sceneDepth);
  gfx::Pipeline pipeline_, modern_;
  gfx::Buffer vertices_;
  gfx::Texture waterMap_, waves_[4], sky_;
};

}  // namespace render
