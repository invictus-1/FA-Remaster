#include "clouds.h"
#include "cloud_zoom.h"  // (cloudzoom: the zoom's march)

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace render {
namespace {

constexpr int kShapeSize = 128, kDetailSize = 32;  // the noise volumes' cells
constexpr int kCoverSize = 512;                    // the coverage picture's pixels across
constexpr float kCoverReach = 16000;               // the world units it covers across, round the camera

// The two noise volumes: tiling (each repeats across its cube), made once.
const char kNoiseShader[] = R"hlsl(
cbuffer Noise : register(b1) {
  float4 noiseSize;  // x: cells across
};
RWTexture3D<float4> noiseOut : register(u0);

uint3 Pcg3(uint3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}
float3 Random3(int3 cell, int period) {
  cell = (cell % period + period) % period;  // (tiling)
  return float3(Pcg3(uint3(cell))) / 4294967295.0;
}
// 1 at a feature point, falling to 0 a cell away: cells of `period` across the cube
float Worley(float3 p, int period) {
  int3 c = int3(floor(p));
  float3 f = frac(p);
  float best = 1e9;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        int3 o = int3(x, y, z);
        float3 r = float3(o) + Random3(c + o, period) - f;
        best = min(best, dot(r, r));
      }
  return 1 - saturate(sqrt(best));
}
// gradient noise, 0..1 near enough, tiling at `period`
float Perlin(float3 p, int period) {
  int3 c = int3(floor(p));
  float3 f = frac(p);
  float3 u = f * f * f * (f * (f * 6 - 15) + 10);
  float n[8];
  for (int k = 0; k < 8; ++k) {
    int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
    float3 g = normalize(Random3(c + o, period) * 2 - 1 + 1e-4);
    n[k] = dot(g, f - float3(o));
  }
  float x0 = lerp(n[0], n[1], u.x), x1 = lerp(n[2], n[3], u.x), x2 = lerp(n[4], n[5], u.x), x3 = lerp(n[6], n[7], u.x);
  return lerp(lerp(x0, x1, u.y), lerp(x2, x3, u.y), u.z) * 0.6 + 0.5;
}
float Remap(float v, float a, float b, float c, float d) { return c + (v - a) / (b - a) * (d - c); }

// the shapes: r Perlin-Worley (soft heaps with rounded edges), g b a Worley at three sizes
[numthreads(4, 4, 4)] void Shape(uint3 id : SV_DispatchThreadID) {
  float3 p = (id + 0.5) / noiseSize.x;
  float perlin = Perlin(p * 4, 4) * 0.5 + Perlin(p * 8, 8) * 0.25 + Perlin(p * 16, 16) * 0.125 + Perlin(p * 32, 32) * 0.125;
  float w1 = Worley(p * 4, 4), w2 = Worley(p * 8, 8), w3 = Worley(p * 16, 16);
  float worley = w1 * 0.625 + w2 * 0.25 + w3 * 0.125;
  // (Perlin-Worley lands between about 0.62 and 0.95: stretched over the whole of 0..1, or every place
  // would be cloud and the outlines would be the coverage's alone)
  float perlinWorley = Remap(perlin, 0, 1, worley, 1);
  noiseOut[id] = float4(saturate((perlinWorley - 0.62) / 0.33), w1, w2, w3);
}
// the detail: Worley at three sizes
[numthreads(4, 4, 4)] void Detail(uint3 id : SV_DispatchThreadID) {
  float3 p = (id + 0.5) / noiseSize.x;
  noiseOut[id] = float4(Worley(p * 2, 2), Worley(p * 4, 4), Worley(p * 8, 8), 1);
}
)hlsl";

