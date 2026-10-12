#include "sky.h"

#include <algorithm>
#include <cmath>

#include "post.h"

namespace render {
namespace {

// One triangle over the picture; each pixel's direction from the camera, then the sky's light from
// there (ComputedSky, view.h), the sun's and the moon's disc, and stars by night.
const char kShader[] = R"hlsl(
cbuffer SkyRays : register(b1) {
  float4 rayForward;  // the direction through the picture's middle; w: how bright the sky is shown
  float4 rayRight;    // what one half picture to the right adds
  float4 rayUp;       // what one half picture upward adds
};

struct VOut {
  float4 position : SV_Position;
  float3 ray : TEXCOORD0;
};

VOut VS(uint id : SV_VertexID) {
  VOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.ray = rayForward.xyz + rayRight.xyz * (uv.x * 2 - 1) + rayUp.xyz * (1 - uv.y * 2);
  return o;
}

float Hash(float3 p) {
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float4 PS(VOut i) : SV_Target {
  float3 d = normalize(i.ray);
  float3 c = ComputedSky(d);
  float overHorizon = saturate(d.y * 60 + 0.5);
  // the sun: a disc of 0.8 degrees, redder when low
  float3 sunDisc = lerp(float3(1, 0.42, 0.12), float3(1, 0.95, 0.85), saturate(skyBody.y * 4)) * 24;
  c += sunDisc * (smoothstep(0.99988, 0.99992, dot(d, skyBody.xyz)) * overHorizon);
  // the moon, opposite: 1.3 degrees, pale by day
  c += float3(0.75, 0.8, 1) * ((0.08 + 0.9 * skyHorizon.w) * smoothstep(0.99972, 0.99976, dot(d, -skyBody.xyz)) * overHorizon);
  // stars: one in some of the cells the directions are sorted into
  [branch] if (skyHorizon.w > 0.01 && d.y > 0) {
    float3 p = d * 380;
    float3 cell = floor(p);
    float h = Hash(cell);
    float star = step(0.991, h) * saturate(1 - length(p - cell - 0.5) * 2.4);
    c += star * star * (0.15 + frac(h * 97.3)) * skyHorizon.w * saturate(d.y * 5) * 0.5;
  }
  return float4(c * rayForward.w, 1);
}
)hlsl";

struct RayConstants {
  float forward[4], right[4], up[4];
};

float Smooth(float from, float to, float x) {
  float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// Everything of the light at that hour but how bright it is against noon.
SkyLight Parts(float hours) {
  const float kPi = 3.14159265f;
  const float tilt = 35 * kPi / 180;  // how far from overhead the sun stands at noon
  const float a = (hours - 6) / 12 * kPi;
  SkyLight s;
  s.body = {std::cos(a), std::sin(a) * std::cos(tilt), std::sin(a) * std::sin(tilt)};
  const float h = s.body.y;  // the sine of the sun's height

  // three skies, blended by the sun's height: day, dusk (or dawn), night
  const float day = Smooth(0.0f, 0.30f, h);
  const float night = Smooth(0.02f, -0.22f, h);
  const float dusk = std::max(0.0f, 1 - day - night);
  auto blend = [&](Vec3 atDay, Vec3 atDusk, Vec3 atNight) { return atDay * day + atDusk * dusk + atNight * night; };
  s.zenith = blend({0.10f, 0.26f, 0.68f}, {0.07f, 0.09f, 0.24f}, {0.004f, 0.007f, 0.020f});
  s.horizon = blend({0.48f, 0.64f, 0.88f}, {0.85f, 0.38f, 0.14f}, {0.010f, 0.015f, 0.032f});
  s.glow = blend({0.55f, 0.50f, 0.38f}, {1.20f, 0.50f, 0.15f}, {0.030f, 0.036f, 0.055f});
  s.night = night;

  // the light that falls: the sun's while it is up, the moon's in the night, neither in between
  s.moon = h < -0.01f;
  if (s.moon) {
    s.color = Vec3{0.50f, 0.62f, 1.0f} * (0.06f * Smooth(-0.02f, -0.15f, h));
  } else {
    const float high = Smooth(0.0f, 0.35f, h);
    s.color = (Vec3{1.0f, 0.50f, 0.22f} * (1 - high) + Vec3{1.0f, 0.96f, 0.90f} * high) * Smooth(0.0f, 0.15f, h);
  }
  // (a light from the horizon itself would throw shadows without end: it is taken from a little higher)
  Vec3 from = s.moon ? s.body * -1.0f : s.body;
  from.y = std::max(from.y, 0.2f);
  s.direction = Normalize(from);
  return s;
}

// How much light reaches level ground: the falling light and the sky's.
float GroundLight(const SkyLight& s) {
  auto brightness = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
  return brightness(s.color) * s.direction.y * 2 + brightness(s.zenith * 0.8f + s.horizon * 0.2f) * 1.1f;
}

}  // namespace

SkyLight SkyAt(float hours) {
  SkyLight s = Parts(hours);
  static const float noon = GroundLight(Parts(12));
  s.daylight = std::clamp(GroundLight(s) / noon, 0.0f, 1.0f);
  // the eye opens as the light goes, but not all the way: dusk and night stay darker than day
  s.exposure = std::pow(1 / std::max(s.daylight, 0.02f), 0.72f);
  return s;
}

void SetSky(const SkyLight& s, ViewConstants* c) {
  auto set = [](float* to, Vec3 v, float w) {
    to[0] = v.x;
    to[1] = v.y;
    to[2] = v.z;
    to[3] = w;
  };
  c->sunDirection[0] = s.direction.x;
  c->sunDirection[1] = s.direction.y;
  c->sunDirection[2] = s.direction.z;
  // (the shaders take the sun's colour as the map says it, as a screen value)
  c->sunColor[0] = std::pow(s.color.x, 1 / 2.2f);
  c->sunColor[1] = std::pow(s.color.y, 1 / 2.2f);
  c->sunColor[2] = std::pow(s.color.z, 1 / 2.2f);
  for (float& f : c->shadowFill) f *= s.daylight;  // the map's fill light is daylight too
  c->modern[2] *= s.exposure;
  set(c->skyZenith, s.zenith, 1);
  set(c->skyHorizon, s.horizon, s.night);
  set(c->skyGlow, s.glow, s.daylight);
  set(c->skyBody, s.body, s.moon ? 1.0f : 0.0f);
}

bool Sky::Init(std::string* error) {
  std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "sky";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void Sky::Draw(const View& view, const Camera& camera) {
  if (view.constants.skyZenith[3] < 0.5f || view.height <= 0) return;
  // the picture's middle and what half a picture adds to a direction, all at the same depth
  const float w = static_cast<float>(view.width), h = static_cast<float>(view.height);
  const Vec3 forward = camera.PixelRay(w * 0.5f, h * 0.5f, w, h).direction;
  auto atDepthOne = [&](float px, float py) {
    const Vec3 d = camera.PixelRay(px, py, w, h).direction;
    return d * (1 / Dot(d, forward));
  };
  const Vec3 right = atDepthOne(w, h * 0.5f) - forward;
  const Vec3 up = atDepthOne(w * 0.5f, 0) - forward;
  // (the picture is exposed for the ground, more the darker it is; the sky is shown with less of
  // that, or a night's sky would stand as bright as a day's)
  const float shown = std::pow(std::max(view.constants.skyGlow[3], 0.02f), 0.43f);
  RayConstants rc{{forward.x, forward.y, forward.z, shown}, {right.x, right.y, right.z, 0}, {up.x, up.y, up.z, 0}};
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &rc, sizeof rc);
  device_->Draw(3);
}

}  // namespace render
