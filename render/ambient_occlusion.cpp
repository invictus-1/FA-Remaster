#include "ambient_occlusion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lighting.h"

namespace render {
namespace {

// Compute: for each pixel, its position in front of the camera from the depth, its surface normal
// from the neighbouring pixels, then twelve samples on a spiral around it. A sample that lies in
// front of the surface (along the normal) shades it, less the further away it is (after McGuire et
// al., "The Alchemy Screen-Space Ambient Obscurance Algorithm"). The spiral is turned by a pattern
// that repeats every 4 x 4 pixels, which Apply's 4 x 4 average removes again.
// Contact shadows (when on): from the same place a short march toward the sun along the depth; a place
// the march finds something in front of, just behind it, lies in a shadow too small for the shadow maps.
// Only where the shadow maps say the sun reaches (else it is in shadow already).
// Output: red = openness (1 open, 0 shut in), green = distance from the camera, blue = 1 lit, 0 in a
// contact shadow.
const char kCompute[] = R"hlsl(
cbuffer Ao : register(b1) {
  float tanHalfFov, aspect, zNear, radius;
  float strength, contact, contactLength, pixelScale;
  float4 sunInView;   // xyz: toward the sun in the camera's space
  float4 axisRight, axisUp, axisBack;  // the camera's, in the world (its space to the world's)
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t0);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t0);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

struct VOut {
  float4 position : SV_Position;
};
VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

// position in the camera's space (it looks down -z); depth is zNear / distance
float3 ViewPosition(int2 p) {
  p = clamp(p, int2(0, 0), int2(viewport.xy) - 1);
  float d = DepthAt(p);
  float away = d > 0 ? zNear / d : 1e6;
  float2 ndc = float2((p.x + 0.5) * viewport.z * 2 - 1, 1 - (p.y + 0.5) * viewport.w * 2);
  return float3(ndc.x * tanHalfFov * aspect * away, ndc.y * tanHalfFov * away, -away);
}

float4 PS(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  float3 P = ViewPosition(p);
  float away = -P.z;
  // (blue 1 in both: lit, no contact shadow; 0 there darkened everything far off once contact shadows were on)
  if (away > 5e5) return float4(1, 1e6, 1, 1);  // nothing drawn here
  // (photofix2: both measured in the window's pixels, pixelScale of this picture's each: AmbientOcclusion::pixelScale)
  float pixelRadius = min(radius * viewport.y / (2 * tanHalfFov * away), 48.0 * pixelScale);
  if (pixelRadius < 1.5 * pixelScale) return float4(1, away, 1, 1);  // too far away to matter
  // [foliage] right at the eye (a cinematic camera among trees) mostly foliage fading out lies there (mesh_renderer.cpp,
  // NearFade): a pixel's first sample, read here, often holds a faded leaf's depth, and so near the estimate below
  // runs away (needles a hair apart in depth turn the pixel black). Nothing nearer than 0.8 world units is shut in,
  // and from there to 1.6 it comes in gradually (the game's camera comes that close only zoomed all the way in among
  // trees).
  if (away < 0.8) return float4(1, away, 1, 1);

  // the surface normal from whichever neighbour on each axis is nearer in depth (so edges stay edges)
  float3 l = ViewPosition(p + int2(-1, 0)), r = ViewPosition(p + int2(1, 0));
  float3 u = ViewPosition(p + int2(0, -1)), d = ViewPosition(p + int2(0, 1));
  float3 dx = abs(l.z - P.z) < abs(r.z - P.z) ? P - l : r - P;
  float3 dy = abs(u.z - P.z) < abs(d.z - P.z) ? P - u : d - P;
  float3 N = normalize(cross(dx, dy));
  if (dot(N, P) > 0) N = -N;  // toward the camera

  static const float turn[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  float angle = turn[(p.x & 3) + 4 * (p.y & 3)] * (6.2831853 / 16.0);
  const int count = 12;
  float sum = 0;
  float bias = 0.03 + 0.002 * away;
  for (int k = 0; k < count; ++k) {
    float t = (k + 0.5) / count;
    float a = angle + k * 2.3999632;  // the golden angle
    int2 q = p + int2(round(float2(cos(a), sin(a)) * (sqrt(t) * pixelRadius)));
    float3 v = ViewPosition(q) - P;
    float vv = dot(v, v);
    float inRange = saturate(1 - vv / (radius * radius));
    sum += max(0, dot(v, N) - bias) / (vv + 0.0001) * inRange;
  }
  float openness = saturate(1 - sum * radius * (1.1 / count));
  if (away < 1.6) openness = lerp(1, openness, smoothstep(0.8, 1.6, away));  // [foliage] (as above)
  float lit = 1;
  [branch] if (contact > 0) {
    float3 world = cameraPosition + axisRight.xyz * P.x + axisUp.xyz * P.y + axisBack.xyz * P.z;
    float3 worldN = normalize(axisRight.xyz * N.x + axisUp.xyz * N.y + axisBack.xyz * N.z);
    [branch] if (dot(N, sunInView.xyz) > 0 && SunShadow(world, worldN) > 0.5) {
      const int steps = 12;
      float start = (turn[(p.x & 3) + 4 * (p.y & 3)] + 0.5) / 16;
      for (int k = 0; k < steps; ++k) {
        float3 q = P + N * (0.02 + 0.001 * away) + sunInView.xyz * (contactLength * (k + start) / steps);
        float2 ndc = q.xy / (-q.z * tanHalfFov * float2(aspect, 1));
        int2 at = int2((ndc * float2(0.5, -0.5) + 0.5) * viewport.xy);
        if (any(at < 0) || any(at >= int2(viewport.xy))) break;
        float there = -ViewPosition(at).z, mine = -q.z;
        if (there < mine - 0.01 && mine - there < 0.4 + 0.004 * away) {  // (something just in front of the way: not far in front, a wall far off is not a shadow)
          lit = saturate(k / float(steps) * 1.5);  // (nearer the place, darker)
          break;
        }
      }
    }
  }
  return float4(openness, away, lit, 1);
}
)hlsl";

// Apply: the 4 x 4 average of the openness around the pixel, leaving out neighbours at another
// distance (so a unit's occlusion does not smear onto the ground behind it); drawn with Multiply.
const char kApply[] = R"hlsl(
cbuffer Ao : register(b1) {
  float tanHalfFov, aspect, zNear, radius;
  float strength, contact, contactLength, pixelScale;
};
Texture2D occlusion : register(t0);

struct VOut {
  float4 position : SV_Position;
};
VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PS(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  int2 last = int2(viewport.xy) - 1;
  float here = occlusion.Load(int3(p, 0)).g;
  float sum = 0, lit = 0, weight = 0;
  for (int y = -2; y < 2; ++y)
    for (int x = -2; x < 2; ++x) {
      float3 s = occlusion.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)).rgb;
      float w = abs(s.g - here) < 0.03 * here + 0.05 ? 1 : 0;
      sum += s.r * w;
      lit += s.b * w;
      weight += w;
    }
  float openness = weight > 0 ? sum / weight : 1;
  float k = lerp(1, openness, saturate(strength)) * lerp(1, weight > 0 ? lit / weight : 1, saturate(contact));
  return float4(k, k, k, 1);
}
)hlsl";

