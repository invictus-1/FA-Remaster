#include "air.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "lighting.h"
#include "lights.h"

namespace render {
namespace {

// What both shaders are told (b1): the camera's axes for the cells' rays, the volume's size and range,
// last frame's view for the blending, and the strengths.
struct AirConstants {
  float right[4];      // xyz: the camera's right x tan(half the view's width)
  float up[4];         // xyz: up x tan(half the view's height); w: where in each cell's depth this frame samples (0..1)
  float ahead[4];      // xyz: forward; w: how much of last frame's a cell keeps (0: none)
  float cells[4];      // cells across, down, deep
  float range[4];      // x the nearest distance, y log(farthest / nearest), z the air's own thickness, w the lamps' light in it
  Mat4 lastViewProj;   // last frame's
  float lastEye[4];    // xyz: last frame's eye; w: last frame's log(farthest / nearest)
  float light[4];      // x the sun's forward scattering, y the lamps', z the sun's light in the air, w the sky's
  float lastRange[4];  // x last frame's nearest distance; y the height the haze and dust thin above; z 1 / their scale height; w the share of a lamp's glow to all sides the air takes
  float points[4];     // x the other small lights' (flashes, glows) light in the air
};

const char kShader[] = R"hlsl(
cbuffer Air : register(b1) {
  float4 airRight;
  float4 airUp;
  float4 airAhead;
  float4 airCells;
  float4 airRange;
  row_major float4x4 lastViewProj;
  float4 lastEye;
  float4 airLight;
  float4 lastRange;
  float4 airPoints;
};

// Henyey-Greenstein: how much of the light goes off at an angle whose cosine is `c` (g > 0: forward).
float AirPhase(float c, float g) {
  float g2 = g * g;
  return (1 - g2) / (4 * 3.14159265 * pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5));
}
float CellDistance(float t) { return airRange.x * exp(t * airRange.y); }  // (t: 0 .. 1 along the volume's depth)
float3 CellRay(float2 uv) { return normalize(airAhead.xyz + airRight.xyz * (uv.x * 2 - 1) + airUp.xyz * (1 - uv.y * 2)); }

// How thick the air is at a place (the share of light lost per world unit): its own dust and the haze of
// the weather, thinning with height above the ground, and the mist on low ground in its drifting banks (as
// view.h's MistLost has it).
float Thickness(float3 p, out float dust) {
  float thins = exp(-max(p.y - lastRange.y, 0) * lastRange.z);
  dust = airRange.z * thins;
  float t = (airRange.z + weather.z) * thins;
  [branch] if (mist.x > 0) {
    float2 q = p.xz + skyClouds.yz * 0.12;
    float banks = smoothstep(0.28, 0.8, CloudNoise(float3(q * 0.008, skyClouds.w * 2.5 + 11.0)) * 0.5 + CloudNoise(float3(q.yx * 0.021 + 7.3, skyClouds.w * 4.0 + 3.0)) * 0.3 +
                                        CloudNoise(float3(q * 0.057 + 2.9, skyClouds.w * 6.0 + 5.0)) * 0.2) * 1.5;
    t += mist.x * exp(-max((p.y - mist.y) * mist.z, -1)) * banks;
  }
  return t;
}

// The sun's shadow at a place in the air (one reading of the nearest map that holds it).
float SunInAir(float3 p) {
  if (shadowsOn < 0.5) return 1;
  for (int i = 0; i < int(cascadeCount); ++i) {
    float3 s = mul(shadowMatrix[i], float4(p, 1)).xyz;
    if (all(s.xy > 0.0) && all(s.xy < 1.0) && s.z > 0 && s.z < 1) return shadowMaps.SampleCmpLevelZero(samplerShadow, float3(s.xy, i), s.z);
  }
  return 1;
}

// The sky's light from all sides (half from above, half from the ground under it).
float3 SkyInAir() {
  [branch] if (skyZenith.w > 0.5) return (SkyAmbient(float3(0, 1, 0)) + SkyAmbient(float3(0, -1, 0))) * 0.5;
  return (ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, 1, 0), 8).rgb) + ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, -1, 0), 8).rgb)) * 0.5;
}

