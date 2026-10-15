// The scene's own picture and the last pass: everything is drawn into a floating-point target, and
// the last pass puts that on the window - unchanged for the original look, or mapped from the modern
// look's open-ended linear light to what a screen can show.
#pragma once
#include <string>

#include "gfx.h"
#include "upscale.h"
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
  // Copies the picture as it is now into a texture that can be read (one sample a pixel), for
  // what looks through to what is behind it (water). False when the scene has one sample a
  // pixel (then there is no copy). It ends the pass: begin one again.
  bool CopyBehind();
  gfx::Texture Behind() const { return behind_; }
  // Begins the window's pass and draws the scene's picture into it. More may be drawn on top after.
  // With an `upscaler` and a screen larger than the scene: the picture as a screen shows it, then
  // brought up to the screen's size by FSR 1 (upscale.h).
  void Present(const View& view, Upscaler* upscaler = nullptr, int screenWidth = 0, int screenHeight = 0);

  // The modern look's glow round what is brighter than the screen can show (lamps, flashes, the
  // sun on water): how much of it is added; 0: none.
  float glow = 0.5f;

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_, toTarget_;  // the last pass into the window, or (before FSR) into ldr_
  gfx::Texture ldr_;  // the picture as a screen shows it, at the scene's size (before FSR)
  gfx::Texture color_, depth_, drawn_, behind_;
  static constexpr int kGlowSteps = 5;
  gfx::Pipeline glowDown_, glowUp_;
  gfx::Texture glowSteps_[kGlowSteps], black_;
  int glowWidth_[kGlowSteps] = {}, glowHeight_[kGlowSteps] = {};
  int width_ = 0, height_ = 0;
};

}  // namespace render
