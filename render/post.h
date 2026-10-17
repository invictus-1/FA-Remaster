// The scene's own picture and the last pass: everything is drawn into a floating-point target, and
// the last pass puts that on the window - unchanged for the original look, or mapped from the modern
// look's open-ended linear light to what a screen can show.
#pragma once
#include <string>

#include "fsr3.h"
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
  gfx::Texture Color() const { return color_; }  // the scene's picture (one sample a pixel: what is drawn into)
  // A picture shown in place of the scene's this frame (the scene's size and format; none: the scene's).
  gfx::Texture shown;
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
  // For the temporal upscaler (fsr3.h), with one sample a pixel. KeepOpaque, once the solid scene and the
  // water are drawn (it ends the pass: begin one again), keeps the picture as it is then; Reactive, once
  // all of it is drawn, makes the mask of where what came after (smoke, sparks, beams: what has no motion
  // of its own) changed the picture, and how much (R8, the scene's size; none without KeepOpaque).
  void KeepOpaque();
  gfx::Texture Reactive();
  // Begins a pass into what KeepOpaque kept, keeping what is there (false: nothing kept): for what darkens
  // the picture where it stands still (the fog of war), drawn there too, so that the reactive mask does not
  // take it for something that moves. ContinueScene afterwards.
  bool BeginKept();
  // The temporal upscaler (fsr3.h): the scene's picture (one sample a pixel) brought up to `screenWidth` x
  // `screenHeight` by FSR 3 from `in` (its colour, depth and size drawn left empty: the scene's own), for this
  // frame's Present to finish: the glow and the tone curve then at the screen's size, FSR 1 not run.
  void UpscaleTemporal(Fsr3* fsr3, Fsr3::Inputs in, int screenWidth, int screenHeight);
  // Begins the window's pass and draws the scene's picture into it. More may be drawn on top after.
  // With an `upscaler` and a screen larger than the scene: the picture as a screen shows it, then
  // brought up to the screen's size by FSR 1 (upscale.h).
  void Present(const View& view, Upscaler* upscaler = nullptr, int screenWidth = 0, int screenHeight = 0);

  // The modern look's glow round what is brighter than the screen can show (lamps, flashes, the
  // sun on water): how much of it is added; 0: none.
  float glow = 0.5f;
  // The original look's bloom, as the original makes it (render 28): on (the game's ren_bloom), its blur's
  // scale (ren_BloomBlurKernelScale), the glow copy's scale (ren_BloomGlowCopyScale) and what it adds
  // (the map's lighting Bloom), and how many times it blurs along and across (ren_BloomBlurCount).
  struct OriginalBloom {
    bool on = false;
    float blurScale = 1.5f, copyScale = 2.0f, copyAdd = 0.08f;
    int blurCount = 2;
  } originalBloom;

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_, toTarget_;  // the last pass into the window, or (before FSR) into ldr_
  gfx::Pipeline copy_;  // the picture copied into behind_ (one sample a pixel)
  gfx::Texture ldr_;  // the picture as a screen shows it, at the scene's size (before FSR)
  gfx::Texture color_, depth_, drawn_, behind_;
  static constexpr int kGlowSteps = 5;
  gfx::Pipeline glowDown_, glowUp_;
  gfx::Texture glowSteps_[kGlowSteps], black_;
  int glowWidth_[kGlowSteps] = {}, glowHeight_[kGlowSteps] = {};
  gfx::Pipeline bloomCopy_, bloomBlur_;  // (the original look's bloom)
  gfx::Texture bloom_[2];
  int bloomWidth_ = 0, bloomHeight_ = 0;
  int width_ = 0, height_ = 0;
  // the glow's and the bloom's targets are made for the size of the picture finished
  int finishWidth_ = 0, finishHeight_ = 0;
  void SizeFinishing(int width, int height);
  gfx::Texture opaque_, reactive_;  // (KeepOpaque, Reactive)
  gfx::Texture upscaled_;  // (UpscaleTemporal: FSR 3's picture at the screen's size, until the next Present)
  int upscaledWidth_ = 0, upscaledHeight_ = 0;
  bool upscaledNow_ = false;
  bool keptOpaque_ = false;
  gfx::Pipeline reactiveMask_;
};

}  // namespace render