// [coverdark] The ground cover left out: the scene's depth (its first sample) kept before the cover is drawn (Ground),
// then where the cover lies after it, in front of that (Cover; 0 elsewhere: the depth is zNear / distance, never 0 on
// something drawn); before the occlusion, the scene's depth with the cover taken out (Solid: where the cover is still
// what is seen, nothing drawn over it since, the ground's behind it), which Compute then reads in place of the scene's.
const char kCoverDepth[] = R"hlsl(
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t0);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t0);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
Texture2D<float> groundDepth : register(t1);
Texture2D<float> coverDepth : register(t2);

struct VOut {
  float4 position : SV_Position;
};
VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float Ground(VOut i) : SV_Target { return DepthAt(int2(i.position.xy)); }
float Cover(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  float d = DepthAt(p);
  return d > groundDepth.Load(int3(p, 0)) ? d : 0;  // (nearer is more: the cover drawn in front of the ground)
}
float Solid(VOut i) : SV_Target {
  int2 p = int2(i.position.xy);
  float d = DepthAt(p), c = coverDepth.Load(int3(p, 0));
  return c > 0 && d == c ? groundDepth.Load(int3(p, 0)) : d;
}
)hlsl";

}  // namespace

bool AmbientOcclusion::Init(std::string* error) {
  std::string compute = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kLightingShaderBlock + kCompute;
  gfx::PipelineDesc pd;
  pd.name = "ambient occlusion";
  pd.vertexSource = pd.pixelSource = compute;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.colorFormat = gfx::Format::RGBA16F;
  compute_ = device_->CreatePipeline(pd, error);
  if (!compute_) return false;

  std::string apply = std::string(kViewShaderBlock) + kApply;
  pd.name = "ambient occlusion, applied";
  pd.vertexSource = pd.pixelSource = apply;
  pd.depthTarget = true;  // it draws into the scene's pass, which has its depth buffer bound
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Multiply;
  apply_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(apply_);
}

