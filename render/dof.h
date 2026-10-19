// Depth of field: the cinematic camera's lens (cinecam.h, Lens), what is nearer or farther than the distance it is
// focused at blurred as a real lens blurs it. Run on the scene's finished light before the glow and the tone curve
// (Post::beforeFinish), at the size the picture is finished at: after FSR 3 has brought the scene up to the
// screen's size, so before frame generation keeps the finished picture without the interface. With any number of
// the scene's samples a pixel (its depth read at the first). Off unless World::lens is on.
//
// How (written from the ideas of J. Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare",
// SIGGRAPH 2014, and the thin lens's sums; nothing copied):
//  - Focus: with auto-focus, the distance at the picture's middle (of 25 places round it, a little nearer than the
//    middle one, so that a unit there is taken and not the ground behind it), eased toward over time in steps of
//    the lens's power (1 / distance, as a focus puller turns the ring), kept on the card from frame to frame.
//  - Each pixel's blur, its "circle of confusion": the radius in pixels a point there spreads to (thin lens: the
//    aperture's width times the focal length over the focus distance less it, times how far the point is from the
//    focus over its own distance), signed: negative nearer than the focus.
//  - At half the width and height, each place's light (the four pixels' mean) and its blur (the least of the
//    four: a place across a sharp edge stays sharp and does not spread the sharp thing's colour).
//  - Tiles of 16 x 16 pixels with the widest blur of what is nearer than the focus round them: what is nearer
//    spreads over what lies beside it, so each place looks that far for it.
//  - Gather: from each place, rings of samples over a disc as wide as the blur there; a sample's light reaches the
//    place when its own disc covers it ("scatter as gather"), spread over its disc (weighed by 1 / its area). Two
//    layers: the far one (behind the focus), where what lies behind the place reaches it only as far as the
//    place's own blur (what is in front hides it: no halo of a blurred background over a sharp thing in front),
//    and the near one, whose share of the place is how much of it the near discs cover. Bright points become
//    discs of light (bokeh), as bright as their light spread over the disc.
//  - At the picture's size: the sharp picture where the blur is under a pixel, the far layer (from the half-size
//    places of a blur like the pixel's own) where it is more, the near layer over it.
#pragma once
#include <string>

#include "camera.h"
#include "cinecam.h"
#include "gfx.h"

namespace render {

class DepthOfField {
 public:
  explicit DepthOfField(gfx::Device* device) : device_(device) {}
  ~DepthOfField();

  // The picture blurred by `lens` as `camera` sees it: `picture` the scene's finished light (`width` x `height`),
  // `depth` the scene's depth (`depthWidth` x `depthHeight`, any number of samples; the picture may be larger: FSR 3
  // brought it up). `seconds`: the frame's time, for the auto-focus's easing; `snap`: focus at once (a cut, a
  // photo). Its shaders are made the first time. Returns the picture to finish in place of `picture` (the same
  // when it cannot run). Between passes: it dispatches.
  gfx::Texture Apply(const Lens& lens, const Camera& camera, gfx::Texture picture, int width, int height, gfx::Texture depth, int depthWidth,
                     int depthHeight, float seconds, bool snap);
  bool Failed() const { return state_ < 0; }
  // (photofix2) Its working pictures let go of (a photo's, at the photo's size: some 200 MB at 5120 x 2880, kept until
  // the lens was next used); the next Apply makes them anew at its size. The focus is kept. Between frames.
  void Release();

  int rings = 7;          // the most rings of samples round a place (8 more each ring out): what the widest blur costs
  float spacing = 1.4f;   // the samples' spacing, in half-size pixels, before the rings run out
  // The height of the water's surface (-1e9: none). The water writes no depth, so what lies under it would set the
  // blur and the focus of the water seen over it: where a pixel's line of sight meets this surface first, that is
  // taken instead.
  float waterLevel = -1e9f;

 private:
  bool Init(std::string* error);
  bool Size(int width, int height);
  gfx::Device* device_;
  int state_ = 0;  // 0 shaders not made yet, 1 made, -1 they cannot be
  gfx::Pipeline focus_, prepare_, tiles_, dilate_, gather_, composite_;
  gfx::Texture focusNow_[2], half_, tileNear_, dilated_, far_, near_, out_;
  int width_ = 0, height_ = 0, frame_ = 0;
  bool haveFocus_ = false;
};

}  // namespace render