// Each frame: where clouds are round the camera (the ground's cloud shadows' field), then the march.
const char kShader[] = R"hlsl(
cbuffer Clouds : register(b1) {
  float4 cloudRight;  // xyz: the camera's right x tan(half the view's width); w: the layer's underside
  float4 cloudUp;     // xyz: up x tan(half the view's height); w: its thickness
  float4 cloudAhead;  // xyz: forward; w: where in its steps this frame starts (0..1)
  float4 cloudSize;   // xy: the marched picture's size; z: 1 / the coverage's reach; w: how much of the frame before is kept
  row_major float4x4 lastViewProj;
  float4 cloudLook;   // x: light taken per world unit of full cloud; y: steps; z, w: 1 / the shapes' and the detail's sizes
  float4 cloudLight;  // rgb: the falling light on the clouds
  float4 coverAt;     // xy: the coverage picture's middle (world x, z); z: the shadows' underside
  float4 cloudBolt;   // a lightning's flash in the layer: xyz where; w how far its light spreads (world units)
  float4 boltLight;   // rgb: its light (0: no flash)
#ifdef RING
  float4 cloudRing;   // (volume explosions) a nuke's shock ring: xy its middle (world x, z); z its radius; w how strongly
  float4 ringLook;    // x its width (world units)
#endif
};

RWTexture2D<float> coverOut : register(u0);

[numthreads(8, 8, 1)] void Coverage(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  coverOut.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 q = coverAt.xy + ((id.xy + 0.5) / float2(w, h) - 0.5) / cloudSize.z;
  // [sunlight] (drifting cloud shadows, sunlight.h) the heaps of many sizes, as many as the flat layer's drifting shadows
  // have (view.h, CloudShadow: about the cover's share of the land under them), full in their middles, thinning over their
  // last few units (the broken field varies less than the field: CloudFrom's thresholds would leave few clouds)
  [branch] if (cloudDrift.x > 0.5) {
    coverOut[id.xy] = saturate((CloudFieldBroken(q) - lerp(0.63, 0.37, skyClouds.x)) * 6 + 0.6);
    return;
  }
  coverOut[id.xy] = CloudFrom(CloudField(q));
}

RWTexture2D<float> shadowOut : register(u2);
Texture2D<float> coverFor : register(t4);
Texture3D shapeNoise : register(t0);
Texture3D detailNoise : register(t1);
Texture2D lastClouds : register(t2);
#define coverPicture coverFor
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t3);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif
RWTexture2D<float4> cloudsOut : register(u1);

float Remap(float v, float a, float b, float c, float d) { return c + (v - a) / (b - a) * (d - c); }
// Henyey-Greenstein against its average (1: as much to all sides)
float Phase(float c, float g) {
  float g2 = g * g;
  return (1 - g2) / pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5);
}

#ifdef RING
#define Cloud CloudBeforeRing
#endif
// How much cloud there is at a place, `h` its height in the layer (0 underside .. 1 top). `frayed`: with
// the detail that frays the edges (the steps toward the light go without it).
float Cloud(float3 p, float h, bool frayed) {
  float2 at = (p.xz - coverAt.xy) * cloudSize.z + 0.5;
  if (any(at < 0) || any(at > 1)) return 0;
  float cover = coverPicture.SampleLevel(samplerLinearClamp, at, 0);
  if (cover <= 0.002) return 0;
  // flat underneath, rounded at the top: the more cloud a place has, the higher it heaps
  float profile = saturate(h * 12) * saturate(Remap(h, 0.15 + 0.75 * cover, 1, 1, 0));
  float3 wind = float3(skyClouds.y, skyClouds.w * 40, skyClouds.z);
  float4 s = shapeNoise.SampleLevel(samplerLinear, (p + wind) * cloudLook.z, 0);
  float low = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
  float shape = Remap(s.r, low - 1, 1, 0, 1) * profile;
  // (coverage only lets the shapes through: where they are low, a hole; their heaps make the outline)
  float d = saturate(Remap(shape, 1 - cover * 0.6, 1, 0, 1)) * cover;
  if (!frayed || d <= 0) return d;
  float3 e = detailNoise.SampleLevel(samplerLinear, (p + wind * 1.4) * cloudLook.w, 0).rgb;
  float fine = e.r * 0.625 + e.g * 0.25 + e.b * 0.125;
  float fray = lerp(fine, 1 - fine, saturate(h * 4));  // (wisps below, billows above)
  return saturate(Remap(d, fray * 0.6, 1, 0, 1));
}
#ifdef RING
#undef Cloud
// (volume explosions, volume_blasts.h) A nuke's shock ring through the layer: what was inside it is pushed out to its
// front (a place reads the cloud from nearer the middle, the more the nearer the front), cleared behind the front and
// heaped up at it.
float Cloud(float3 p, float h, bool frayed) {
  float2 off = p.xz - cloudRing.xy;
  float r = length(off), width = max(ringLook.x, 1);
  float pushed = cloudRing.w * cloudRing.z * 0.4 * exp(-max(r - cloudRing.z, 0) / width);
  float3 from = float3(cloudRing.x, p.y, cloudRing.y) + float3(off.x, 0, off.y) * (max(r - pushed, 0) / max(r, 1e-3));
  float cleared = cloudRing.w * (1 - smoothstep(cloudRing.z - width * 1.5, cloudRing.z - width * 0.2, r));
  float front = (r - cloudRing.z) / width;
  float heaped = 1 + 0.8 * cloudRing.w * exp(-front * front);
  return saturate(CloudBeforeRing(from, h, frayed) * (1 - cleared) * heaped);
}
#endif

