// The computed sky (modern look): the light of a time of day, worked out instead of read from the
// map. From the hour follow where the sun stands, which body's light falls (the sun's by day, the
// moon's by night; around sunrise and sunset neither, and the sky alone lights the ground), that
// light's colour and strength, the sky's colours, and how the picture is exposed. Every scene
// shader gets these with the view (view.h) and lights by them; the sky itself is drawn behind
// everything, with the sun, the moon and stars.
//
// This is a simple model of colours chosen by eye and blended by the sun's height, not a
// calculation of light through an atmosphere. The map's own light (its sun, its sky pictures)
// stays what is used when no time of day is set.
#pragma once
#include <string>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct SkyLight {
  Vec3 body;       // toward the sun (below the horizon at night); the moon stands opposite
  bool moon = false;  // the light that falls is the moon's
  Vec3 direction;  // toward the body whose light falls, held a little above the horizon
  Vec3 color;      // that light, linear; 1 is the sun at noon
  Vec3 zenith, horizon, glow;  // the sky overhead, at the horizon, and the glow around the body; linear
  float night = 0;     // 0 by day .. 1 in deep night (how much the stars show)
  float daylight = 1;  // how bright things are that have no lighting of their own: 1 at noon, little at night
  float exposure = 1;  // what the picture's exposure is multiplied by (the eye opens at night)
};

// The light at that hour (0..24; 6 sunrise, 12 noon, 18 sunset). The sun rises in the east (+x)
// and stands in the south (+z) at noon, 55 degrees high.
SkyLight SkyAt(float hours);
// Puts that light into a view's values in place of the map's: the light's direction and colour,
// the sky's values, the exposure.
void SetSky(const SkyLight& sky, ViewConstants* constants);

class Sky {
 public:
  explicit Sky(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // The sky behind everything, into the scene's pass right after it was cleared. Only for a view
  // whose sky is computed (SetSky).
  void Draw(const View& view, const Camera& camera);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
};

}  // namespace render