void AmbientOcclusion::Compute(const View& view, const Camera& camera, gfx::Texture sceneDepth) {
  if (view.width != width_ || view.height != height_ || !target_) {
    if (target_) device_->Destroy(target_);
    gfx::TargetDesc td;
    td.width = view.width;
    td.height = view.height;
    td.format = gfx::Format::RGBA16F;
    td.clearValue[0] = 1;
    target_ = device_->CreateTarget(td);
    width_ = view.width;
    height_ = view.height;
  }
  // [coverdark] the ground cover left out this frame (CoverAfter): the scene's depth with it taken out, read in its place
  const bool cover = coverNow_ && coverWidth_ == view.width && coverHeight_ == view.height;
  coverNow_ = false;
  if (cover) {
    gfx::PassDesc solid;
    solid.color = solidDepth_;
    solid.clearColor = false;  // every pixel is written
    device_->BeginPass(solid);
    device_->SetPipeline(coverSolid_);
    device_->SetTexture(0, sceneDepth);
    device_->SetTexture(1, groundDepth_);
    device_->SetTexture(2, coverDepth_);
    device_->Draw(3);
    device_->SetTexture(1, {});
    device_->SetTexture(2, {});
    sceneDepth = solidDepth_;
  }
  constants_.tanHalfFov = std::tan(camera.fovY * 0.5f);
  constants_.aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
  constants_.zNear = camera.zNear;
  constants_.radius = radius;
  constants_.strength = strength;
  constants_.contact = contactShadows;
  constants_.contactLength = contactLength;
  constants_.pixelScale = std::max(pixelScale, 1e-3f);  // (photofix2)
  {  // the sun in the camera's space, and the camera's axes in the world's
    Vec3 r, u, b;
    camera.Axes(&r, &u, &b);
    const Vec3 sun{view.constants.sunDirection[0], view.constants.sunDirection[1], view.constants.sunDirection[2]};
    const float in[4] = {Dot(sun, r), Dot(sun, u), Dot(sun, b), 0};
    std::copy(in, in + 4, constants_.sunInView);
    const float axes[3][4] = {{r.x, r.y, r.z, 0}, {u.x, u.y, u.z, 0}, {b.x, b.y, b.z, 0}};
    std::copy(axes[0], axes[0] + 4, constants_.axisRight);
    std::copy(axes[1], axes[1] + 4, constants_.axisUp);
    std::copy(axes[2], axes[2] + 4, constants_.axisBack);
  }

  gfx::PassDesc pass;
  pass.color = target_;
  pass.clearColor = false;  // every pixel is written
  device_->BeginPass(pass);
  device_->SetPipeline(cover ? computeCover_ : compute_);  // ([coverdark] one sample a pixel)
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &constants_, sizeof constants_);
  device_->SetTexture(0, sceneDepth);
  device_->Draw(3);
}