// The clouds' shadows: from each place of the layer's underside (the coverage picture's places) through
// the layer toward the sun, what comes through.
[numthreads(8, 8, 1)] void ShadowMarch(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  shadowOut.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float bottom = coverAt.z, thick = cloudUp.w;
  float3 sun = skyBody.w > 0.5 ? skyMoon.xyz : skyBody.xyz;
  sun.y = max(sun.y, 0.2);
  sun = normalize(sun);
  float3 p = float3(coverAt.x, bottom, coverAt.y) + float3(((id.xy + 0.5) / float2(w, h) - 0.5) / cloudSize.z, 0).xzy;
  float span = thick / sun.y, step = span / 8, through = 0;
  for (int k = 0; k < 8; ++k) {
    float3 q = p + sun * (step * (k + 0.5));
    through += Cloud(q, saturate((q.y - bottom) / thick), true) * step;  // (frayed, as they are seen)
  }
  shadowOut[id.xy] = exp(-through * cloudLook.x);
}

[numthreads(8, 8, 1)] void March(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(cloudSize.xy))) return;
  float2 uv = (id.xy + 0.5) / cloudSize.xy;
  float3 d = normalize(cloudAhead.xyz + cloudRight.xyz * (uv.x * 2 - 1) + cloudUp.xyz * (1 - uv.y * 2));
  float4 result = float4(0, 0, 0, 1);
  // only where the sky shows past what was drawn (its depth is the far end's, 0) in one of the four pixels
  uint fullWidth, fullHeight;
#ifdef MSAA
  uint samples;
  sceneDepth.GetDimensions(fullWidth, fullHeight, samples);
#else
  sceneDepth.GetDimensions(fullWidth, fullHeight);
