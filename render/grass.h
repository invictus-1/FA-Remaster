// Ground cover near the camera (World::groundCover; the modern look): grass blades made by the shader alone, no
// art and nothing kept on the processor. A square of cells round what the camera looks at, each cell a blade or
// none; a cell's place, the blade's lean, height and hue follow from the cell's whole-number place in the world, so
// the pattern stays with the ground as the camera moves. How grassy the ground is and its colour come from
// Terrain::GroundColours (rgb the colour, a how grassy, by the layers' colours); the blade is coloured from the
// ground under it, a share of them drier toward the tip, and sways in the wind the trees sway in. Low (no taller
// than half a small unit's hull, for readability), thinner and gone with the distance; nothing when the camera is
// far, where a blade would be less than a pixel.
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

class GroundCover {
 public:
  explicit GroundCover(gfx::Device* device) : device_(device) {}
  ~GroundCover();
  bool Init(std::string* error);
  // Before the scene's pass (it dispatches): where the grass lies this frame, round what `camera` looks at, and
  // what presses it flat there: `discs` x, z, radius each (world units), what stands on the ground. Nothing when
  // the camera is far.
  void Prepare(const View& view, const Camera& camera, const std::vector<float>& discs);
  // Into the scene's pass, after the ground. `heights`: the ground's heights (Terrain::HeightTexture), `heightUnit`
  // world units of its largest value; `cover`: Terrain::GroundColours; `normals`: Terrain::NormalMap; the map's
  // size; `waterLevel`: none at or under it (a map without water: very low).
  void Draw(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover, gfx::Texture normals,
            float mapWidth, float mapHeight, float waterLevel);

  float bladeHeight = 0.22f;  // the tallest blade, world units
  float bladeWidth = 0.025f;  // at its foot
  float cell = 0.15f;         // one tuft of three blades (or none) in a cell this wide
  float farthest = 50;        // no grass at all when the camera is farther from what it looks at (a blade is then
                              // less than a pixel)

  int drawnBlades = 0;  // (the last Draw: cells gone through)

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_, trample_;
  gfx::Texture trampled_;  // (how flat the grass is pressed over its square: 1 flat)
  float area_[4] = {};     // (Prepare's square: its first cell's x, z; a cell's size; cells a side)
  bool prepared_ = false;
};

}  // namespace render
