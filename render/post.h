// The scene's own picture and the last pass: everything is drawn into a floating-point target, and
// the last pass puts that on the window - unchanged for the original look, or mapped from the modern
// look's open-ended linear light to what a screen can show.
#pragma once
#include <string>

#include "gfx.h"
#include "view.h"

namespace render {

class Post {
 public:
  explicit Post(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);

  // Begins the pass everything in the scene is drawn in; makes or resizes the targets as needed.
  void BeginScene(int width, int height, const float clearColor[4]);
  // The scene's depth, readable once another pass has begun (several samples a pixel when the
  // scene is anti-aliased).
  gfx::Texture Depth() const { return depth_; }
  // Begins the scene's pass again after a pass in between, keeping what was drawn.
  void ContinueScene();
  // Begins a pass into the scene's colour alone, keeping what was drawn: for work that reads the
  // scene's depth while it changes the colour (decals). ContinueScene afterwards.
  void BeginColorOnly();
  // Begins the window's pass and draws the scene's picture into it. More may be drawn on top after.
  void Present(const View& view);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  gfx::Texture color_, depth_, drawn_;
  int width_ = 0, height_ = 0;
};

}  // namespace render
