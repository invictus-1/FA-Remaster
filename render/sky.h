// The computed sky (modern look): the light of a time of day, worked out instead of read from the
// map. From the hour follow where the sun and the moon stand, which of the two lights the ground
// (the sun by day, the moon by night; around sunrise and sunset the sky alone), that light's
// colour, the sky's light, and how the picture is exposed. Every scene shader gets these with the
// view (view.h) and lights by them; the sky itself is drawn behind everything.
//
// The sky's light is sunlight (or moonlight) scattered once on its way through an atmosphere like
// the Earth's: by the air's molecules (which scatter blue far more than red: the blue of day, the
// red of a low sun), by haze (which scatters forward: the glow around the sun), and thinned by the
// ozone layer (the deep blue of dusk). The picture's sky works that out for every pixel. What lights
// the scene is a simpler form of it, three colours fitted to the worked-out sky (ComputedSky in
// view.h), so that a surface need not march through the air. By night there are stars, more and
// fainter ones along a band, and a moon lit by the sun from the side it stands on; the stars turn
// with the hours. The stars are made by a formula: they are no real sky's.
//
// The map's own light (its sun, its sky pictures) stays what is used when no time of day is set.
#pragma once
#include <string>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

struct SkyLight {
  Vec3 body;          // toward the sun (below the horizon at night)
  Vec3 moonPlace;     // toward the moon
  bool moon = false;  // the light that falls is the moon's
  Vec3 direction;     // toward the body whose light falls, held a little above the horizon
  Vec3 color;         // that light on the ground, linear; about 1 is the sun at noon
  Vec3 zenith, horizon, glow;  // the fitted sky: overhead, at the horizon, around the body; linear
  float night = 0;     // 0 by day .. 1 in deep night (how much the stars show)
  float daylight = 1;  // light on level ground against noon (for what has no lighting of its own)
  float exposure = 1;  // what the picture's exposure is multiplied by (the eye opens at night)
  // for the picture of the sky
  Vec3 sunSky, moonSky;  // how strong each body's light is in the air
  Vec3 sunDisc;          // the sun's disc as seen through the air
  float starTurn = 0;    // how far the stars have turned, radians
  float faint = 0;       // how much of the night's own faint light there is (0 by day, 1 from dusk to dawn)
  float cover = 0;       // how much of the sky the clouds cover, 0..1
  Vec3 cloudLight;       // the falling light on the clouds
};

// The light at that hour (0..24; 6 sunrise, 12 noon, 18 sunset). The sun rises in the east (+x)
// and stands in the south (+z) at noon, 55 degrees high. The moon follows 160 degrees behind it: it
// rises some time after sunset, nearly full. `cloudCover`: how much of the sky a layer of clouds
// covers (0 none, 1 all): they drift, are lit by the falling light, throw shadows on the ground,
// and the more there are the greyer the sky's light is.
SkyLight SkyAt(float hours, float cloudCover = 0);
// Puts that light into a view's values in place of the map's: the light's direction and colour,
// the sky's values, the exposure.
void SetSky(const SkyLight& sky, ViewConstants* constants);

class Sky {
 public:
  explicit Sky(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // The sky where nothing was drawn, into the scene's pass after everything solid: the computed
  // one for a view whose sky is computed (SetSky was called with `sky`), and else the map's own sky
  // picture `mapSky` (a cube map; none: nothing is drawn, as before).
  void Draw(const View& view, const Camera& camera, const SkyLight& sky, gfx::Texture mapSky = {});

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
};

}  // namespace render
