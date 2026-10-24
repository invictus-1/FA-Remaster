// The picture drawn smaller than the screen and brought up to it with AMD's FidelityFX Super Resolution 1
// (FSR 1: an edge-aware upscale, EASU, then a sharpening, RCAS; MIT, render/third_party/fsr1). For slower
// cards: a player's option (Chris, 2026-10-15: "Let's just do fsr"). What is drawn on top afterwards -
// strategic icons, life bars, the interface - stays at the screen's own size.
#pragma once
#include <utility>  // [views] (std::swap)
#include <initializer_list>  // [views]

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

  // [views] What it keeps for one view (its pictures at that view's size, what it keeps of the frames before) swapped
  // with another's, so each of a picture's views has its own (views.h, ViewSide); its pipelines and settings stay.
  void SwapView(Upscaler& o) { std::swap(upscaled_, o.upscaled_), std::swap(width_, o.width_), std::swap(height_, o.height_); }
  // [views] A view's own (views.h, ViewSide) let go of: the pictures SwapView swaps, destroyed (a class without a
  // destructor of its own: the world's are let go of with the device).
  void ReleaseView() {
    if (upscaled_) {
      device_->Destroy(upscaled_);
      upscaled_ = {};
    }
    width_ = height_ = 0;
  }
 private:
  gfx::Device* device_;
  gfx::Pipeline easu_, rcas_;
  gfx::Texture upscaled_;
  int width_ = 0, height_ = 0;
};

}  // namespace render
