// The fog of war's look: what the army the picture is for does not see now is darkened. What it
// sees comes as a square grid over the map (1 seen, 0 not; SimFrames makes it from the engine's
// ReconAt); the picture is multiplied, pixel by pixel, by how much of the place under the pixel is
// seen, found from the scene's depth. So everything there darkens alike: ground, water, wrecks.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

class SightShade {
 public:
  explicit SightShade(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // What is seen: `cells` x `cells` values over the whole map, row by row from z = 0 (255 seen,
  // 0 not). An empty grid: nothing is darkened.
  void Set(const std::vector<uint8_t>& grid, int cells);
  bool On() const { return static_cast<bool>(texture_); }
  // Into a pass that has the scene's colour only (the depth is read): darkens what is not seen.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, float mapWidth, float mapHeight);

  float darkness = 0.5f;  // how much of the light an unseen place keeps

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  gfx::Texture texture_;
  std::vector<uint8_t> pixels_;
};

}  // namespace render
