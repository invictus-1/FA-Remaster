#include "weather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lighting.h"

namespace render {
namespace {

// Six corners per drop, no vertex data: a drop's number gives its place in the box, the time moves
// it along its fall, and it comes in again at the far side when it leaves the box.
const char kShader[] = R"hlsl(
cbuffer Fall : register(b1) {
  float4 box;       // xyz: the box's middle; w: its edge
  float4 velocity;  // xyz: how it falls, world units a second; w: a drop's length in seconds of fall (0: a flake)
  float4 shape;     // x: half its width in pixels; y: world units per pixel at distance 1; z: how see-through; w: how much it sways
  float4 tint;      // its colour, linear
#if LIT
  float4 lit;       // x: how much of the light on it a drop or flake sends on (on top of the sky's, `tint`); y 1: the
                    // layer near the ground, seen only where the small lights light it; z: that layer's height
#endif
};
#if LIT
#define BOX_SIZE (lit.z > 0 ? float3(box.w, lit.z, box.w) : box.www)  // (the layer near the ground: flat)
#else
#define BOX_SIZE box.w
#endif

struct VOut {
  float4 position : SV_Position;
  float2 across : TEXCOORD0;  // x: -1..1 across; y: 0..1 along
  float fade : TEXCOORD1;
#if LIT
  float3 light : TEXCOORD2;  // its colour, lit where it is
  float cover : TEXCOORD3;   // how much of it shows (the layer near the ground: by how lit it is)
#endif
};

#if LIT
// How much a drop sends toward the eye of light going `along`, by the angle to the way on to the eye: drops send light
// mostly onward (Henyey-Greenstein, g 0.6; 1 on average over all ways).
float DropPhase(float3 along, float3 toEye) {
  const float g = 0.6;
  return (1 - g * g) / pow(max(1 + g * g - 2 * g * dot(along, toEye), 1e-4), 1.5);
}
// The light on a drop or flake where it is (lighting.h): the sun's through its shadow and the clouds' (returned), the
// small lights' (`small`; lights.h: as SmallLights, a lamp's beam through the lamp's shadow), each scattered toward
// the eye.
float3 DropLight(float3 p, float3 toEye, out float3 small) {
  const float3 up = float3(0, 1, 0);
  small = 0;
  float3 light = ToLinear(sunColor) * (lightMultiplier * modern.x) * (SunShadow(p, up) * CloudShadow(p) * DropPhase(-sunDirection, toEye));
  [branch] if (lightGrid.w > 0.5) {
    float2 g = (p.xz - lightGrid.xy) * lightGrid.z;
    [branch] if (all(g >= 0) && all(g < lightGridSize.xy)) {
      float4 square = lightData[uint(g.y) * uint(lightGridSize.x) + uint(g.x)];
      uint first = uint(square.x), count = min(uint(square.y), 48u);
      for (uint s = 0; s < count; ++s) {
        uint at = first + s;
        uint which = uint(lightData[at >> 2][at & 3]);
        float4 where = lightData[uint(lightGridSize.z) + which * 4];
        float3 toLight = where.xyz - p;
        float far2 = dot(toLight, toLight) / (where.w * where.w);
        if (far2 >= 1) continue;
        float3 l = normalize(toLight);
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 colour = lightData[uint(lightGridSize.z) + which * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + which * 4 + 2];
        if (beam.w > -1.5) {  // a beam: within its cone, through its lamp's shadow (and a share round the lamp)
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float inside = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + which * 4 + 3].x;
          float reaches = inside;
          [branch] if (map > -0.5 && inside > 0) reaches *= LampShadow(p, l, uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * colour.w * saturate(1 - far2 * 25);
        }
        small += colour.rgb * (fade * DropPhase(-l, toEye));
      }
    }
  }
  return light;
}
#endif

float Hash1(float n) { return frac(sin(n * 12.9898) * 43758.5453); }

VOut VS(uint id : SV_VertexID) {
  uint drop = id / 6, corner = id % 6;
  float n = float(drop);
  float3 start = float3(Hash1(n + 0.13), Hash1(n * 1.7 + 4.1), Hash1(n * 2.3 + 9.7)) * BOX_SIZE;
  float3 moved = start + velocity.xyz * (time * (0.8 + 0.4 * Hash1(n + 7.7)));
  // a flake wanders sideways as it falls
  moved.xz += shape.w * float2(sin(time * (0.7 + Hash1(n + 2.2)) + n), cos(time * (0.6 + Hash1(n + 3.3)) + n * 1.3));
  float3 local = frac((moved - box.xyz) / BOX_SIZE + 0.5);
  float3 p = box.xyz + (local - 0.5) * BOX_SIZE;

  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  float4 clip = mul(viewProj, float4(p, 1));
  float pixel = shape.y * max(clip.w, 0.01);  // world units a pixel covers there
  float3 toEye = normalize(cameraPosition - p);
  float3 along, side;
  if (velocity.w > 0) {  // a streak along its fall, at least three pixels long
    float3 fallen = velocity.xyz * velocity.w;
    along = normalize(fallen) * max(length(fallen), pixel * 3);
    side = normalize(cross(along, toEye)) * (shape.x * pixel);
  } else {  // a flake: a small square toward the eye
    side = normalize(cross(float3(0, 1, 0), toEye)) * (shape.x * pixel);
    along = normalize(cross(toEye, side)) * (shape.x * pixel * 2);
  }
  VOut o;
  o.position = mul(viewProj, float4(p + side * uv.x - along * uv.y, 1));
  o.across = uv;
  // nothing at the box's sides, so that no drop is seen coming in or going out
  float3 edge = abs(local - 0.5) * 2;
  o.fade = saturate((1 - max(edge.x, max(edge.y, edge.z))) * 6) * (0.5 + 0.5 * Hash1(n + 5.5));
#if LIT
  float3 small;
  float3 sun = DropLight(p, toEye, small);
  if (lit.y > 0.5) {  // the layer near the ground: only the small lights' light, shown by how much it outshines the sky's
    o.light = small * lit.x;
    o.cover = saturate(dot(o.light, float3(0.3, 0.6, 0.1)) / max(dot(tint.rgb, float3(0.3, 0.6, 0.1)) * 4, 1e-5));
  } else {  // on top of the sky's light it had
    o.light = tint.rgb + (sun + small) * lit.x;
    o.cover = 1;
  }
#endif
  return o;
}

float4 PS(VOut i) : SV_Target {
  float soft = velocity.w > 0 ? (1 - abs(i.across.x)) * saturate(i.across.y * 4) * saturate((1 - i.across.y) * 1.5)
                              : saturate(1 - length(float2(i.across.x, i.across.y * 2 - 1)));
#if LIT
  return float4(i.light, tint.a * shape.z * i.fade * soft * i.cover);
#else
  return float4(tint.rgb, tint.a * shape.z * i.fade * soft);
#endif
}
)hlsl";

constexpr float kLowLayer = 12;  // (lit drops: how high their layer near the ground is, world units)

struct FallConstants {
  float box[4], velocity[4], shape[4], tint[4];
  float lit[4];  // (the lit shader's only)
};

}  // namespace

