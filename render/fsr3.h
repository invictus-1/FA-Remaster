// AMD's FidelityFX Super Resolution 3 upscaler (FSR 3.1, from the FidelityFX SDK v2.3.0; MIT,
// render/third_party/fsr3): the scene drawn smaller than the screen, a little shifted every frame (the
// jitter), and brought up to the screen's size by gathering what the frames before saw where each pixel
// was then (the motion), so the picture keeps more detail than it was drawn with and its edges stay smooth
// without several samples a pixel. Chris, 2026-10-15: "FSR only"; 2026-10-16: the SDK's download approved.
// A player's option, off by default (docs/render-lighting.md); FSR 1 (upscale.h) stays as the fallback.
//
// Its passes run as compute shaders through gfx.h, on Direct3D 12 and Vulkan alike. They need DXC
// (shader model 6): with Direct3D's own compiler only, Init fails and the host keeps FSR 1.
#pragma once
#include <utility>  // [views] (std::swap)

#include <string>

#include "gfx.h"

namespace render {

class Fsr3 {
 public:
  explicit Fsr3(gfx::Device* device) : device_(device) {}
  ~Fsr3();
  // Compiles its passes. False (with the reason) where they cannot be made.
  bool Init(std::string* error);

  // What one frame hands it.
  struct Inputs {
    gfx::Texture color;     // the scene at the size drawn (RGBA16F), linear light before exposure, drawn with this frame's jitter
    gfx::Texture depth;     // its depth (a D32 target, one sample a pixel): reversed (near 1, far 0), the far plane at infinity
    gfx::Texture motion;    // RG16F at the size drawn: where each pixel was last frame, as last frame's place minus this
                            // frame's, in the picture's 0..1 (u right, v down), the jitter left out of both
    gfx::Texture reactive;  // optional R8 at the size drawn: how much a pixel follows this frame alone (particles, sparks); none: 0
    int renderWidth = 0, renderHeight = 0;  // the size drawn
    float jitterX = 0, jitterY = 0;  // this frame's jitter in pixels of the size drawn (Jitter below)
    float exposure = 1;              // what the colour is multiplied by before the tone curve
    float cameraNear = 0.25f;        // the projection's near plane (world units)
    float fovY = 0.9f;               // its vertical field of view (radians)
    float frameSeconds = 1 / 60.0f;  // since the frame before
    bool reset = false;              // a cut (the camera jumped, the map changed): nothing of the frames before is used
    float sharpness = 0.25f;         // the sharpening after (RCAS): 0 the sharpest, each step of one half as sharp; negative: none
  };
  // Upscales into `output`, a storage target (RGBA16F) of `displayWidth` x `displayHeight`, linear light as
  // the colour came in. Between passes: it dispatches. The size drawn may change from frame to frame up to
  // the display's size; the internal pictures are made again when the display's size changes.
  void Run(const Inputs& in, gfx::Texture output, int displayWidth, int displayHeight);

  // The jitter of frame `index` (counting up every frame), in pixels of the size drawn (-0.5 .. 0.5), as FSR
  // wants it: Halton (2, 3) over a number of phases that grows with the upscaling.
  static void Jitter(int index, int renderWidth, int displayWidth, float* x, float* y);

  // [views] What it keeps for one view (its pictures at that view's size, what it keeps of the frames before) swapped
  // with another's, so each of a picture's views has its own (views.h, ViewSide); its pipelines and settings stay.
  void SwapView(Fsr3& o) { std::swap(impl_, o.impl_); }
 private:
  gfx::Device* device_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace render
