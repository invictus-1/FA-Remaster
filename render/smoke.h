// Smoke's shadow (the modern look's lit smoke, World::litSmoke and smokeShadows): the smoke and dust drawn as the
// sun sees them into a small picture round what the camera looks at, so that a column of smoke darkens the ground,
// the units and the water under it, the light in the air (air.h) has its shafts cut by it, and the smoke itself is
// darker deep inside a plume than on its sunny side.
//
// What a texel holds is not a depth, as in the sun's shadow maps, but how much smoke the line toward the sun through
// it crosses: its optical depth (each particle's -ln(1 - alpha), added up), and that weighted by the smoke's height
// along the line and by the height squared. From those a place reads the smoke's mean height on its line and how far
// it spreads about it, and takes as its shadow the share of the smoke that lies higher than itself, as a normal
// distribution has it (lighting.h, SmokeShadowAt). Heights are counted from the camera's target and over the sun's
// height above the horizon, so that the numbers stay small (the picture is half floats) wherever the line runs.
// After the "opacity shadow maps" family of techniques (Kim and Neumann 2001; moments as in variance shadow maps),
// written here from the idea.
#pragma once
#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

class SmokeShadowMap {
 public:
  explicit SmokeShadowMap(gfx::Device* device) : device_(device) {}

  // Places the picture for this frame's camera and the light that falls (`sunDirection`, toward it) and writes
  // where it is into `view` (smokeMatrix, smokeShadow: `surfaces` says whether what the sun lights takes it, besides
  // the smoke). False, and nothing written, when the light is too low over the horizon to cast a useful shadow.
  bool Place(const Camera& camera, Vec3 sunDirection, View* view, bool surfaces);
  // The view the smoke is drawn with into it (the sun's projection; the particles face the sun).
  const View& SunView() const { return sunView_; }
  // The pass to draw the smoke in (the picture cleared to no smoke), and the picture.
  gfx::PassDesc Pass();
  gfx::Texture Map() const { return map_; }

  int size = 512;           // texels across
  float strength = 0.75f;   // what the smoke's optical depth is multiplied by (the game's smoke is drawn for its look)
  float leastSpread = 1.5f; // world units: smoke on one line is never taken as thinner along it than this

 private:
  gfx::Device* device_;
  gfx::Texture map_;
  int made_ = 0;
  View sunView_;
};

}  // namespace render
