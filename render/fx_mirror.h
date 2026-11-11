#pragma once
// [fxmirror] The effects' mirror image in the water (World::waterFxMirror; the review of 2026-11-09, water 9: "what is
// drawn after the water is never mirrored in it").
//
// The modern water's mirror is the sky, and what the picture shows the mirrored way (water.cpp: a march through the
// scene's depth, reading the picture as it was before the water; with ray tracing, what a ray meets). Everything drawn
// after the water (the particles' fire, flashes and smoke, beams, tracers and trails, the volume blasts, the shields'
// bubbles) is in neither: they have no depth to march through and are no geometry for a ray, and they are the brightest
// things of a battle. A night fight at sea showed fire over water that mirrored none of it.
//
// So those kinds are drawn once more before the water, turned over about the water's level (a "planar reflection": the
// world mirrored in the plane of the water is, seen from the eye, what a flat sheet of water shows), into a picture of
// their own of half the scene's width and height (FxMirror, below), and the water adds that picture to its mirror where it
// lies: read a little to the side by the waves' tilt, as the mirrored ray off the wave differs from the one off flat
// water, several times along that tilt (a smear along it: the streak of light a wavy surface draws out of a lamp), under
// the water's Fresnel term and its foam as the rest of the mirror is. Laid over what the march or the rays found (by its
// alpha: fire and beams add, smoke covers).
//
// What is in front of what, without a depth for the mirrored picture: each mirrored pixel knows the place in the world it
// shows (above the water) and so the place on the water its image lies at (where the line from the eye to the place's
// mirror image goes through the surface). The way between those two is the mirrored ray; MirrorShows walks it in a few
// steps through the scene's own depth (as the water's march does) and leaves the pixel out where something solid stands
// in it: a hull or a shore between the water and the blast hides the blast's image as it hides it in real water. What
// lies under the water's level is not mirrored at all. (What the picture does not show cannot hide anything: an island
// off the screen, the far side of a hull. As with the water's own march.)
//
// It assumes one level of water (the map's sea); the streams' water (stream_water.h) lies at other heights and takes
// none of it. The lifted ocean (ocean.h) is treated as lying at its mean level.
//
// HLSL: kFxMirrorShaderBlock after the view's block, in the mirrored variants of the effects' shaders (built with
// FX_MIRROR_SAMPLES, the scene's samples a pixel). C++: FxMirror makes the picture and the mirrored view, and binds what
// the block reads.
#include <algorithm>
#include <cstring>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

inline constexpr int kFxMirrorSlot = 21;          // the water's shader: the effects' mirrored picture
inline constexpr int kFxMirrorDepthSlot = 7;      // the mirrored effects' shaders: the scene's depth
inline constexpr int kFxMirrorConstantsSlot = 3;  // and FxMirror's constants (b3)
inline constexpr gfx::Format kFxMirrorFormat = gfx::Format::RGBA16F;

inline const char kFxMirrorShaderBlock[] = R"hlsl(
cbuffer FxMirror : register(b3) {
  row_major float4x4 mirrorPicture;  // the picture's own view and projection (viewProj is the mirrored one in this pass)
  float4 mirrorEye;                  // the picture's eye; w: the water's level
  float4 mirrorMore;                 // x the camera's near distance; y, z the scene's size (its depth's), pixels
};
#if FX_MIRROR_SAMPLES > 1
Texture2DMS<float> mirrorSceneDepth : register(t7);
#define MIRROR_SCENE_DEPTH(at) mirrorSceneDepth.Load(at, 0)
#else
Texture2D<float> mirrorSceneDepth : register(t7);
#define MIRROR_SCENE_DEPTH(at) mirrorSceneDepth.Load(int3(at, 0))
#endif