#endif
  int2 full = min(int2((id.xy + 0.5) * float2(fullWidth, fullHeight) / cloudSize.xy), int2(fullWidth, fullHeight) - 2);
  float nearest = min(min(DepthAt(full), DepthAt(full + int2(1, 0))), min(DepthAt(full + int2(0, 1)), DepthAt(full + int2(1, 1))));
  [branch] if (nearest <= 0 && d.y > 0.012) {
    float bottom = max(cloudRight.w, cameraPosition.y + 40), thick = cloudUp.w;
    float t0 = (bottom - cameraPosition.y) / d.y, t1 = (bottom + thick - cameraPosition.y) / d.y;
    t1 = min(t1, t0 + 5000);  // (low toward the horizon: a long way through it)
    int steps = int(cloudLook.y);
    float dt = (t1 - t0) / steps;
    float3 body = skyBody.w > 0.5 ? skyMoon.xyz : skyBody.xyz;
    float c = dot(d, body);
    float phase = lerp(Phase(c, -0.15), Phase(c, 0.65), 0.5);  // (light that goes on forward: bright edges toward the sun)
    float3 around = skyZenith.rgb * 0.75 + skyHorizon.rgb * 0.45;
    float lightStep = thick * 0.1;
    float3 light = 0;
    float through = 1;
    float t = t0 + dt * cloudAhead.w;
    const bool flashing = boltLight.r + boltLight.g + boltLight.b > 0;
    for (int k = 0; k < steps && through > 0.01; ++k, t += dt) {
      float3 p = cameraPosition + d * t;
      float h = saturate((p.y - bottom) / thick);
      float cloud = Cloud(p, h, true);
      if (cloud <= 0.002) continue;
      // the cloud between this place and the light
      float toward = 0;
      for (int j = 0; j < 5; ++j) {
        float3 q = p + body * (lightStep * (j + 0.5) * (1 + j * 0.5));
        toward += Cloud(q, saturate((q.y - bottom) / thick), false) * lightStep * (1 + j * 0.5);
      }
      float depthToLight = toward * cloudLook.x;
      // (light scattered on and on inside a cloud keeps its insides from going black)
      float lit = max(exp(-depthToLight), 0.3 * exp(-depthToLight * 0.25));
      float3 scattered = cloudLight.rgb * (lit * phase * 1.1) + around * (0.12 + 0.55 * h);
      // a lightning's flash lights the cloud from within round where the bolt leaves it, fading through it
      if (flashing) {
        float far = length(p - cloudBolt.xyz) / cloudBolt.w;
        scattered += boltLight.rgb * (exp(-far * 1.5) / (1 + far * far));
      }
      float sigma = cloud * cloudLook.x;
      float stepThrough = exp(-sigma * dt);
      light += through * scattered * (1 - stepThrough);
      through *= stepThrough;
    }
    float fade = smoothstep(0.012, 0.12, d.y);  // (down at the horizon the layer goes over into the sky's haze)
    result = float4(light * fade, lerp(1, through, fade));
  }
  // built up over frames: what this direction showed the frame before (the clouds are far: the camera's
  // turn moves them, its going hardly)
  [branch] if (cloudSize.w > 0) {
    float4 clip = mul(lastViewProj, float4(d, 0));
    if (clip.w > 1e-4) {
      float2 lastUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
      if (all(lastUv >= 0) && all(lastUv <= 1)) result = lerp(result, lastClouds.SampleLevel(samplerLinearClamp, lastUv, 0), cloudSize.w);
    }
  }
  cloudsOut[id.xy] = result;
}
)hlsl";

struct Constants {
  float right[4], up[4], ahead[4], size[4];
  Mat4 lastViewProj;
  float look[4], light[4], coverAt[4];
  float bolt[4], boltLight[4];
  float ring[4], ringLook[4];  // (the ring's shaders only)
};

}  // namespace

Clouds::~Clouds() {
  for (gfx::Texture t : {shape_, detail_, cover_, shadow_, marched_[0], marched_[1], coverFine_})
    if (t) device_->Destroy(t);
}

bool Clouds::Init(std::string* error) {
  gfx::ComputeDesc cd;
  cd.source = kNoiseShader;
  cd.name = "clouds: shapes' noise";
  cd.entry = "Shape";
  shapeNoise_ = device_->CreateCompute(cd, error);
  if (!shapeNoise_) return false;
  cd.name = "clouds: detail's noise";
  cd.entry = "Detail";
  detailNoise_ = device_->CreateCompute(cd, error);
  if (!detailNoise_) return false;
  const std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kShader;
  cd.source = source;
  cd.name = "clouds: where they are";
  cd.entry = "Coverage";
  coverage_ = device_->CreateCompute(cd, error);
  if (!coverage_) return false;
  cd.name = "clouds: their shadows";
  cd.entry = "ShadowMarch";
  shadowMarch_ = device_->CreateCompute(cd, error);
  if (!shadowMarch_) return false;
  cd.name = "clouds: marched";
  cd.entry = "March";
  march_ = device_->CreateCompute(cd, error);
  return static_cast<bool>(march_);
}

