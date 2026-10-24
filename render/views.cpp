// [views] Several views in one picture: views.h.
#include "views.h"

#include <algorithm>
#include <utility>

namespace render {

ViewSide::ViewSide(gfx::Device* d)
    : device(d), post(d), ao(d), upscaler(d), ranges(d), outlines(d), fsr3(d), motion(d), air(d), clouds(d), bounce(d) {}

ViewSide::~ViewSide() {
  // (what has no destructor of its own lets go of its pictures here; the rest by their destructors)
  post.ReleaseView();
  ao.ReleaseView();
  upscaler.ReleaseView();
  ranges.ReleaseView();
  for (gfx::Texture t : {color, depth})
    if (t) device->Destroy(t);
}

bool ViewSide::Size(int w, int h) {
  w = std::max(w, 16);
  h = std::max(h, 16);
  if (color && depth && w == width && h == height) return true;
  for (gfx::Texture* t : {&color, &depth}) {
    if (*t) device->Destroy(*t);
    *t = {};
  }
  gfx::TargetDesc td;
  td.width = w;
  td.height = h;
  td.format = gfx::Format::RGBA8;  // (as the window's: the pipelines that draw into the window draw into it)
  color = device->CreateTarget(td);
  td.format = gfx::Format::D32;  // (cleared to 0: depth is reversed)
  depth = device->CreateTarget(td);
  width = w;
  height = h;
  drawn = false;
  return color && depth;
}

// The view's own parts into the world and the world's into `side`; called again after the view's frame, it puts them back.
void World::SwapView(ViewSide& s) {
  post.SwapView(s.post);
  ao.SwapView(s.ao);
  upscaler.SwapView(s.upscaler);
  ranges.SwapView(s.ranges);
  outlines.SwapView(s.outlines);
  fsr3.SwapView(s.fsr3);
  motion.SwapView(s.motion);
  air.SwapView(s.air);
  clouds.SwapView(s.clouds);
  bounce.SwapView(s.bounce);
  meshes.SwapBefore(s.before);
  std::swap(camera, s.camera);
  std::swap(view, s.view);
  std::swap(screenView_, s.screenView);
  std::swap(screenWidth_, s.screenWidth);
  std::swap(screenHeight_, s.screenHeight);
  barAnchors.swap(s.barAnchors);
  std::swap(temporalNow_, s.temporalNow);
  std::swap(temporalBefore_, s.temporalBefore);
  std::swap(temporalCut_, s.temporalCut);
  std::swap(fsr3State_, s.fsr3State);
  std::swap(temporalFrame_, s.temporalFrame);
  std::swap(lastEye_, s.lastEye);
  std::swap(lensSnap_, s.lensSnap);
  std::swap(framegenNow_, s.framegenNow);
  std::swap(framegenBefore_, s.framegenBefore);
  std::swap(framegenCut_, s.framegenCut);
  std::swap(bounceTried_, s.bounceTried);
  std::swap(bounceMade_, s.bounceMade);
  std::swap(bounceNow_, s.bounceNow);
  std::swap(lampUnits_, s.lampUnits);
  std::swap(lampUnitsBefore_, s.lampUnitsBefore);
}

static const char kLayShader[] = R"hlsl(
cbuffer Place : register(b1) {
  float4 rect;    // the picture's top left and size in the window's pixels
  float4 window;  // the window's width and height
};
Texture2D picture : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position {
  const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
  const float2 at = rect.xy + corners[id] * rect.zw;
  return float4(at / window.xy * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 at : SV_Position) : SV_Target { return float4(picture.Load(int3(at.xy - rect.xy, 0)).rgb, 1); }
)hlsl";

ViewLayer::~ViewLayer() {
  if (pipeline_) device_->Destroy(pipeline_);
}

bool ViewLayer::Init(std::string* error) {
  gfx::PipelineDesc pd;
  pd.name = "a view's picture into the window";
  pd.vertexSource = pd.pixelSource = kLayShader;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.colorFormat = gfx::Format::RGBA8;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void ViewLayer::Lay(gfx::Texture picture, int x, int y, int width, int height, int windowWidth, int windowHeight) {
  if (!pipeline_ || !picture) return;
  const float place[8] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height),
                          static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0, 0};
  device_->SetPipeline(pipeline_);
  device_->SetConstants(1, place, sizeof place);
  device_->SetTexture(0, picture);
  device_->Draw(6);
  device_->SetTexture(0, {});
}

}  // namespace render
