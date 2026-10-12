#include "ambient_occlusion.h"

#include <cmath>

namespace render {
namespace {

// Compute: for each pixel, its position in front of the camera from the depth, its surface normal
// from the neighbouring pixels, then twelve samples on a spiral around it. A sample that lies in
// front of the surface (along the normal) shades it, less the further away it is (after McGuire et
// al., "The Alchemy Screen-Space Ambient Obscurance Algorithm"). The spiral is turned by a pattern
// that repeats every 4 x 4 pixels, which Apply's 4 x 4 average removes again.
// Output: red = openness (1 open, 0 shut in), green = distance from the camera.
const char kCompute[] = R"hlsl(
cbuffer Ao : register(b1) {
  float tanHalfFov, aspect, zNear, radius;
  float strength; float3 aoPad;
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
  if (away > 5e5) return float4(1, 1e6, 0, 1);  // nothing drawn here
  float pixelRadius = min(radius * viewport.y / (2 * tanHalfFov * away), 48.0);
  if (pixelRadius < 1.5) return float4(1, away, 0, 1);  // too far away to matter

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
  return float4(openness, away, 0, 1);
}
)hlsl";

// Apply: the 4 x 4 average of the openness around the pixel, leaving out neighbours at another
// distance (so a unit's occlusion does not smear onto the ground behind it); drawn with Multiply.
const char kApply[] = R"hlsl(
cbuffer Ao : register(b1) {
  float tanHalfFov, aspect, zNear, radius;
  float strength; float3 aoPad;
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
  float sum = 0, weight = 0;
  for (int y = -2; y < 2; ++y)
    for (int x = -2; x < 2; ++x) {
      float2 s = occlusion.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)).rg;
      float w = abs(s.g - here) < 0.03 * here + 0.05 ? 1 : 0;
      sum += s.r * w;
      weight += w;
    }
  float openness = weight > 0 ? sum / weight : 1;
  float k = lerp(1, openness, saturate(strength));
  return float4(k, k, k, 1);
}
)hlsl";

}  // namespace

bool AmbientOcclusion::Init(std::string* error) {
  std::string compute = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kCompute;
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
  constants_.tanHalfFov = std::tan(camera.fovY * 0.5f);
  constants_.aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
  constants_.zNear = camera.zNear;
  constants_.radius = radius;
  constants_.strength = strength;

  gfx::PassDesc pass;
  pass.color = target_;
  pass.clearColor = false;  // every pixel is written
  device_->BeginPass(pass);
  device_->SetPipeline(compute_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &constants_, sizeof constants_);
  device_->SetTexture(0, sceneDepth);
  device_->Draw(3);
}

void AmbientOcclusion::Apply(const View& view) {
  device_->SetPipeline(apply_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &constants_, sizeof constants_);
  device_->SetTexture(0, target_);
  device_->Draw(3);
}

}  // namespace render