// [coverdark] The ground cover left out of the occlusion: its shaders made the first time it is wanted, its three
// pictures at the view's size.
bool AmbientOcclusion::CoverReady(const View& view) {
  if (coverMade_ < 0) return false;
  if (!coverMade_) {
    coverMade_ = -1;
    std::string error;
    const std::string samples = gSceneSamples > 1 ? "#define MSAA 1\n" : "";
    // (Compute then reads the scene's depth with the cover taken out, one sample a pixel: with several samples a
    // pipeline of its own for it, else its own)
    const std::string compute = std::string(kViewShaderBlock) + kLightingShaderBlock + kCompute;
    gfx::PipelineDesc pd;
    pd.name = "ambient occlusion, the ground cover left out";
    pd.vertexSource = pd.pixelSource = compute;
    pd.cull = gfx::Cull::None;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.colorFormat = gfx::Format::RGBA16F;
    computeCover_ = gSceneSamples > 1 ? device_->CreatePipeline(pd, &error) : compute_;
    const std::string copy = samples + kCoverDepth;
    pd.vertexSource = pd.pixelSource = copy;
    pd.colorFormat = gfx::Format::R32F;
    pd.name = "ambient occlusion: the ground's depth";
    pd.pixelEntry = "Ground";
    if (computeCover_) groundCopy_ = device_->CreatePipeline(pd, &error);
    pd.name = "ambient occlusion: the ground cover's depth";
    pd.pixelEntry = "Cover";
    if (groundCopy_) coverMark_ = device_->CreatePipeline(pd, &error);
    pd.name = "ambient occlusion: the depth without the ground cover";
    pd.pixelEntry = "Solid";
    if (coverMark_) coverSolid_ = device_->CreatePipeline(pd, &error);
    if (!coverSolid_) {
      fprintf(stderr, "render: the ground cover cannot be left out of the ambient occlusion (%s)\n", error.c_str());
      return false;
    }
    coverMade_ = 1;
  }
  if (view.width != coverWidth_ || view.height != coverHeight_ || !groundDepth_ || !coverDepth_ || !solidDepth_) {
    for (gfx::Texture t : {groundDepth_, coverDepth_, solidDepth_})
      if (t) device_->Destroy(t);
    gfx::TargetDesc td;
    td.width = view.width;
    td.height = view.height;
    td.format = gfx::Format::R32F;
    groundDepth_ = device_->CreateTarget(td);
    coverDepth_ = device_->CreateTarget(td);
    solidDepth_ = device_->CreateTarget(td);
    coverWidth_ = view.width;
    coverHeight_ = view.height;
  }
  return groundDepth_ && coverDepth_ && solidDepth_;
}

void AmbientOcclusion::CoverBefore(const View& view, gfx::Texture sceneDepth) {
  coverNow_ = coverKept_ = false;
  if (!CoverReady(view)) return;
  gfx::PassDesc pass;
  pass.color = groundDepth_;
  pass.clearColor = false;  // every pixel is written
  device_->BeginPass(pass);
  device_->SetPipeline(groundCopy_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, sceneDepth);
  device_->Draw(3);
  device_->SetTexture(0, {});  // (the scene's depth is its target again next)
  coverKept_ = true;
}

void AmbientOcclusion::CoverAfter(const View& view, gfx::Texture sceneDepth) {
  if (!coverKept_) return;
  coverKept_ = false;
  gfx::PassDesc pass;
  pass.color = coverDepth_;
  pass.clearColor = false;  // every pixel is written
  device_->BeginPass(pass);
  device_->SetPipeline(coverMark_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetTexture(0, sceneDepth);
  device_->SetTexture(1, groundDepth_);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  coverNow_ = true;
}

void AmbientOcclusion::Apply(const View& view) {
  device_->SetPipeline(apply_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &constants_, sizeof constants_);
  device_->SetTexture(0, target_);
  device_->Draw(3);
}

}  // namespace render
