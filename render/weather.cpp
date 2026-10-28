#include "weather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lighting.h"

namespace render {
namespace {

// The light on a drop, a flake or a splash where it is (the lit rain's and the splashes' shaders take it, after the
// view's and the lighting's blocks).
const char kDropLight[] = R"hlsl(
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
)hlsl";

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

// Splashes (Precipitation::DrawSplashes): each a place on the picture by its number and the time, its depth back to
// the world; where that faces up (the ground, a unit's top), a small crown toward the eye that rises and fades in its
// life, then comes again somewhere else. Nothing where the picture shows the sky or a slope.
const char kSplashShader[] = R"hlsl(
cbuffer Splashes : register(b1) {
  row_major float4x4 back;  // the picture's -1..1 and depth back to the world (the view's inverse)
  float4 splash;            // x: a splash's life, seconds; y: its size, world units; z: world units a pixel covers at
                            // distance 1; w: how far from the eye they are seen
  float4 tint;              // rgb: the sky's light on them (the rain's); a: how much they show
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
float DepthAt(int2 px) { return sceneDepth.Load(px, 0); }
#else
Texture2D<float> sceneDepth : register(t3);
float DepthAt(int2 px) { return sceneDepth.Load(int3(px, 0)); }
#endif
float3 WorldAt(int2 px) {
  float2 uv = (px + 0.5) * viewport.zw;
  float4 w = mul(back, float4(uv.x * 2 - 1, 1 - uv.y * 2, DepthAt(px), 1));
  return w.xyz / w.w;
}
// A number 0..1 from two whole numbers (by integers alone, so every backend puts a splash in the same place: a hash
// through sin() rounds differently on each).
float SplashHash(uint a, uint b) {
  uint x = a * 0x9e3779b9u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return float(x >> 8) * (1.0 / 16777216.0);
}

struct SOut {
  float4 position : SV_Position;
  float2 across : TEXCOORD0;  // x -1..1 across, y 0..1 up
  float show : TEXCOORD1;
  float3 light : TEXCOORD2;
};

SOut SplashVS(uint id : SV_VertexID) {
  uint n = id / 6, corner = id % 6;
  SOut o;
  o.position = float4(0, 0, -1, 1);  // (none: behind the eye)
  o.across = 0;
  o.show = 0;
  o.light = 0;
  float life = splash.x * (0.75 + 0.5 * SplashHash(n, 1));
  float t = time / life + SplashHash(n, 2);
  float round = floor(t), age = frac(t);
  uint r = uint(round) * 2 + 3;
  int2 px = int2(float2(SplashHash(n, r), SplashHash(n, r + 1)) * viewport.xy);
  px = clamp(px, 0, int2(viewport.xy) - 3);
  if (DepthAt(px) <= 0) return o;  // (the sky: reversed depth)
  float3 p = WorldAt(px);
  float3 nrm = cross(WorldAt(px + int2(0, 2)) - p, WorldAt(px + int2(2, 0)) - p);
  float3 toEye = cameraPosition - p;
  float far = length(toEye);
  toEye /= far;
  if (dot(nrm, toEye) < 0) nrm = -nrm;
  nrm = normalize(nrm);
  if (nrm.y < 0.75 || far > splash.w) return o;  // (a slope, a wall, or too far to see)
  float pixel = splash.z * far;
  // (a real splash is far smaller than a pixel from the game's camera: a speck of a pixel or two, a little taller
  // than wide, up at once and gone)
  float size = max(splash.y, pixel * 1.2);
  // (photofix) Near the eye a splash is many pixels wide, and as many splashes fall in each part of the picture as far
  // off: so many large ones near a low camera that they cover it (white arcs over the whole picture). One larger than
  // four pixels is shown only as often as keeps the share of the picture splashes cover as it is at four (the same
  // number to each piece of ground as it nears), the rest left out; four pixels and smaller, as before.
  float across = size / pixel;
  if (across > 4 && SplashHash(n ^ 0x9e3779b9u, r) > 16 / (across * across)) return o;
  float3 side = cross(float3(0, 1, 0), toEye);
  side = dot(side, side) > 1e-10 ? normalize(side) : float3(1, 0, 0);  // (straight under the eye: any way across)
  // (photofix) and such a large one seen from above is turned toward the eye, the more the steeper it is seen: a speck,
  // not a card seen edge on (a bar round the middle of the picture); four pixels and smaller, upright as before
  float3 rise = float3(0, 1, 0);
  [branch] if (across > 4) rise = normalize(lerp(rise, cross(toEye, side), saturate((across - 4) / 4) * smoothstep(0.6, 0.95, abs(toEye.y))));
  float2 uv = corner == 0 ? float2(-1, 0) : corner == 1 ? float2(1, 0) : corner == 2 ? float2(1, 1)
            : corner == 3 ? float2(-1, 0) : corner == 4 ? float2(1, 1) : float2(-1, 1);
  float wide = size * (0.6 + 0.6 * age), high = size * 2 * sin(3.14159 * min(age * 1.6, 1));
  o.position = mul(viewProj, float4(p + nrm * 0.02 + side * (uv.x * wide) + rise * (uv.y * max(high, pixel)), 1));
  o.across = uv;
  o.show = tint.a * (1 - age) * (1 - age) * saturate((splash.w - far) / (splash.w * 0.3));
  float3 small;
  float3 sun = DropLight(p + float3(0, 0.1, 0), toEye, small);
  o.light = tint.rgb + (sun + small) * 0.6;
  return o;
}

float4 SplashPS(SOut i) : SV_Target {
  // a soft speck, brightest at its foot
  float speck = saturate(1 - length(float2(i.across.x, i.across.y * 1.5)));
  return float4(i.light, i.show * speck);
}
)hlsl";

struct SplashConstants {
  Mat4 back;
  float splash[4], tint[4];
};

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

// The kinds of weather, and how often each comes in a climate (shares of 100). ([seasons] `autumn`: the temperate
// climate in autumn, more overcast, fog and showers; `showers`: rain that comes and goes within the spell, Showers)
struct Kind {
  WeatherNow weather;
  int temperate, cold, dry, autumn;
  bool showers = false;
};
const Kind kKinds[] = {
    {{0.10f, 0, 0, 0, 0}, 22, 15, 45, 10},     // clear
    {{0.35f, 0, 0, 0, 0}, 26, 20, 35, 14},     // fair
    {{0.65f, 0, 0, 0, 0}, 16, 18, 12, 18},     // cloudy
    {{0.92f, 0, 0, 0.1f, 0}, 8, 12, 0, 16},    // overcast
    {{0.85f, 0.5f, 0, 0, 0}, 14, 0, 0, 11},    // rain
    {{1.0f, 1.0f, 0, 0.25f, 0}, 6, 0, 4, 5},   // storm
    {{0.5f, 0, 0, 1.0f, 0}, 8, 10, 4, 12},     // fog
    {{0.9f, 0, 0.8f, 0.1f, 0}, 0, 25, 0, 0},   // snow
    {{0.75f, 0.45f, 0, 0.05f, 0}, 0, 0, 0, 14, true},  // [seasons] showers (autumn's)
};

const Kind& SpellKind(uint32_t seed, int64_t spell, Climate climate) {
  if (spell <= 0) return kKinds[1];
  float left = Draw01(seed, static_cast<uint32_t>(spell), 17) * 100;
  for (const Kind& k : kKinds) {
    left -= static_cast<float>(climate == Climate::Cold ? k.cold : climate == Climate::Dry ? k.dry : climate == Climate::Autumn ? k.autumn : k.temperate);
    if (left < 0) return k;
  }
  return kKinds[1];
}

// [seasons] Showers: how much of a shower falls at `seconds`, 0..1. Time in stretches of 50 seconds, about two in five
// of them a shower, each coming and going over the last fifth of its stretch; between them the clouds break a little.
WeatherNow Showers(const WeatherNow& kind, uint32_t seed, double seconds) {
  const double at = std::max(seconds, 0.0) / 50;
  const int64_t n = static_cast<int64_t>(at);
  auto on = [&](int64_t i) { return Draw01(seed, static_cast<uint32_t>(i), 97) < 0.4f ? 1.0f : 0.0f; };
  float t = std::clamp((static_cast<float>(at - static_cast<double>(n)) - 0.8f) * 5, 0.0f, 1.0f);
  t = t * t * (3 - 2 * t);
  const float falls = on(n) + (on(n + 1) - on(n)) * t;
  WeatherNow w = kind;
  w.rain = kind.rain * falls;
  w.clouds = kind.clouds * (0.8f + 0.2f * falls);
  return w;
}

}  // namespace

