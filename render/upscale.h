// The picture drawn smaller than the screen and brought up to it with AMD's FidelityFX Super Resolution 1
// (FSR 1: an edge-aware upscale, EASU, then a sharpening, RCAS; MIT, render/third_party/fsr1). For slower
// cards: a player's option (Chris, 2026-10-15: "Let's just do fsr"). What is drawn on top afterwards -
// strategic icons, life bars, the interface - stays at the screen's own size.
#pragma once

#include <string>

#include "gfx.h"

namespace render {

class Upscaler {
 public:
  explicit Upscaler(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // `picture` (RGBA8 as a screen shows it, `inWidth` x `inHeight`) up to `outWidth` x `outHeight`: into a
  // target of its own, then sharpened into the window's pass, which is left begun (more may be drawn on
  // top of it).
  void Run(gfx::Texture picture, int inWidth, int inHeight, int outWidth, int outHeight);
  // FSR's sharpening: 0 the sharpest, each step of one half as sharp ("stops"); AMD suggests 0.2.
  float sharpness = 0.25f;

 private:
  gfx::Device* device_;
  gfx::Pipeline easu_, rcas_;
  gfx::Texture upscaled_;
  int width_ = 0, height_ = 0;
};

}  // namespace render