// The small lights at a place in the air, toward the eye along `ray` (as lighting.h's SmallLights
// for a surface: the grid square under the place, the beams' cones and shadows): the lamps' beams, and
// apart from them the lights to all sides (flashes of explosions, glows).
float3 LampsInAir(float3 p, float3 ray, out float3 points) {
  float3 sum = 0;
  points = 0;
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), n = min(uint(square.y), 48u);
      for (uint s = 0; s < n; ++s) {
        uint at = first + s;
        uint light = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + light * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float3 l = normalize(toLight);
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 tint = lightData[uint(lightGridSize.z) + light * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + light * 4 + 2];
        if (beam.w < -1.5) {  // a light to all sides
          points += tint.rgb * (fade * 3.14159265 * AirPhase(dot(l, ray), airLight.y));
          continue;
        }
        {  // a beam: its cone, its shadow; its glow to all sides near the lamp
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + light * 4 + 3].x;
          float reaches = inside;
          [branch] if (map > -0.5 && inside > 0) reaches *= LampShadow(p, float3(0, 0, 0), uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * tint.w * saturate(1 - far2 * 25) * lastRange.w;
        }
        sum += tint.rgb * (fade * 3.14159265 * AirPhase(dot(l, ray), airLight.y));
      }
    }
  }
  return sum;
}

// (both shaders' input and output: last frame's cells and this frame's for Gather, the gathered cells and
// the sums for Sum)
Texture3D airSource : register(t0);
RWTexture3D<float4> airTarget : register(u0);

[numthreads(8, 8, 1)] void Gather(uint3 id : SV_DispatchThreadID) {
  if (any(id >= uint3(airCells.xyz))) return;
  float2 uv = (id.xy + 0.5) / airCells.xy;
  float3 ray = CellRay(uv);
  float3 p = cameraPosition + ray * CellDistance((id.z + airUp.w) / airCells.z);
  float dust;
  float thick = Thickness(p, dust);
  float3 scattered = 0;
  [branch] if (thick > 0) {
    float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x * 3.14159265) * AirPhase(dot(sunDirection, ray), airLight.x) * SunInAir(p) * CloudShadow(p);
    // (the lamps' light strengthened on the clear air's dust alone: in mist and fog a beam shows by itself)
    float3 points;
    float3 beams = LampsInAir(p, ray, points);
    scattered = (sun * airLight.z + SkyInAir() * (modern.y * airLight.w)) * thick + beams * (thick + dust * max(airRange.w - 1, 0)) +
                points * (thick + dust * max(airPoints.x - 1, 0));
  }
  float4 now = float4(scattered, thick);
  // what the cell held last frame, read where its middle was then
  [branch] if (airAhead.w > 0) {
    float3 middle = cameraPosition + ray * CellDistance((id.z + 0.5) / airCells.z);
    float4 c = mul(lastViewProj, float4(middle, 1));
    if (c.w > 1e-3) {
      float2 lastUv = c.xy / c.w * float2(0.5, -0.5) + 0.5;
      float lastT = log(max(length(middle - lastEye.xyz), lastRange.x) / lastRange.x) / lastEye.w;
      if (all(lastUv >= 0) && all(lastUv <= 1) && lastT <= 1) now = lerp(now, airSource.SampleLevel(samplerLinearClamp, float3(lastUv, lastT), 0), airAhead.w);
    }
  }
  airTarget[id] = now;
}

// Along each column from the eye outward: the light each cell throws toward the eye, dimmed by the air
// before it (what a cell of even thickness and light gives over its length, so a thick cell is not too bright).
[numthreads(8, 8, 1)] void Sum(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= uint2(airCells.xy))) return;
  float3 light = 0;
  float through = 1, start = 0;
  uint depth = uint(airCells.z);
  for (uint s = 0; s < depth; ++s) {
    float end = CellDistance((s + 1) / airCells.z);
    float4 g = airSource.Load(int4(id.xy, s, 0));
    float thick = max(g.a, 1e-7);
    float cell = exp(-thick * (end - start));
    light += through * (g.rgb / thick) * (1 - cell);
    through *= cell;
    airTarget[uint3(id.xy, s)] = float4(light, through);
    start = end;
  }
}
)hlsl";

}  // namespace

Air::~Air() {
  for (gfx::Texture t : {cells_[0], cells_[1], summed_})
    if (t) device_->Destroy(t);
}

bool Air::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
  gfx::ComputeDesc cd;
  cd.source = source;
  cd.name = "air: gather";
  cd.entry = "Gather";
  gather_ = device_->CreateCompute(cd, error);
  if (!gather_) return false;
  cd.name = "air: sum";
  cd.entry = "Sum";
  sum_ = device_->CreateCompute(cd, error);
  return static_cast<bool>(sum_);
}

bool Air::Size(int width, int height) {
  const int w = std::clamp(across, 16, 512);
  const int h = std::clamp(static_cast<int>(std::lround(static_cast<float>(w) * static_cast<float>(height) / static_cast<float>(std::max(width, 1)))), 8, 512);
  const int d = std::clamp(deep, 8, 256);
  if (w == width_ && h == height_ && d == depth_ && summed_) return true;
  for (gfx::Texture& t : cells_) {
    if (t) device_->Destroy(t);
    t = {};
  }
  if (summed_) device_->Destroy(summed_);
  gfx::TargetDesc td;
  td.storage = true;
  td.width = w;
  td.height = h;
  td.depth = d;
  td.format = gfx::Format::RGBA16F;
  cells_[0] = device_->CreateTarget(td);
  cells_[1] = device_->CreateTarget(td);
  summed_ = device_->CreateTarget(td);
  width_ = w;
  height_ = h;
  depth_ = d;
  haveLast_ = false;
  return cells_[0] && cells_[1] && summed_;
}