// (photomode) the kinds, by their number in kKinds
int WeatherKindCount() { return static_cast<int>(sizeof kKinds / sizeof kKinds[0]); }

const char* WeatherKindName(int kind) {
  static const char* const kNames[] = {"clear", "fair", "cloudy", "overcast", "rain", "storm", "fog", "snow", "showers"};
  static_assert(sizeof kNames / sizeof kNames[0] == sizeof kKinds / sizeof kKinds[0], "a name for every kind");
  return kind >= 0 && kind < WeatherKindCount() ? kNames[kind] : "";
}

WeatherNow WeatherKindOf(int kind) { return kKinds[std::clamp(kind, 0, WeatherKindCount() - 1)].weather; }

bool ClimateSnows(Climate climate) {  // [snowhitch]
  for (const Kind& k : kKinds) {
    const int share = climate == Climate::Cold ? k.cold : climate == Climate::Dry ? k.dry : climate == Climate::Autumn ? k.autumn : k.temperate;
    if (share > 0 && k.weather.snow > 0) return true;
  }
  return false;
}

float AutumnMorningFog(uint32_t seed, float hour) {
  auto rise = [](float from, float to, float h) {
    const float t = std::clamp((h - from) / (to - from), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
  };
  const float byHour = rise(3.5f, 5.5f, hour) * (1 - rise(8, 10.5f, hour));
  return byHour * (0.45f + 0.4f * Draw01(seed, 3, 101));
}

WeatherNow WeatherAt(uint32_t seed, double seconds, Climate climate, float spell) {
  spell = std::max(spell, 1.0f);
  const double at = std::max(seconds, 0.0) / spell;
  const int64_t index = static_cast<int64_t>(at);
  const Kind& ka = SpellKind(seed, index, climate);
  const Kind& kb = SpellKind(seed, index + 1, climate);
  // ([seasons] a spell of showers: its rain as it comes and goes; any other as it was)
  const WeatherNow a = ka.showers ? Showers(ka.weather, seed, seconds) : ka.weather;
  const WeatherNow b = kb.showers ? Showers(kb.weather, seed, seconds) : kb.weather;
  // [onset] (Chris's snapshot test, 2026-10-25: "Rain pops in at once ... Wanted: clouds slowly forming and covering the map,
  // then the rain slowly picking up, as in real life") The last `kTurn` of a spell goes over into the next (it was the last
  // quarter, everything at once). Where more is to fall, the clouds gather first, over the first three fifths of that
  // (81 s of a 300 s spell), and the rain or snow begins only once they are well on, from just under half of it, easing in
  // slowly; where less is to fall, the other way round: the rain eases off first and the clouds break up after. Fog thickens
  // with the clouds and thins after the rain. A change with as much falling before as after goes over all of it together.
  constexpr float kTurn = 0.45f;
  const float into = std::clamp((static_cast<float>(at - static_cast<double>(index)) - (1 - kTurn)) / kTurn, 0.0f, 1.0f);
  auto over = [into](float from, float to) {  // (eased from `from` to `to` of the way)
    const float s = std::clamp((into - from) / (to - from), 0.0f, 1.0f);
    return s * s * (3 - 2 * s);
  };
  const float fallsBefore = a.rain + a.snow, fallsAfter = b.rain + b.snow;
  float tClouds = over(0, 1), tFalls = tClouds, tFog = tClouds;
  if (fallsAfter > fallsBefore + 1e-4f) {
    tClouds = over(0, 0.6f);
    tFog = over(0, 0.8f);
    const float s = over(0.45f, 1);
    tFalls = s * s;  // (picking up slowly: a few drops first)
  } else if (fallsAfter < fallsBefore - 1e-4f) {
    const float s = over(0, 0.55f);
    tFalls = 1 - (1 - s) * (1 - s);  // (the last few drops linger)
    tFog = over(0.2f, 1);
    tClouds = over(0.4f, 1);
  }
  WeatherNow w;
  w.clouds = a.clouds + (b.clouds - a.clouds) * tClouds;
  w.rain = a.rain + (b.rain - a.rain) * tFalls;
  w.snow = a.snow + (b.snow - a.snow) * tFalls;
  w.fog = a.fog + (b.fog - a.fog) * tFog;
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
  std::string source = lit ? std::string("#define LIT 1\n") + kViewShaderBlock + kLightingShaderBlock + kDropLight + kShader
                            : std::string(kViewShaderBlock) + kShader;
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

gfx::Pipeline Precipitation::MakeSplashes(std::string* error) {
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kLightingShaderBlock + kDropLight +
                       kSplashShader;
  gfx::PipelineDesc pd;
  pd.name = "rain's splashes";
  pd.vertexSource = pd.pixelSource = source;
  pd.vertexEntry = "SplashVS";
  pd.pixelEntry = "SplashPS";
  pd.cull = gfx::Cull::None;
  pd.depthTest = false;
  pd.depthWrite = false;
  pd.depthTarget = false;  // (into the scene's colour alone: its depth is read)
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  return device_->CreatePipeline(pd, error);
}

void Precipitation::PrepareSplashes() {  // [hitches] (as DrawSplashes makes them the first time)
  if (splashTried_) return;
  splashTried_ = true;
  std::string why;
  splashPipeline_ = MakeSplashes(&why);
  if (!splashPipeline_) fprintf(stderr, "render: rain's splashes cannot be used (%s)\n", why.c_str());
}

void Precipitation::PrepareLit() {  // [hitches] (as Draw makes it the first time)
  if (litTried_) return;
  litTried_ = true;
  std::string why;
  litPipeline_ = Make(true, &why);
  if (!litPipeline_) fprintf(stderr, "render: rain and snow lit where they fall cannot be used (%s)\n", why.c_str());
}

void Precipitation::DrawSplashes(const View& view, const Camera& camera, float rain, gfx::Texture sceneDepth) {
  if (rain <= 0 || view.height <= 0 || !sceneDepth) return;
  if (!splashTried_) {  // (made the first time they are wanted)
    splashTried_ = true;
    std::string why;
    splashPipeline_ = MakeSplashes(&why);
    if (!splashPipeline_) fprintf(stderr, "render: rain's splashes cannot be used (%s)\n", why.c_str());
  }
  if (!splashPipeline_) return;
  const ViewConstants& c = view.constants;
  SplashConstants k{};
  k.back = Inverse(c.viewProj);
  k.splash[0] = 0.2f;
  k.splash[1] = 0.05f;
  k.splash[2] = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height) * pixelScale;  // (photofix: the window's pixel)
  k.splash[3] = std::clamp(camera.distance * 2.5f, 60.0f, 400.0f);
  for (int i = 0; i < 3; ++i) k.tint[i] = (c.skyZenith[i] + c.skyHorizon[i]) * 0.6f * 1.2f;  // (as the rain's)
  k.tint[3] = 0.45f;
  device_->SetPipeline(splashPipeline_);
  device_->SetConstants(0, &c, sizeof c);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(3, sceneDepth);
  // (photofix: so many to the window's view, a photo's tile its share)
  const uint32_t splashes = static_cast<uint32_t>(3000.0f * std::clamp(rain, 0.0f, 1.0f) * pictureShare);
  if (splashes) device_->Draw(splashes * 6);
  device_->SetTexture(3, {});
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
  const float perPixel = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height) * pixelScale;  // (photofix: the window's pixel)
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
    // thin drop in a beam is seen against the patch of ground the beam lights, and would hardly show (Chris, 2026-10-17:
    // the sparkle is fine as it is)
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