bool Clouds::Size(int width, int height) {
  const int by = std::clamp(shrink, 1, 4);
  const int w = std::max(1, (width + by - 1) / by), h = std::max(1, (height + by - 1) / by);
  if (w == width_ && h == height_ && marched_[0] && marched_[1]) return true;
  for (gfx::Texture& t : marched_) {
    if (t) device_->Destroy(t);
    t = {};
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = w;
  td.height = h;
  td.format = gfx::Format::RGBA16F;
  marched_[0] = device_->CreateTarget(td);
  marched_[1] = device_->CreateTarget(td);
  width_ = w;
  height_ = h;
  haveLast_ = false;
  return marched_[0] && marched_[1];
}

// (the constants both steps give their shaders: the camera's rays, the layer, the look)
static void FillConstants(void* to, const View& view, const Camera& camera, const Clouds& c, float under, const float coverAt[2]);

void Clouds::Skip(View* view) {
  prepared_ = built_ = false;
  std::fill(std::begin(view->constants.cloudShadow), std::end(view->constants.cloudShadow), 0.0f);
}

// [session] (clouds.h) the shaders with a nuke's ring
void Clouds::PrepareRings() {
  if (ringsTried_ || !march_) return;
  {
    ringsTried_ = true;
    std::string error;
    const std::string source = std::string("#define RING 1\n") + (gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kShader;
    gfx::ComputeDesc cd;
    cd.source = source;
    cd.name = "clouds: their shadows, a nuke's ring";
    cd.entry = "ShadowMarch";
    ringShadowMarch_ = device_->CreateCompute(cd, &error);
    cd.name = "clouds: marched, a nuke's ring";
    cd.entry = "March";
    if (ringShadowMarch_) ringMarch_ = device_->CreateCompute(cd, &error);
    if (!ringMarch_) fprintf(stderr, "render: clouds: a nuke's ring cannot be drawn (%s)\n", error.c_str());
  }
}

void Clouds::Prepare(View* view, const Camera& camera, bool on) {
  prepared_ = built_ = false;
  ViewConstants& c = view->constants;
  std::fill(std::begin(c.cloudShadow), std::end(c.cloudShadow), 0.0f);
  if (!on || c.skyZenith[3] < 0.5f || c.skyClouds[0] <= 0 || !march_) {
    haveLast_ = false;
    return;
  }
  if (!noiseMade_) {  // (once: the noise volumes, and the pictures of coverage and shadows)
    gfx::TargetDesc td;
    td.storage = true;
    td.format = gfx::Format::RGBA8;
    td.width = td.height = td.depth = kShapeSize;
    shape_ = device_->CreateTarget(td);
    td.width = td.height = td.depth = kDetailSize;
    detail_ = device_->CreateTarget(td);
    td.width = td.height = kCoverSize;
    td.depth = 1;
    td.format = gfx::Format::R16F;
    cover_ = device_->CreateTarget(td);
    shadow_ = device_->CreateTarget(td);
    if (!shape_ || !detail_ || !cover_ || !shadow_) return;
    for (int which = 0; which < 2; ++which) {
      const float size[4] = {static_cast<float>(which ? kDetailSize : kShapeSize), 0, 0, 0};
      device_->SetPipeline(which ? detailNoise_ : shapeNoise_);
      device_->SetConstants(1, size, sizeof size);
      device_->SetStorage(0, which ? detail_ : shape_);
      const uint32_t groups = static_cast<uint32_t>((which ? kDetailSize : kShapeSize) / 4);
      device_->Dispatch(groups, groups, groups);
    }
    device_->SetStorage(0, {});
    noiseMade_ = true;
  }
  if (ringsWanted) PrepareRings();  // (volume explosions: the shaders with a nuke's ring, the first frame they may be wanted)
  const Vec3 eye = camera.Eye();
  coverAt_[0] = eye.x;
  coverAt_[1] = eye.z;
  // (sky) With the zoom through them (cloud_zoom.h), the pictures of where they are (the coarse one and the zoom's finer
  // one) and of their shadows stay put on the ground and move on by whole texels of both as the camera goes (3 of the
  // coarse one's, 16 of the finer one's: 93.75 world units). Laid out afresh round a moving camera, a place was read
  // between other texels each frame, and the clouds' outlines and their shadows on the ground crawled as it moved.
  if (zoom.on) {
    constexpr float kStep = 3 * kCoverReach / kCoverSize;
    static_assert(3 * kCoverReach / kCoverSize == 16 * kCloudZoomCoverReach / kCloudZoomCoverSize, "a step of whole texels of both pictures");
    coverAt_[0] = std::round(eye.x / kStep) * kStep;
    coverAt_[1] = std::round(eye.z / kStep) * kStep;
  }
  Constants k = {};
  FillConstants(&k, *view, camera, *this, base, coverAt_);
  device_->SetConstants(0, &c, sizeof c);
  device_->SetConstants(1, &k, RingNow() ? sizeof k : offsetof(Constants, ring));
  // where the clouds are round the camera
  device_->SetPipeline(coverage_);
  device_->SetStorage(0, cover_);
  device_->Dispatch(kCoverSize / 8, kCoverSize / 8);
  device_->SetStorage(0, {});
  // their shadows, at their underside over the ground's zero
  device_->SetPipeline(RingNow() ? ringShadowMarch_ : shadowMarch_);
  device_->SetTexture(0, shape_);
  device_->SetTexture(1, detail_);
  device_->SetTexture(4, cover_);
  device_->SetStorage(2, shadow_);
  device_->Dispatch(kCoverSize / 8, kCoverSize / 8);
  device_->SetStorage(2, {});
  for (int slot : {0, 1, 4}) device_->SetTexture(slot, {});
  c.cloudShadow[0] = base;
  c.cloudShadow[1] = coverAt_[0];
  c.cloudShadow[2] = coverAt_[1];
  c.cloudShadow[3] = 1 / kCoverReach;
  prepared_ = true;
}

void Clouds::Build(const View& view, const Camera& camera, const SkyLight& sky, gfx::Texture depth) {
  built_ = false;
  if (!prepared_ || !depth) return;
  if (!Size(view.width, view.height)) return;
  const ViewConstants& c = view.constants;
  Constants k = {};
  FillConstants(&k, view, camera, *this, base, coverAt_);
  // (cloudzoom) the zoom through them (cloud_zoom.h), once the eye is no more than 40 under the layer (lower, the
  // layer was 40 over it as without the zoom, and the march is that one); its march made the first frame it is on
  if (zoom.on && !zoomTried_) {
    zoomTried_ = true;
    marchZoom_ = MakeCloudZoomMarch(device_, std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kShader);
  }
  zoomedNow_ = zoom.on && marchZoom_ && camera.Eye().y > base - 40;
  const CloudZoomMarchNumbers zoomNumbers = zoomedNow_ ? CloudZoomNumbers(zoom, base, camera, lastEye_, view.width, view.height) : CloudZoomMarchNumbers{};
  if (zoomedNow_) {  // the finer coverage picture round the camera that its march reads first (the coverage's own pass)
    if (!coverFine_) {
      gfx::TargetDesc td;
      td.storage = true;
      td.width = td.height = kCloudZoomCoverSize;
      td.format = gfx::Format::R16F;
      coverFine_ = device_->CreateTarget(td);
    }
    if (coverFine_) {
      Constants fine = k;
      fine.size[2] = 1 / kCloudZoomCoverReach;
      device_->SetConstants(0, &c, sizeof c);
      device_->SetConstants(1, &fine, sizeof fine);
      device_->SetPipeline(coverage_);
      device_->SetStorage(0, coverFine_);
      device_->Dispatch(kCloudZoomCoverSize / 8, kCloudZoomCoverSize / 8);
      device_->SetStorage(0, {});
    } else {
      zoomedNow_ = false;
    }
  }
  k.ahead[3] = std::fmod(static_cast<float>(frame_) * 0.6180339887f, 1.0f);
  k.size[0] = static_cast<float>(width_);
  k.size[1] = static_cast<float>(height_);
  k.size[3] = haveLast_ ? std::clamp(zoomedNow_ ? zoom.history : keep, 0.0f, 0.97f) : 0.0f;
  k.lastViewProj = lastViewProj_;
  k.light[0] = sky.cloudSun.x;
  k.light[1] = sky.cloudSun.y;
  k.light[2] = sky.cloudSun.z;
  if (boltLight.x + boltLight.y + boltLight.z > 0) {  // a lightning's flash: in the layer, little of the frame before kept (it flickers)
    k.bolt[0] = boltAt.x;
    k.bolt[1] = boltAt.y;
    k.bolt[2] = boltAt.z;
    k.bolt[3] = std::max(boltReach, 1.0f);
    k.boltLight[0] = boltLight.x;
    k.boltLight[1] = boltLight.y;
    k.boltLight[2] = boltLight.z;
    k.size[3] = std::min(k.size[3], 0.3f);
  }
  device_->SetConstants(0, &c, sizeof c);
  device_->SetConstants(1, &k, RingNow() ? sizeof k : offsetof(Constants, ring));
  const int last = now_;
  now_ = 1 - now_;
  device_->SetPipeline(RingNow() ? ringMarch_ : march_);
  device_->SetTexture(0, shape_);
  device_->SetTexture(1, detail_);
  device_->SetTexture(2, marched_[last]);
  device_->SetTexture(3, depth);
  device_->SetTexture(4, cover_);
  if (zoomedNow_) {  // (cloudzoom) the zoom's march in place of that one, with its own numbers and the finer coverage
    device_->SetPipeline(marchZoom_);
    device_->SetConstants(2, &zoomNumbers, sizeof zoomNumbers);
    device_->SetTexture(5, coverFine_);
  }
  device_->SetStorage(1, marched_[now_]);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8));
  device_->SetStorage(1, {});
  for (int slot = 0; slot < 5; ++slot) device_->SetTexture(slot, {});
  if (zoomedNow_) device_->SetTexture(5, {});
  lastViewProj_ = c.viewProj;
  lastEye_ = camera.Eye();  // (cloudzoom)
  haveLast_ = true;
  ++frame_;
  built_ = true;
}