namespace {

// A number 0..1 from whole numbers.
float Draw01(uint32_t seed, uint32_t a, uint32_t b) {
  uint32_t h = seed * 0x9e3779b9u + a * 0x85ebca6bu + b * 0xc2b2ae35u + 0x27d4eb2fu;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// The kinds of weather, and how often each comes in a climate (shares of 100).
struct Kind {
  WeatherNow weather;
  int temperate, cold, dry;
};
const Kind kKinds[] = {
    {{0.10f, 0, 0, 0, 0}, 22, 15, 45},     // clear
    {{0.35f, 0, 0, 0, 0}, 26, 20, 35},     // fair
    {{0.65f, 0, 0, 0, 0}, 16, 18, 12},     // cloudy
    {{0.92f, 0, 0, 0.1f, 0}, 8, 12, 0},    // overcast
    {{0.85f, 0.5f, 0, 0, 0}, 14, 0, 0},    // rain
    {{1.0f, 1.0f, 0, 0.25f, 0}, 6, 0, 4},  // storm
    {{0.5f, 0, 0, 1.0f, 0}, 8, 10, 4},     // fog
    {{0.9f, 0, 0.8f, 0.1f, 0}, 0, 25, 0},  // snow
};

const WeatherNow& SpellWeather(uint32_t seed, int64_t spell, Climate climate) {
  if (spell <= 0) return kKinds[1].weather;
  float left = Draw01(seed, static_cast<uint32_t>(spell), 17) * 100;
  for (const Kind& k : kKinds) {
    left -= static_cast<float>(climate == Climate::Cold ? k.cold : climate == Climate::Dry ? k.dry : k.temperate);
    if (left < 0) return k.weather;
  }
  return kKinds[1].weather;
}

}  // namespace

WeatherNow WeatherAt(uint32_t seed, double seconds, Climate climate, float spell) {
  spell = std::max(spell, 1.0f);
  const double at = std::max(seconds, 0.0) / spell;
  const int64_t index = static_cast<int64_t>(at);
  const WeatherNow& a = SpellWeather(seed, index, climate);
  const WeatherNow& b = SpellWeather(seed, index + 1, climate);
  float t = std::clamp((static_cast<float>(at - static_cast<double>(index)) - 0.75f) * 4, 0.0f, 1.0f);
  t = t * t * (3 - 2 * t);
  WeatherNow w;
  w.clouds = a.clouds + (b.clouds - a.clouds) * t;
  w.rain = a.rain + (b.rain - a.rain) * t;
  w.snow = a.snow + (b.snow - a.snow) * t;
  w.fog = a.fog + (b.fog - a.fog) * t;
  w.flash = LightningAt(seed, seconds, w.rain);
  return w;
}

float WeatherDraw(uint32_t seed, uint32_t a, uint32_t b) { return Draw01(seed, a, b); }

float LightningAt(uint32_t seed, double seconds, float rain, uint32_t* stroke) {
  if (rain < 0.7f || seconds < 0) return 0;
  // time in stretches of seven seconds; a stretch may have one stroke, the likelier the harder it rains
  // (in a full storm about one in three stretches, a stroke every 22 seconds or so: it was four in
  // five, and Chris found that too often, 2026-10-13)
  float flash = 0;
  const int64_t now = static_cast<int64_t>(seconds / 7);
  for (int64_t stretch = now - 1; stretch <= now; ++stretch) {
    if (stretch < 0) continue;
    const uint32_t n = static_cast<uint32_t>(stretch);
    if (Draw01(seed, n, 31) > (rain - 0.6f) * 0.8f) continue;
    const double struck = static_cast<double>(stretch) * 7 + Draw01(seed, n, 47) * 6.5;
    const float since = static_cast<float>(seconds - struck);
    if (since < 0 || since > 0.6f) continue;
    // a stroke flickers: bright at once, twice more, and gone
    const float flicker = 0.55f + 0.45f * std::cos(since * 46);
    const float bright = std::exp(-since * 7) * flicker * (0.6f + 0.4f * Draw01(seed, n, 59));
    if (bright > flash) {
      flash = bright;
      if (stroke) *stroke = n;
    }
  }
  return flash;
}

gfx::Pipeline Precipitation::Make(bool lit, std::string* error) {
  std::string source = lit ? std::string("#define LIT 1\n") + kViewShaderBlock + kLightingShaderBlock + kShader : std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = lit ? "precipitation, lit where it falls" : "precipitation";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  return device_->CreatePipeline(pd, error);
}

bool Precipitation::Init(std::string* error) {
  pipeline_ = Make(false, error);
  return static_cast<bool>(pipeline_);
}

void Precipitation::Draw(const View& view, const Camera& camera, float rain, float snow, bool lit) {
  if ((rain <= 0 && snow <= 0) || view.height <= 0) return;
  if (lit && !litTried_) {  // (the lit shader the first time it is wanted; where it cannot be made, unlit as before)
    litTried_ = true;
    std::string why;
    litPipeline_ = Make(true, &why);
    if (!litPipeline_) fprintf(stderr, "render: rain and snow lit where they fall cannot be used (%s)\n", why.c_str());
  }
  lit = lit && litPipeline_;
  const ViewConstants& c = view.constants;
  // the air's light on what falls: the sky's, and some of the falling light
  const float light[3] = {c.skyZenith[0] * 0.6f + c.skyHorizon[0] * 0.6f, c.skyZenith[1] * 0.6f + c.skyHorizon[1] * 0.6f,
                          c.skyZenith[2] * 0.6f + c.skyHorizon[2] * 0.6f};
  const float edge = std::clamp(camera.distance * 1.6f, 30.0f, 900.0f);
  const float perPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  device_->SetPipeline(lit ? litPipeline_ : pipeline_);
  device_->SetConstants(0, &c, sizeof c);
  // `low`: the lit drops' layer near the ground (seen only where the small lights light it)
  auto draw = [&](float amount, bool flakes, bool low) {
    FallConstants f{};
    f.box[0] = camera.target.x;
    f.box[1] = low ? camera.target.y + kLowLayer * 0.5f : camera.target.y + edge * 0.25f;
    f.box[2] = camera.target.z;
    f.box[3] = low ? std::clamp(edge * 0.5f, 30.0f, 200.0f) : edge;
    // (the fall is as fast as the box is large: seen from far it would otherwise stand still)
    const float speed = flakes ? std::max(1.2f, edge * 0.05f) : std::max(9.0f, edge * 0.55f);
    f.velocity[0] = speed * (flakes ? 0.25f : 0.18f);
    f.velocity[1] = -speed;
    f.velocity[2] = speed * 0.08f;
    f.velocity[3] = flakes ? 0.0f : 0.05f;
    f.shape[0] = flakes ? 1.6f : 0.9f;
    f.shape[1] = perPixel;
    f.shape[2] = flakes ? 0.8f : 0.2f;
    f.shape[3] = flakes ? edge * 0.01f : 0.0f;
    const float bright = flakes ? 2.0f : 1.2f;
    f.tint[0] = light[0] * bright;
    f.tint[1] = light[1] * bright;
    f.tint[2] = light[2] * bright;
    f.tint[3] = 1;
    // how much of the light on it goes on to the eye (snow is white); near the ground three times that, as a glint: a
    // thin drop in a beam is seen against the patch of ground the beam lights, and would hardly show (Chris to judge)
    f.lit[0] = (flakes ? 1.0f : 0.6f) * (low ? 3.0f : 1.0f);
    f.lit[1] = low ? 1.0f : 0.0f;
    f.lit[2] = low ? kLowLayer : 0.0f;
    device_->SetConstants(1, &f, sizeof f);
    const uint32_t drops = static_cast<uint32_t>((low ? 6000.0f : flakes ? 12000.0f : 14000.0f) * std::clamp(amount, 0.0f, 1.0f));
    if (drops) device_->Draw(drops * 6);
  };
  if (rain > 0) draw(rain, false, false);
  if (snow > 0) draw(snow, true, false);
  if (lit && rain > 0) draw(rain, false, true);
  if (lit && snow > 0) draw(snow, true, true);
}

}  // namespace render
