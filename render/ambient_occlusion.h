// Ambient occlusion from the picture's own depth: creases, the ground under a unit and the gaps
// between parts get darker, as they would where the sky's light cannot reach.
//
// Two steps. Compute reads the scene's depth after everything solid is drawn and writes, per pixel,
// how open the surroundings are. Apply multiplies the scene's picture by that, before water and
// anything else see-through is drawn on top. It is a correction made on the finished lighting (the
// sun's direct light is dimmed with the rest), which is the usual trade for doing it without a
// separate depth pass.
#pragma once
#include <utility>  // [views] (std::swap)
#include <initializer_list>  // [views]
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
  // [coverdark] The ground cover (grass.h) left out: its blades are one to three pixels wide and its cards thin, which
  // the depth alone takes for solid walls (each blade shut in by the ground in front of its foot, the ground round it
  // shut in by the blade: dark slashes and halos all over a meadow). Around the cover's drawing, CoverBefore keeps the
  // ground's depth and CoverAfter where the cover then lies in front of it; this frame's Compute then reads, where the
  // cover is still what is seen, the ground's depth behind it: the cover neither shades the ground nor itself, and
  // takes the occlusion of the ground it stands on (by a unit, a rock, a crease) and its contact shadows. Each begins
  // its own pass (Post::ContinueScene afterwards); call both or neither in a frame, with Compute after them.
  void CoverBefore(const View& view, gfx::Texture sceneDepth);
  void CoverAfter(const View& view, gfx::Texture sceneDepth);

  float radius = 0.9f;    // world units: how far a surface looks for things that shade it
  float strength = 1.0f;  // 0: no effect
  // Contact shadows (the lighting upgrade; World::contactShadows): with the occlusion, where a short march
  // toward the sun along the depth meets something just in front, a shadow too small for the sun's
  // shadow maps. How dark (0: none), and how far it looks (world units).
  float contactShadows = 0;
  float contactLength = 1.0f;
  // [shadowmask] The sun's shadow on the ground worked out once a pixel (sun_mask.h), for the contact shadows' test of where the
  // sun reaches, where a pixel shows the ground: the mask (SunMask::Rows, its words a row and rows) and the ground's depth it
  // was worked out from (SunMask::Depth); none: each pixel works it out, as before. How near a pixel's depth (the scene's first
  // sample) must be to the ground's, a share of it. Set by the world each frame.
  gfx::Buffer sunMask;
  gfx::Texture sunMaskDepth;
  int sunMaskWords = 0, sunMaskHeight = 0;
  float sunMaskTolerance = 2e-3f;
  // (photofix2) A photo's frame drawn larger than the window: this many of its pixels make one of the window's. How far
  // a pixel looks is at most 48 of the window's pixels (and nothing under 1.5 of them), so that a photo's surroundings
  // reach as far over the ground as the window's (in its own pixels the cap took a smaller disc, whose nearer samples
  // shade more: a photo's grass came out darker than the window's). 1 in the game's own frames: as before.
  float pixelScale = 1;
  // [hitches] The ground cover's leaving out (CoverBefore): its pipelines made now (World::readAhead: the first frame the
  // cover is on), not the first time the camera comes close enough for grass; its pictures then, as before.
  bool PrepareCover() { return CoverPipelines(); }

  // [views] What it keeps for one view (its pictures at that view's size, what it keeps of the frames before) swapped
  // with another's, so each of a picture's views has its own (views.h, ViewSide); its pipelines and settings stay.
  void SwapView(AmbientOcclusion& o) {
    std::swap(target_, o.target_), std::swap(width_, o.width_), std::swap(height_, o.height_);
    std::swap(groundDepth_, o.groundDepth_), std::swap(coverDepth_, o.coverDepth_), std::swap(solidDepth_, o.solidDepth_);
    std::swap(coverWidth_, o.coverWidth_), std::swap(coverHeight_, o.coverHeight_), std::swap(coverKept_, o.coverKept_);
    std::swap(coverNow_, o.coverNow_);
  }
  // [views] A view's own (views.h, ViewSide) let go of: the pictures SwapView swaps, destroyed (a class without a
  // destructor of its own: the world's are let go of with the device).
  void ReleaseView() {
    for (gfx::Texture* t : {&target_, &groundDepth_, &coverDepth_, &solidDepth_})
      if (*t) {
        device_->Destroy(*t);
        *t = {};
      }
    width_ = height_ = coverWidth_ = coverHeight_ = 0;
  }
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
  // [coverdark] (the ground cover left out: its shaders, made the first time it is wanted, -1 they cannot be; the
  // ground's depth, the cover's and the scene's without it, each one 32-bit float a pixel at the view's size; kept /
  // marked this frame)
  bool CoverReady(const View& view);
  bool MaskPipelines();  // [shadowmask]
  gfx::Pipeline computeMask_, computeCoverMask_;
  int maskMade_ = 0;
  bool CoverPipelines();  // [hitches] (CoverReady's pipelines: PrepareCover)
  gfx::Pipeline computeCover_, groundCopy_, coverMark_, coverSolid_;
  gfx::Texture groundDepth_, coverDepth_, solidDepth_;
  int coverMade_ = 0, coverWidth_ = 0, coverHeight_ = 0;
  bool coverKept_ = false, coverNow_ = false;
};

}  // namespace render
