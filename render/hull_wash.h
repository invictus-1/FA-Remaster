// [hulls] Where the sea meets a ship's hull (World::oceanLift; Chris, 2026-10-23: a frigate in a storm looked "floating on the
// water vs in the water since the waves aren't hitting the ship and its above the water a bit vs a tad in it"). With the
// ships sunk a little into the lifted sea (World::RideWaves) and the water's own part (water.cpp: the white wash round the
// waterline, the hull under the surface hidden by the sea), this draws what the waves leave on the hull's side above the
// water: a darker wet band whose top is ragged and rises and falls with the waves along the hull, and white water riding
// on the hull at the line the sea meets it, the higher the water stands against the hull just now (a short wave running
// along it), the rougher the sea, at the bow the faster it goes.
//
// How: after the water, a box round each hull's waterline (Ocean::AddHull's list, the nearest kHulls) is drawn from behind
// (its back faces, no depth test: each pixel it covers once); for each pixel it reads the scene's depth there (what stands
// in the picture: the water writes none), and where that place stands in the box, well above the sea bed, and above the
// lifted surface there (the ocean's own field, where the surface over that place stands just now) by less than the band, it
// darkens and whitens what is drawn there. Purely for the eye; nothing without the lifted ocean and its hulls.
#pragma once
#include <string>

#include "camera.h"
#include "gfx.h"
#include "ocean.h"
#include "view.h"

namespace render {

class HullWash {
 public:
  explicit HullWash(gfx::Device* device) : device_(device) {}
  // Draws it over the scene (after the water: in its colour-only pass, the lights and shadow maps bound as for the water),
  // when the ocean lifts the sea and has hulls this frame. `sceneDepth` the scene's (several samples a pixel, or one);
  // `heights` the ground's (Terrain::HeightTexture) and its height unit; the map's size and the water's level.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, Ocean& ocean, gfx::Texture heights, float heightUnit, float mapWidth,
            float mapHeight, float level);
  int Drawn() const { return drawn_; }  // (hulls drawn the last frame)

 private:
  bool Ready();
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  gfx::Buffer cube_;
  bool tried_ = false;
  int drawn_ = 0;
};

}  // namespace render
