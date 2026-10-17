// Volumetric clouds (the modern look's lighting upgrade, docs/render-lighting.md step 4): the layer of
// clouds under a computed sky as real volumes in place of the sky pass's few steps through a flat noise.
//  - Two tiling noise volumes, made once on the card: the clouds' shapes (Perlin-Worley, 128 cubed) and
//    the detail that frays their edges (Worley, 32 cubed).
//  - Where clouds are at all: the same field the ground's cloud shadows come from (view.h, CloudField and
//    CloudFrom), laid into a picture over the land around the camera each frame, so the shadows on the
//    ground stay under the clouds above.
//  - The view marched through the layer at half the picture's width and height, only where the sky
//    shows past what was drawn (the scene's depth): how thick the cloud is at each step, the light that
//    reaches it from the sun or moon through the cloud on the way (bright edges toward the light) and from
//    the sky around; each frame a new start inside the steps, blended with what the same direction showed
//    the frame before.
// The sky's pass lays the result over the sky. The clouds stay above the eye (the layer's underside at
// least 40 units over it) and are only seen against the sky: they never hide the battlefield.
#pragma once

#include <cstdint>
#include <string>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "sky.h"
#include "view.h"

namespace render {

class Clouds {
 public:
  explicit Clouds(gfx::Device* device) : device_(device) {}
  ~Clouds();
  bool Init(std::string* error);

  // Before the scene's passes (it dispatches): where the clouds are round the camera, and their shadows
  // (what comes through them toward the sun, at their underside), which every lit surface and the light in
  // the air then read (`view`'s cloudShadow, kCloudShadowSlot). Off (or no computed sky, or no clouds):
  // nothing, the view's cloudShadow left 0.
  void Prepare(View* view, const Camera& camera, bool on);
  // In place of Prepare for a frame drawn for something else (World::sidePicture): none this frame (Build
  // then does nothing), and what they hold of the frames before is kept.
  void Skip(View* view);
  gfx::Texture Shadows() const { return prepared_ ? shadow_ : gfx::Texture{}; }
  // After the scene's solid surfaces and the water, before the sky: the clouds where the sky shows.
  // `depth`: the scene's depth (any number of samples). It dispatches: begin the scene's pass again after.
  // Nothing unless Prepare was on this frame; then Picture is none.
  void Build(const View& view, const Camera& camera, const SkyLight& sky, gfx::Texture depth);
  // rgb: the clouds' light (as the sky's pass counts light); a: what of the sky behind comes through.
  gfx::Texture Picture() const { return built_ ? marched_[now_] : gfx::Texture{}; }

  // The look, changeable between frames.
  float base = 150;          // the layer's underside over the ground's zero (world units; at least 40 over the eye)
  float thickness = 140;     // how deep it is
  float density = 0.09f;     // how much light a world unit of full cloud takes
  int steps = 64;            // steps through the layer
  float keep = 0.85f;        // how much of the frame before a pixel keeps
  int shrink = 2;            // the march at the picture's size (1) or at half its width and height (2)
  float shapeSize = 600;     // the shapes' noise repeats every this many world units
  float detailSize = 110;    // the detail's
  // A storm's lightning, set by the world each frame: where its flash is in the layer (where the bolt leaves the
  // clouds), how far its light spreads through them, and its light (0: no flash). It lights them from within.
  Vec3 boltAt{0, 0, 0};
  float boltReach = 120;
  Vec3 boltLight{0, 0, 0};

 private:
  bool Size(int width, int height);
  gfx::Device* device_;
  gfx::Pipeline shapeNoise_, detailNoise_, coverage_, shadowMarch_, march_;
  gfx::Texture shape_, detail_, cover_, shadow_, marched_[2];
  bool noiseMade_ = false, built_ = false, haveLast_ = false, prepared_ = false;
  float coverAt_[2] = {};
  int width_ = 0, height_ = 0, now_ = 0;
  uint32_t frame_ = 0;
  Mat4 lastViewProj_ = Mat4::Identity();
};

}  // namespace render
