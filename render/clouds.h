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
// (cloudzoom) Unless the zoom through them is on (`zoom`, cloud_zoom.h): then the eye rises into and above the
// layer, and the clouds below it are laid over the ground, thinned so the map stays readable.
#pragma once

#include <algorithm>
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
  // (volume explosions, volume_blasts.h) A nuke's shock ring, set by the world each frame: its middle (x, z), radius and
  // width (world units), how strongly it clears the clouds inside it and heaps them at its front (0: none). Marched by
  // shaders of their own, made the first frame `ringsWanted` (so that without a ring the clouds are as they were).
  float ringX = 0, ringZ = 0, ringRadius = 0, ringWidth = 1, ringStrength = 0;
  bool ringsWanted = false;

  // (cloudzoom) Zooming out through the clouds (cloud_zoom.h, World::cloudZoom), set by the world before Build each
  // frame. While `on` and the eye is no more than 40 under the layer, the layer stays where it is (it no longer keeps
  // 40 over the eye: the eye goes into it and above it) and the view is marched through it wherever it lies before
  // what was drawn, the ground too; Picture() then holds the clouds over the ground as well (Zoomed()), which
  // CloudZoom::Lay puts over the scene. Off, or with the eye lower: the march as without it.
  struct Zoom {
    bool on = false;
    float erode = 0;         // everywhere: how much is eaten from the clouds' thin outer parts (0 nothing .. 1 all but the hearts)
    float keep = 1;          // and what is kept of the density of the rest (1 all)
    float partErode = 0;     // round the view's middle, at its strongest: eaten as by `erode` ...
    float partKeep = 1;      // ... and kept as by `keep`
    float partFrom = 0.35f;  // the parting whole up to this far off the view's middle (in half the view's heights)
    float partTo = 1.1f;     // and gone from this far
    float near = 25;         // clouds nearer the eye than this fade away (world units), so the eye is never in a white-out
    float gain = 1;          // the density times this (seen from close, outlines crisper)
    float lumps = 0;         // how far lumps raise and lower their tops (in the layer's thickness): heaps, not a sheet
    float powder = 0;        // how much thin outer parts seen away from the light are darkened (the billows' edges)
    float fine = 0;          // how deep a finer detail bites into their outer parts (billows on top, wisps at the edges)
    float fineSize = 0.27f;  // its size against the detail's (Clouds::detailSize)
    float history = 0.85f;   // how much of the frame before a pixel keeps (as `keep`, below the layer) ...
    float letGo = 10;        // ... less as the eye moves: none once it has gone a tenth (1 / this) of the clouds' distance
    float inside = 0.3f;     // the least of the falling light that reaches deep inside them (light scattered on and on) ...
    float insideFade = 0.25f;  // ... fading by this share of their depth toward the light (as without the zoom: 0.3, 0.25)
  };
  Zoom zoom;
  bool Zoomed() const { return built_ && zoomedNow_; }  // this frame's Picture() holds clouds over the ground too
  // The layer's underside as the march has it for an eye at that height (the lightning's flash is put in it).
  float Underside(float eyeY) const { return zoom.on && marchZoom_ && eyeY > base - 40 ? base : std::max(base, eyeY + 40); }

 private:
  bool Size(int width, int height);
  gfx::Device* device_;
  gfx::Pipeline shapeNoise_, detailNoise_, coverage_, shadowMarch_, march_;
  gfx::Pipeline ringShadowMarch_, ringMarch_;  // (the same with a nuke's ring; made when rings are first wanted)
  bool ringsTried_ = false;
  bool RingNow() const { return ringStrength > 0 && ringMarch_ && ringShadowMarch_; }
  gfx::Texture shape_, detail_, cover_, shadow_, marched_[2];
  bool noiseMade_ = false, built_ = false, haveLast_ = false, prepared_ = false;
  float coverAt_[2] = {};
  int width_ = 0, height_ = 0, now_ = 0;
  uint32_t frame_ = 0;
  Mat4 lastViewProj_ = Mat4::Identity();
  // (cloudzoom) the zoom's march (made the first frame the zoom is on), whether this frame was marched by it, the
  // finer coverage picture round the camera it reads, and where the eye was the frame before
  gfx::Pipeline marchZoom_;
  bool zoomTried_ = false, zoomedNow_ = false;
  gfx::Texture coverFine_;
  Vec3 lastEye_{0, 0, 0};
};

}  // namespace render