// How much of something at `world` shows in the water's mirror (0..1): nothing of what lies under the water's level (a
// tenth of a unit above it: all), and nothing where something solid stands between the place on the water its image lies
// at and itself. That way is walked in five steps through the scene's depth; a step that lies behind what the picture
// shows there, by no more than a step's length and a little, has met it (further behind: it passes behind it, as the
// water's march has it).
float MirrorShows(float3 world) {
  float level = mirrorEye.w;
  float above = world.y - level;
  if (above <= 0 || mirrorEye.y <= level + 1e-3) return 0;
  float3 image = float3(world.x, 2 * level - world.y, world.z);
  float3 onWater = mirrorEye.xyz + (image - mirrorEye.xyz) * ((mirrorEye.y - level) / (mirrorEye.y - image.y));
  float3 way = world - onWater;
  const int kSteps = 5;
  float slack = length(way) / kSteps + 0.4;
  for (int k = 0; k < kSteps; ++k) {
    float4 c = mul(mirrorPicture, float4(onWater + way * ((k + 0.5) / kSteps), 1));
    if (c.w <= 0) continue;
    float2 at = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
    if (any(at < 0) || any(at >= 1)) continue;
    float there = MIRROR_SCENE_DEPTH(int2(at * mirrorMore.yz));
    float here = c.z / c.w;
    if (there > here && mirrorMore.x / here - mirrorMore.x / there < slack) return 0;
  }
  return saturate(above * 10);
}
)hlsl";

// The mirrored effects' picture and its pass (World::Render, before the water).
class FxMirror {
 public:
  explicit FxMirror(gfx::Device* device) : device_(device) {}

  // The view turned over about the water's level: what is drawn with it lies in the picture where its mirror image in
  // a flat sheet of water at `level` lies. Its eye is the mirrored eye (under the water), so what faces the camera
  // faces that; its sides are the picture's own, mirrored (BoxVisible).
  static View Mirrored(const View& view, float level) {
    View v = view;
    Mat4& m = v.constants.viewProj;
    for (int r = 0; r < 4; ++r) {  // (viewProj x the mirror: y -> 2 level - y)
      m.m[r][3] += 2 * level * m.m[r][1];
      m.m[r][1] = -m.m[r][1];
    }
    v.constants.cameraPosition[1] = 2 * level - v.constants.cameraPosition[1];
    v.eye.y = 2 * level - v.eye.y;
    for (Vec4& p : v.planes) {
      p.w += 2 * level * p.y;
      p.y = -p.y;
    }
    return v;
  }

  // Begins the mirrored picture's pass (it ends the pass under way: Post::ContinueScene afterwards), cleared to
  // nothing, half the view's size; and returns the view to draw with. `sceneDepth`: the scene's, as it is now
  // (everything solid drawn); `zNear` the camera's near distance.
  View Begin(const View& view, float level, gfx::Texture sceneDepth, float zNear) {
    const int width = std::max((view.width + 1) / 2, 1), height = std::max((view.height + 1) / 2, 1);
    if (!picture_ || width != width_ || height != height_) {
      if (picture_) device_->Destroy(picture_);
      gfx::TargetDesc td;
      td.width = width_ = width;
      td.height = height_ = height;
      td.format = kFxMirrorFormat;
      picture_ = device_->CreateTarget(td);
    }
    constants_.picture = view.constants.viewProj;
    std::memcpy(constants_.eye, view.constants.cameraPosition, 3 * sizeof(float));
    constants_.eye[3] = level;
    constants_.more[0] = zNear;
    constants_.more[1] = static_cast<float>(view.width);
    constants_.more[2] = static_cast<float>(view.height);
    constants_.more[3] = 0;
    depth_ = sceneDepth;
    View mirrored = Mirrored(view, level);
    mirrored.width = width;
    mirrored.height = height;
    const float viewport[4] = {static_cast<float>(width), static_cast<float>(height), 1 / static_cast<float>(width), 1 / static_cast<float>(height)};
    std::memcpy(mirrored.constants.viewport, viewport, sizeof viewport);
    gfx::PassDesc pass;
    pass.color = picture_;
    device_->BeginPass(pass);
    drawn = true;
    return mirrored;
  }
  // What the mirrored shaders' block reads, bound (after a pipeline is set, before its draws).
  void Bind() const {
    device_->SetConstants(kFxMirrorConstantsSlot, &constants_, sizeof constants_);
    device_->SetTexture(kFxMirrorDepthSlot, depth_);
  }
  // After the pass: the scene's depth let go of (it is drawn into again).
  void End() { device_->SetTexture(kFxMirrorDepthSlot, {}); }
  gfx::Texture Picture() const { return picture_; }
  // Whether Begin ran this frame (the world clears it each frame; the water reads the picture only then).
  bool drawn = false;

 private:
  struct Constants {
    Mat4 picture;
    float eye[4];
    float more[4];
  };
  gfx::Device* device_;
  gfx::Texture picture_, depth_;
  int width_ = 0, height_ = 0;
  Constants constants_ = {};
};

}  // namespace render