void Air::Build(View* view, const Camera& camera, gfx::Texture sunShadows, gfx::Texture lampShadows, Lights* lights, bool on, float floor) {
  ViewConstants& c = view->constants;
  std::fill(std::begin(c.air), std::end(c.air), 0.0f);
  built_ = false;
  if (!on || !gather_ || !sum_) {
    haveLast_ = false;
    return;
  }
  if (!Size(view->width, view->height)) return;
  const float closest = std::max(nearest, 0.05f), farthestNow = std::max(farthest, closest * 2);
  AirConstants k = {};
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float t = std::tan(camera.fovY * 0.5f), aspect = static_cast<float>(view->width) / static_cast<float>(std::max(view->height, 1));
  const Vec3 r = right * (t * aspect), u = up * t, a = back * -1.0f;
  const float r4[4] = {r.x, r.y, r.z, 0}, u4[4] = {u.x, u.y, u.z, 0}, a4[4] = {a.x, a.y, a.z, 0};
  std::memcpy(k.right, r4, sizeof k.right);
  std::memcpy(k.up, u4, sizeof k.up);
  std::memcpy(k.ahead, a4, sizeof k.ahead);
  // this frame's place inside each cell's depth: the golden ratio's steps, which spread evenly over frames
  k.up[3] = std::fmod(static_cast<float>(frame_) * 0.6180339887f, 1.0f);
  k.ahead[3] = haveLast_ ? std::clamp(keep, 0.0f, 0.98f) : 0.0f;
  k.cells[0] = static_cast<float>(width_);
  k.cells[1] = static_cast<float>(height_);
  k.cells[2] = static_cast<float>(depth_);
  k.range[0] = closest;
  k.range[1] = std::log(farthestNow / closest);
  k.range[2] = std::max(dust, 0.0f);
  k.range[3] = std::max(lampsInAir, 0.0f);
  k.lastViewProj = lastViewProj_;
  const Vec3 eye = camera.Eye();
  k.lastEye[0] = lastEye_.x;
  k.lastEye[1] = lastEye_.y;
  k.lastEye[2] = lastEye_.z;
  k.lastEye[3] = lastFarthest_ > lastNearest_ && lastNearest_ > 0 ? std::log(lastFarthest_ / lastNearest_) : k.range[1];
  k.lastRange[0] = lastNearest_ > 0 ? lastNearest_ : closest;
  k.lastRange[1] = floor;
  k.lastRange[2] = 1 / std::max(thinsOver, 1.0f);
  k.lastRange[3] = std::max(lampGlowInAir, 0.0f);
  k.points[0] = std::max(pointsInAir, 0.0f);
  k.light[0] = std::clamp(sunForward, -0.95f, 0.95f);
  k.light[1] = std::clamp(lampForward, -0.95f, 0.95f);
  k.light[2] = std::max(sunInAir, 0.0f);
  k.light[3] = std::max(skyInAir, 0.0f);

  const gfx::Texture last = cells_[1 - now_], gathered = cells_[now_];
  device_->SetPipeline(gather_);
  device_->SetConstants(0, &view->constants, sizeof view->constants);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, last);
  device_->SetTexture(kShadowMapSlot, sunShadows);
  device_->SetTexture(kLampShadowSlot, lampShadows);
  if (lights) lights->Bind();
  device_->SetStorage(0, gathered);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8), static_cast<uint32_t>(depth_));
  device_->SetPipeline(sum_);
  device_->SetTexture(0, gathered);
  device_->SetStorage(0, summed_);
  device_->Dispatch(static_cast<uint32_t>((width_ + 7) / 8), static_cast<uint32_t>((height_ + 7) / 8), 1);
  device_->SetStorage(0, {});
  device_->SetTexture(0, {});

  lastViewProj_ = view->constants.viewProj;
  lastEye_ = eye;
  lastNearest_ = closest;
  lastFarthest_ = farthestNow;
  haveLast_ = true;
  now_ = 1 - now_;
  ++frame_;
  built_ = true;
  c.air[0] = 1;
  c.air[1] = closest;
  c.air[2] = k.range[1];
  c.air[3] = std::clamp(std::max(c.weather[3], c.mist[3]) + 0.1f, 0.0f, 1.0f);  // (the most the air may hide: a little more than the formulas allowed)
}

}  // namespace render
