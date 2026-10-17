// AMD's FidelityFX Super Resolution 3 frame generation (from the FidelityFX SDK v2.3.0; MIT,
// render/third_party/fsr3): a picture made between two frames drawn, halfway in time, so that the screen can show
// twice the frames the card draws. Its optical flow follows how the finished picture itself moves (what the
// game's motion misses: shadows sliding, transparent effects); its frame interpolation carries the frame before
// and this one halfway along the game's motion and the optical flow's, fills what neither shows (what comes out
// from behind something) from around it, and lays this frame's interface over the result unmoved.
//
// Its passes run as compute shaders through gfx.h, on Direct3D 12 and Vulkan alike, as Fsr3's (fsr3.h); they need
// DXC (shader model 6, with wave operations). Showing the made-up frame between the real ones (the swap chain, the
// pacing) is the host's.
#pragma once

#include <string>

#include "gfx.h"

namespace render {

class FrameGeneration {
 public:
  explicit FrameGeneration(gfx::Device* device);
  ~FrameGeneration();
  // Compiles its passes. False (with the reason) where they cannot be made.
  bool Init(std::string* error);

  // What one frame hands it.
  struct Inputs {
    gfx::Texture depth;    // at the size drawn: D32, reversed (near 1, far 0), the far plane at infinity
    gfx::Texture motion;   // RG16F at the size drawn: last frame's place minus this frame's, in the picture's 0..1
                           // (u right, v down), the jitter left out (the same as Fsr3::Inputs::motion)
    gfx::Texture hudless;  // at the display's size, RGBA8 (the window's format, sRGB-encoded values in UNORM):
                           // the finished picture before the icons and the interface
    gfx::Texture present;  // the same with the icons and the interface on it (none: `hudless`, and then
                           // whatever is on it is interpolated as the world is)
    int renderWidth = 0, renderHeight = 0;
    float jitterX = 0, jitterY = 0;  // this frame's jitter in pixels of the size drawn (0 without the upscaler)
    float cameraNear = 0.25f, fovY = 0.9f, frameSeconds = 1 / 60.0f;
    bool reset = false;    // a cut: nothing of the frame before is used (the output is then this frame)
  };
  // The picture halfway in time between the frame before (the last Run's) and this one, into `output`: a
  // storage target (RGBA8) of displayWidth x displayHeight. Between passes: it dispatches. Keeps what the
  // next Run needs (this frame's colour as AMD's "previous interpolation source", the optical flow's
  // history). The internal pictures are made again when the display's or the largest drawn size changes.
  //
  // As AMD's: the first Run after a reset (or a cut the optical flow sees) gives this frame itself; for the ten
  // after it only the game's motion carries the picture, and from then on the optical flow's too where the two
  // disagree (the optical flow itself starts six frames after a reset).
  void Run(const Inputs& in, gfx::Texture output, int displayWidth, int displayHeight);

 private:
  gfx::Device* device_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace render
