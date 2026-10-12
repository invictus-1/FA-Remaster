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

 private:
  struct Constants {
    float tanHalfFov, aspect, zNear, radius;
    float strength, pad[3];
  };
  gfx::Device* device_;
  gfx::Pipeline compute_, apply_;
  gfx::Texture target_;
  int width_ = 0, height_ = 0;
  Constants constants_{};
};

}  // namespace render