static void FillConstants(void* to, const View& view, const Camera& camera, const Clouds& clouds, float under, const float coverAt[2]) {
  Constants& k = *static_cast<Constants*>(to);
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float t = std::tan(camera.fovY * 0.5f), aspect = static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1));
  const Vec3 r = right * (t * aspect), u = up * t, a = back * -1.0f;
  const float r4[4] = {r.x, r.y, r.z, clouds.base}, u4[4] = {u.x, u.y, u.z, std::max(clouds.thickness, 1.0f)}, a4[4] = {a.x, a.y, a.z, 0};
  std::memcpy(k.right, r4, sizeof k.right);
  std::memcpy(k.up, u4, sizeof k.up);
  std::memcpy(k.ahead, a4, sizeof k.ahead);
  k.size[2] = 1 / kCoverReach;
  k.look[0] = std::max(clouds.density, 0.0f);
  k.look[1] = static_cast<float>(std::clamp(clouds.steps, 4, 256));
  k.look[2] = 1 / std::max(clouds.shapeSize, 1.0f);
  k.look[3] = 1 / std::max(clouds.detailSize, 1.0f);
  k.coverAt[0] = coverAt[0];
  k.coverAt[1] = coverAt[1];
  k.coverAt[2] = under;
  k.ring[0] = clouds.ringX;  // (volume explosions: a nuke's ring, read by the ring's shaders only)
  k.ring[1] = clouds.ringZ;
  k.ring[2] = clouds.ringRadius;
  k.ring[3] = std::clamp(clouds.ringStrength, 0.0f, 1.0f);
  k.ringLook[0] = std::max(clouds.ringWidth, 1.0f);
}

}  // namespace render
