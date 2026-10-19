// Ambient occlusion from the picture's own depth: creases, the ground under a unit and the gaps
// between parts get darker, as they would where the sky's light cannot reach.
//
// Two steps. Compute reads the scene's depth after everything solid is drawn and writes, per pixel,
// how open the surroundings are. Apply multiplies the scene's picture by that, before water and
// anything else see-through is drawn on top. It is a correction made on the finished lighting (the
// sun's direct light is dimmed with the rest), which is the usual trade for doing it without a
// separate depth pass.
#pragma once
#include <string>

#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

class AmbientOcclusion {
 public:
  explicit AmbientOcclusion(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);

  // Begins its own pass: call between the scene's solid drawing and Post::ContinueScene.
  void Compute(const View& view, const Camera& camera, gfx::Texture sceneDepth);
  // Draws into the current (scene) pass.
  void Apply(const View& view);

  float radius = 0.9f;    // world units: how far a surface looks for things that shade it
  float strength = 1.0f;  // 0: no effect
  // Contact shadows (the lighting upgrade; World::contactShadows): with the occlusion, where a short march
  // toward the sun along the depth meets something just in front, a shadow too small for the sun's
  // shadow maps. How dark (0: none), and how far it looks (world units).
  float contactShadows = 0;
  float contactLength = 1.0f;
  // (photofix2) A photo's frame drawn larger than the window: this many of its pixels make one of the window's. How far
  // a pixel looks is at most 48 of the window's pixels (and nothing under 1.5 of them), so that a photo's surroundings
  // reach as far over the ground as the window's (in its own pixels the cap took a smaller disc, whose nearer samples
  // shade more: a photo's grass came out darker than the window's). 1 in the game's own frames: as before.
  float pixelScale = 1;

 private:
  struct Constants {
    float tanHalfFov, aspect, zNear, radius;
    float strength, contact, contactLength, pixelScale;
    float sunInView[4], axisRight[4], axisUp[4], axisBack[4];
  };
  gfx::Device* device_;
  gfx::Pipeline compute_, apply_;
  gfx::Texture target_;
  int width_ = 0, height_ = 0;
  Constants constants_{};
};

}  // namespace render
