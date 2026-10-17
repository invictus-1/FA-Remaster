// Motion: where each pixel of the scene was the frame before, as an offset in the picture (for the
// temporal upscaler, fsr3.h; docs/render-lighting.md, the groundwork). With it on, the scene is also
// drawn a little shifted every frame (the jitter), by less than a pixel, so that frames laid over each
// other see between the pixels.
//  - The camera's part, for everything: each pixel's place in the world from the scene's depth, seen
//    with last frame's view. Ground, water, scenery and the sky are right with it alone.
//  - What moves of itself (units, projectiles): MeshRenderer::DrawMotion draws each model again where
//    its place and bones put it, writing where they had it the frame before, over the camera's part.
// The offsets leave the jitter out (FSR's way): last frame's place minus this frame's, in the picture's
// 0..1 (u right, v down), in RG16F at the size drawn.
#pragma once

#include <string>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class MeshRenderer;

class Motion {
 public:
  explicit Motion(gfx::Device* device) : device_(device) {}
  ~Motion();
  bool Init(std::string* error);

  // At the frame's start, right after the view is made: shifts it by this frame's jitter (pixels of the
  // size drawn; 0, 0: none) and keeps its matrix without the jitter, and last frame's. `reset`: nothing
  // of last frame's view is used (a cut).
  void Begin(View* view, float jitterX, float jitterY, bool reset);
  // After the scene's solid surfaces and the water are drawn: the motion of every pixel. `depth`: the
  // scene's depth target, one sample a pixel. It begins passes of its own: begin the scene's again after.
  void Build(const View& view, gfx::Texture depth, MeshRenderer* meshes);
  gfx::Texture Texture() const { return motion_; }
  // For checking (the viewer's --motion-view): 1 the motion as colours (red right, green down, blue how
  // far); 2 the frame before's picture carried to this frame by the motion (with no jitter it is this
  // frame's picture, but where something comes out from behind another); 3 what that misses of this
  // frame, four times as bright (black where the motion is right). The picture to show in place
  // of the scene's (none: as it is); keeps `color` (the scene's, one sample a pixel) for the next frame.
  gfx::Texture Check(int mode, const View& view, gfx::Texture color);
  // The view's matrix this frame and last frame, without the jitter.
  const Mat4& Now() const { return now_; }
  const Mat4& Before() const { return before_; }
  float jitterX = 0, jitterY = 0;  // this frame's (Begin's)

 private:
  gfx::Device* device_;
  gfx::Pipeline camera_, colours_, carry_, copy_, error_;
  gfx::Texture motion_, shown_, lastColor_;
  bool haveLastColor_ = false;
  int width_ = 0, height_ = 0;
  Mat4 now_ = Mat4::Identity(), before_ = Mat4::Identity(), back_ = Mat4::Identity();  // back_: the shifted view's inverse
  bool haveBefore_ = false;
};

}  // namespace render
