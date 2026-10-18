#include "movement_dust.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "dds.h"
#include "json.h"
#include "lighting.h"
#include "models.h"
#include "remasters.h"
#include "scmap.h"

namespace render {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

float Smooth(float from, float to, float x) {
  const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// The puffs of dust and spray: each a card facing the camera, its place from where it was thrown and its age (thrown,
// slowed as by air, carried by the wind; spray falling back), growing as it spreads; dust coloured as the ground where
// it was thrown (paler and greyer: the dry top of it), spray as water's; lit as the blasts' dust is (blasts.cpp: the sun
// softly through it on its sunny side, the sky from round about), the sun sent on a little more forward than back, and
// with lit smoke on by the small lights and through the smoke's own shadow as smoke is; seen through the air. All of that
// at its corners (a puff is soft and small: the light across it goes smoothly), so a pixel only shapes it and tests it:
// faded where it meets the ground or anything solid (as the particles are, by the scene's depth). Six corners a puff.
const char kPuffs[] = R"hlsl(
cbuffer Dust : register(b1) {
  float4 right;    // xyz: the camera's right; w: the camera's near distance (the scene's depth is that over the distance)
  float4 upward;   // xyz: the camera's up; w: world units of the heights' largest value
  float4 mapInfo;  // xy: the map's width and height; z: the water's level; w: the first row of the puffs drawn
  float4 wind;     // xy: the wind near the ground (world units a second, along x and z); z: 1 = lit as smoke is; w: 1 = the ground's colours are there
  float4 fade;     // x: from how far off the eye a puff thins out (world units); y: 1 / over how far
};
// three rows a puff: (x, its height over what lies there, z, its age), (as it was thrown: velocity, its life),
// (its size at first, at the last, how much shows, its kind + its own number: 0 dust, 1 spray, 2 a hover unit's faint dust)
StructuredBuffer<float4> dustRows : register(t0, space1);
Texture2D<float> heightMap : register(t0);
Texture2D groundColours : register(t1);  // Terrain::GroundColours: rgb the ground's colour (as stored), a how grassy
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

float PuffGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * upward.w;
}

// The small lights at a place in the dust (lights.h; as the lit smoke's, particles.cpp): dust sends light on to all
// sides about evenly, so only how near each light is counts; a beam within its cone, through its lamp's shadow.
float3 PuffSmallLights(float3 p) {
  float3 sum = 0;
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
        float fade = (1 - far2) * (1 - far2) / (1 + 4 * far2);
        float4 colour = lightData[uint(lightGridSize.z) + which * 4 + 1];
        float4 beam = lightData[uint(lightGridSize.z) + which * 4 + 2];
        if (beam.w > -1.5) {
          float3 l = normalize(toLight);
          float off = sqrt(saturate((1 - dot(-l, beam.xyz)) / max(1 - beam.w, 1e-4)));
          float reaches = 1 - smoothstep(0.7, 1.0, off);
          float map = lightData[uint(lightGridSize.z) + which * 4 + 3].x;
          [branch] if (map > -0.5 && reaches > 0) reaches *= LampShadow(p, l, uint(map));
          fade = (1 - far2) * (1 - far2) / (1 + 1.5 * far2) * reaches + fade * colour.w * saturate(1 - far2 * 25);
        }
        sum += colour.rgb * fade;
      }
    }
  }
  return sum;
}

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD1;      // -1..1 across the card
  float3 colour : TEXCOORD2;  // as the eye sees it here: lit, through the air (linear)
  float4 shade : TEXCOORD3;   // x: how much shows (0..1); y: over how many world units it fades where it meets something; z: its
                              // noise's seed
  nointerpolation float kind : TEXCOORD6;
};

static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};

VOut VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (no puff: behind the eye)
  uint at = uint(mapInfo.w) + instance * 3;
  float4 a = dustRows[at], b = dustRows[at + 1], c = dustRows[at + 2];
  float age = a.w, life = b.w;
  if (age < 0 || age >= life) return o;
  float kind = floor(c.w), seed = frac(c.w) * 97;
  bool spray = kind > 0.5 && kind < 1.5;
  float t = age / life;
  // where it is: thrown and slowed as by air (to a stop in about a second), carried by the wind (spray less: it falls)
  float slow = (1 - exp(-2.0 * age)) * 0.5;
  float2 xz = a.xz + b.xz * slow + wind.xy * (age * (spray ? 0.6 : 1.0));
  // growing as it spreads, quickly at first
  float size = lerp(c.x, c.y, (1 - exp(-3 * t)) / (1 - exp(-3.0)));
  // its height over what lies there: thrown up and slowed; spray falls back; never through it (its lower part fades into it)
  float lift = a.y + b.y * slow - (spray ? 1.4 * age * age : 0);
  lift = max(lift, size * (spray ? 0.2 : 0.4));
  float surface = spray ? mapInfo.z : PuffGround(xz);
  float3 centre = float3(xz.x, surface + lift, xz.y);
  // (turned a little each, so the cards do not all show the same face)
  float turn = seed * 0.37 + age * (frac(seed * 0.13) - 0.5) * 0.8;
  float2 corner = kCorners[vertex];
  float2 tc = float2(corner.x * cos(turn) - corner.y * sin(turn), corner.x * sin(turn) + corner.y * cos(turn));
  float3 out_ = right.xyz * tc.x + upward.xyz * tc.y;
  float3 p = centre + out_ * size;
  o.position = mul(viewProj, float4(p, 1));
  o.uv = corner;
  // as much of it shows: kicked up over a fifth of a second (spray at once), thinning out; less far off
  float far = length(centre - cameraPosition);
  o.shade.x = c.z * smoothstep(0, spray ? 0.08 : 0.2, age) * pow(1 - t, 1.5) * (1 - saturate((far - fade.x) * fade.y));
  o.shade.y = size * 0.6;
  o.shade.z = seed;
  o.kind = kind;
  if (o.shade.x <= 0) return o;
  float sun = SunShadow(centre, float3(0, 1, 0)) * CloudShadow(centre);
  float3 small = 0;
  [branch] if (wind.z > 0.5) {
    // lit as smoke is: the smoke over it shadows it (read here whether or not the surfaces take it), and the small lights
    [branch] if (smokeShadow.w > 0.5 && smokeShadow.x < 0.5) sun *= SmokeShadowAt(centre + sunDirection * (size * 0.6), 0.6);
    small = PuffSmallLights(centre);
  }
  // what it is made of: spray as water; dust as the ground where it was thrown, paler and greyer (the dry top of it), and
  // where the ground is grassy the earth under the grass more than the grass (as the blasts' dust)
  float3 albedo = float3(0.74, 0.79, 0.82);
  if (!spray) {
    float4 g4 = wind.w > 0.5 ? groundColours.SampleLevel(samplerLinearClamp, a.xz / mapInfo.xy, 0) : float4(0.55, 0.48, 0.38, 0);
    float3 g = lerp(ToLinear(g4.rgb), ToLinear(float3(0.43, 0.37, 0.28)), g4.a * 0.7);
    albedo = lerp(g, dot(g, float3(0.3, 0.6, 0.1)), 0.45) * 1.4 + 0.02;
  }
  // lit as the ground is, at each corner (a ball's way out there: its sunny side brighter across the card): the sun softly
  // on the side that faces it (spray lets more of it through), sent on a little more forward than back (thin dust, and spray
  // more, brightens before the sun), the sky from round about; seen through the air
  float3 toward = normalize(cameraPosition - centre);
  float3 n = normalize(out_ * 0.53 + toward * 0.66);
  float wrap = spray ? 0.8 : 0.5;
  float sunSide = saturate((dot(n, sunDirection) + wrap) / (1 + wrap));
  float g = spray ? 0.55 : 0.3, mu = dot(-sunDirection, toward);
  float phase = lerp(1, (1 - g * g) / pow(max(1 + g * g - 2 * g * mu, 1e-4), 1.5), spray ? 0.45 : 0.3);
  float3 skyDiffuse = skyZenith.w > 0.5 ? SkyAmbient(n) : ToLinear(environmentSky.SampleLevel(samplerLinear, n, 8).rgb);
  float3 light = ToLinear(sunColor) * (lightMultiplier * modern.x) * (sunSide * phase * lerp(0.35, 1, sun)) +
                 (skyDiffuse + ToLinear(shadowFill) * 0.5) * modern.y + small;
  o.colour = ThroughAirTo(albedo * light, p);
  return o;
}

float Meets(VOut i, float soft, out uint coverage) {
  coverage = 0;
  float nearest = 1e9;
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
    if (gap > 0) {
      coverage |= 1u << s;
      nearest = min(nearest, gap);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float gap = (d > 0 ? right.w / d : 1e9) - i.position.w;
  if (gap > 0) {
    coverage = 1;
    nearest = gap;
  }
#endif
  return coverage == 0 ? 0 : saturate(nearest / max(soft, 1e-3));
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  float r2 = dot(i.uv, i.uv);
  float body = (1 - r2) * (1 - r2);
  coverage = 0;
  if (r2 >= 1 || body * i.shade.x < 0.004) discard;  // (outside it, or too faint to show: before the scene's depth is read)
  float meets = Meets(i, i.shade.y, coverage);
  if (meets <= 0) discard;
  float spray = i.kind > 0.5 && i.kind < 1.5 ? 1 : 0;
  // a soft round puff, billowing (a noise over it); spray finer and broken, as drops
  float billow = CloudNoise(float3(i.uv * lerp(1.6, 3.4, spray) + i.shade.z, i.shade.z)) * 0.6 +
                 CloudNoise(float3(i.uv * lerp(3.7, 8.0, spray) - i.shade.z, 2.3)) * 0.4;
  float alpha = saturate(body * lerp(0.35 + 0.9 * billow, saturate(billow * 2.6 - 0.9), spray) * i.shade.x) * meets;
  if (alpha <= 0.004) discard;
  return float4(i.colour, alpha);
}
)hlsl";

// The contrails: a band facing the camera through each contrail's points (the oldest first, its newest where the engine
// is now), widening with its age, drifting with the wind high up; condensing a few units behind the engine (a gap between
// the aircraft and its contrail, which a shot's trail never has), thinning as it spreads (the same vapour over a wider
// band), breaking up into wisps as it ages, gone at its end. Pale and lit as a thin cloud is (the sun sent on forward:
// bright toward the sun), never glowing; never thinner than a pixel and a half on the screen (thinner, it would shimmer):
// what it lacks in width it lacks in how much shows. Six corners a piece.
const char kTrails[] = R"hlsl(
cbuffer Trails : register(b1) {
  float4 trail;   // x: the first row of the points; y: how long a contrail lasts (seconds); z: its width when fresh; w: at the last
  float4 trail2;  // x: how much of it shows fresh; y: how fast it widens (1 / seconds); z, w: the wind high up (world units a second, x, z)
  float4 trail3;  // x: world units a pixel spans one world unit off the eye; y: the camera's near distance; z: from how far off it thins out; w: 1 / over how far
  float4 trail4;  // x: how many points there are
};
StructuredBuffer<float4> trailRows : register(t0, space1);  // two rows a point: (x, y, z, its age), (1 = joined to the next, how strong, its seed, 0)
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t3);
#else
Texture2D<float> sceneDepth : register(t3);
#endif

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float4 info : TEXCOORD1;  // x: -1..1 across; y: how much shows; z: its age over its life; w: its seed
  float sunLit : TEXCOORD2; // what of the sun's light comes through the clouds and the shadows to it
};

float3 Drifted(float4 p) { return p.xyz + float3(trail2.z, 0, trail2.w) * p.w; }

static const float2 kCorners[6] = {float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1)};

VOut VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);
  uint first = uint(trail.x), points = uint(trail4.x);
  if (instance + 1 >= points) return o;
  uint at = first + instance * 2;
  float4 p0 = trailRows[at], f0 = trailRows[at + 1];
  if (f0.x < 0.5) return o;  // (the last point of its contrail: no piece from it)
  float4 p1 = trailRows[at + 2], f1 = trailRows[at + 3];
  float3 a = Drifted(p0), b = Drifted(p1);
  // (the way the band runs at each end: along the points on both sides of it, so pieces meet without a gap or a fold;
  // every row read lies within the points, whichever way the branches go: HLSL's && does not stop at the first)
  uint beforeAt = instance > 0 ? at - 2 : at, afterAt = min(at + 4, first + (points - 1) * 2);
  float4 pb = trailRows[beforeAt], fb = trailRows[beforeAt + 1], pa = trailRows[afterAt];
  float3 before = instance > 0 && fb.x > 0.5 ? Drifted(pb) : a;
  float3 after = f1.x > 0.5 ? Drifted(pa) : b;
  float2 corner = kCorners[vertex];
  bool end = corner.y > 0.5;
  float3 P = end ? b : a;
  float3 T = end ? after - a : b - before;
  float age = end ? p1.w : p0.w, strength = end ? f1.y : f0.y;
  float width = lerp(trail.z, trail.w, 1 - exp(-age * trail2.y));
  float3 toEye = cameraPosition - P;
  float far = length(toEye);
  float drawn = max(width, far * trail3.x * 1.5);
  float3 side = cross(T, toEye);
  side = dot(side, side) > 1e-10 ? normalize(side) : float3(1, 0, 0);
  float3 w = P + side * (corner.x * drawn);
  o.position = mul(viewProj, float4(w, 1));
  o.world = w;
  float t = saturate(age / trail.y);
  float shows = trail2.x * strength * smoothstep(0.12, 0.6, age) * pow(1 - t, 1.3) * min(1, pow(0.3 / width, 0.6)) * (width / drawn);
  o.info = float4(corner.x, shows * (1 - saturate((far - trail3.z) * trail3.w)), t, f0.z);
  o.sunLit = CloudShadow(P) * SunShadowMaps(P, float3(0, 0, 0));
  return o;
}

float4 PS(VOut i, out uint coverage : SV_Coverage) : SV_Target {
  // only where nothing solid stands in front of it; and thinner where it passes close in front of what the eye looks at
  // (the readability rule: a camera low over a fight sees the fight through it, not it: from far up, and against the sky,
  // it shows whole)
  coverage = 0;
  float behind = 1e9;  // (the nearest solid thing behind it, along the line of sight)
#ifdef MSAA
  int2 pixel = int2(i.position.xy);
  for (uint s = 0; s < SAMPLES; ++s) {
    float d = sceneDepth.Load(pixel, s);
    float scene = d > 0 ? trail3.y / d : 1e9;
    if (scene > i.position.w) {
      coverage |= 1u << s;
      behind = min(behind, scene);
    }
  }
#else
  float d = sceneDepth.Load(int3(i.position.xy, 0));
  float scene = d > 0 ? trail3.y / d : 1e9;
  if (scene > i.position.w) {
    coverage = 1;
    behind = scene;
  }
#endif
  if (coverage == 0) discard;
  float clear = behind > 1e8 ? 1 : smoothstep(0.3, 0.8, i.position.w / behind);  // (the sky behind it: nothing to see through it)
  float x = i.info.x;
  float across = exp(-2.2 * x * x) * (1 - x * x);
  // (it breaks up into wisps as it ages)
  float n = CloudNoise(i.world * 0.45 + i.info.w) * 0.65 + CloudNoise(i.world * 1.3 - i.info.w) * 0.35;
  float alpha = saturate(i.info.y * across * lerp(1, 0.25 + 1.5 * n, saturate(i.info.z * 1.8))) * clear;
  if (alpha <= 0.002) discard;
  float3 V = normalize(cameraPosition - i.world);
  float g = 0.6, mu = dot(-sunDirection, V);
  float phase = lerp(1, (1 - g * g) / pow(max(1 + g * g - 2 * g * mu, 1e-4), 1.5), 0.35);
  float3 sky = skyZenith.w > 0.5 ? SkyAmbient(float3(0, 1, 0)) : ToLinear(environmentSky.SampleLevel(samplerLinear, float3(0, 1, 0), 8).rgb);
  float3 light = ToLinear(sunColor) * (lightMultiplier * modern.x) * (0.75 * phase * i.sunLit) + (sky + ToLinear(shadowFill) * 0.3) * modern.y;
  return float4(ThroughAirTo(0.8 * light, i.world), alpha);
}
)hlsl";

}  // namespace

MovementDust::~MovementDust() {
  for (gfx::Pipeline p : {puffPipeline_, trailPipeline_})
    if (p) device_->Destroy(p);
  if (rows_) device_->Destroy(rows_);
}

void MovementDust::SetMap(const MapData* map, const std::string& mapPath, std::function<std::optional<std::string>(const std::string&)> readFile) {
  map_ = map;
  mapPath_ = mapPath;
  readFile_ = std::move(readFile);
  gridMade_ = false;  // (worked out again the first frame it is on)
  grid_.clear();
  Clear();
  const char* debug = std::getenv("MOHO64_DEBUG_DUST");
  debug_ = debug ? std::max(1, atoi(debug)) : 0;
}

void MovementDust::Clear() {
  puffs_.clear();
  trails_.clear();
  unitsSeen_.clear();
  lastClock_ = -1;
}

float MovementDust::LayerDust(const std::string& texture, const std::string& given) {
  std::string name = Lower(texture);
  name = name.substr(name.rfind('/') + 1);
  auto has = [&](std::initializer_list<const char*> words) {
    for (const char* w : words)
      if (name.find(w) != std::string::npos) return true;
    return false;
  };
  const bool wet = has({"wet", "damp", "moist"});
  // (a kind given, as the tracks read it: dry sand the most, earth, a little of gravel's, rock's and grass's, none of
  // mud's or snow's; two with a plus half each ("sand+mud": the wet sand at the waterline); and a layer its name calls
  // wet raises next to none whatever its kind)
  if (!given.empty()) {
    float sum = 0;
    int n = 0;
    for (size_t at = 0; at <= given.size();) {
      const size_t plus = given.find('+', at);
      const std::string w = Lower(given.substr(at, plus == std::string::npos ? std::string::npos : plus - at));
      const float d = w == "sand" ? 1.0f : w == "dirt" || w == "soil" || w == "earth" ? 0.7f : w == "gravel" ? 0.35f : w == "hard" || w == "rock" ? 0.12f
                    : w == "grass" ? 0.04f : w == "mud" || w == "snow" ? 0.0f : -1.0f;
      if (d >= 0) sum += d, ++n;
      if (plus == std::string::npos) break;
      at = plus + 1;
    }
    if (n > 0) return sum / static_cast<float>(n) * (wet ? 0.1f : 1.0f);
  }
  if (has({"snow", "ice", "mud", "swamp", "bog", "marsh", "water", "puddle"})) return 0;
  const bool rock = has({"rock", "cliff", "stone", "concrete", "tarmac", "asphalt", "crystal", "lava", "metal", "plate", "macro", "ribbon"});
  if (has({"sand", "beach", "dune"}) && !has({"sandstone"})) return wet ? 0.05f : 1.0f;
  if (has({"desert"}) && !rock) return wet ? 0.05f : 0.9f;
  if (has({"grass", "veg", "meadow", "moss", "lawn", "field", "leaf", "leaves"})) return has({"dry", "dead", "brown", "burnt"}) ? 0.45f : 0.03f;
  if (has({"forest", "floor", "litter", "humus", "fern"})) return 0.15f;
  if (has({"gravel", "pebble", "scree", "shingle", "rubble"})) return 0.3f;
  if (rock) return has({"pale", "dust", "desert", "sandstone"}) ? 0.2f : 0.1f;
  if (has({"dirt", "soil", "earth", "dust", "clay", "path", "road", "track"})) return wet ? 0.1f : 0.7f;
  return wet ? 0.05f : 0.5f;  // (what the name does not tell: some)
}

void MovementDust::MakeGrid() {
  const auto started = std::chrono::steady_clock::now();
  gridMade_ = true;
  grid_.clear();
  gridW_ = gridH_ = 0;
  if (!map_ || map_->width <= 0 || map_->height <= 0) return;
  // each layer's dust (the upper one, the tenth, colours over the others: left out), by remap.json's "ground" where the
  // map's remastered ground gives one, else by its name
  JsonValue remap;
  if (readFile_) {
    if (auto file = readFile_(std::string(kRemasteredTerrain) + "/remap.json")) {
      std::string why;
      if (!ReadJson(*file, &remap, &why)) remap = JsonValue{};
    }
  }
  const JsonValue& entries = remap["maps"][Lower(mapPath_)];
  float layer[9] = {};
  for (int i = 0; i < 9; ++i) {
    const std::string& texture = map_->albedo[i].texture;
    const std::string given = entries[Lower(texture)]["ground"].StringOr(std::string());
    layer[i] = texture.empty() ? 0.0f : LayerDust(texture, given);
    if (debug_ && !texture.empty()) printf("render: movement dust: layer %d %s%s%s: dust %.2f\n", i, texture.c_str(), given.empty() ? "" : " ground ", given.c_str(), layer[i]);
  }
  // the masks (the second only for the ground's look of eight layers, as its shader reads them): plain ones read where
  // they lie in the map's file, a 4096-square one is not copied; compressed ones decoded whole
  struct Mask {
    DdsImage image;
    std::vector<uint8_t> decoded;
    const uint8_t* texels = nullptr;
    int w = 0, h = 0;
    bool swap = false;  // (stored blue first)
    bool Open(const std::string& dds) {
      std::string why;
      if (dds.empty() || !ParseDds(dds, &image, &why) || image.width <= 0 || image.height <= 0) return false;
      w = image.width;
      h = image.height;
      if (image.format == gfx::Format::RGBA8 || image.format == gfx::Format::BGRA8) {
        texels = image.data;
        swap = image.format == gfx::Format::BGRA8;
        return true;
      }
      if (!DecodeRgba(image, 0, &decoded)) return false;
      texels = decoded.data();
      return true;
    }
    uint8_t At(float u, float v, int k) const {  // (channel k: 0 red .. 3 alpha, as a shader reads it)
      const uint8_t* t = texels + (static_cast<size_t>(std::min(static_cast<int>(v * static_cast<float>(h)), h - 1)) * static_cast<size_t>(w) +
                                   static_cast<size_t>(std::min(static_cast<int>(u * static_cast<float>(w)), w - 1))) * 4;
      return t[swap && k != 1 && k != 3 ? 2 - k : k];
    }
  };
  Mask low, high;
  const bool haveLow = low.Open(map_->maskLow);
  const bool haveHigh = map_->shader == "TTerrainXP" && high.Open(map_->maskHigh);
  // a cell of two world units (more on the largest maps: at most 512 cells a side)
  const float width = static_cast<float>(map_->width), height = static_cast<float>(map_->height);
  cell_ = std::max(2.0f, std::max(width, height) / 512.0f);
  gridW_ = static_cast<int>(std::ceil(width / cell_)) + 1;
  gridH_ = static_cast<int>(std::ceil(height / cell_)) + 1;
  grid_.assign(static_cast<size_t>(gridW_) * static_cast<size_t>(gridH_), layer[0]);
  // (laid over each other as the ground's shader lays the layers: each by its mask pushed to its upper half)
  auto over = [](float d, float to, uint8_t raw) { return d + (to - d) * std::clamp(static_cast<float>(raw) / 255.0f * 2 - 1, 0.0f, 1.0f); };
  double sum = 0;
  for (int gz = 0; gz < gridH_; ++gz)
    for (int gx = 0; gx < gridW_; ++gx) {
      const float u = std::min(static_cast<float>(gx) * cell_ / width, 1.0f), v = std::min(static_cast<float>(gz) * cell_ / height, 1.0f);
      float d = layer[0];
      for (int k = 0; k < 4 && haveLow; ++k) d = over(d, layer[1 + k], low.At(u, v, k));
      for (int k = 0; k < 4 && haveHigh; ++k) d = over(d, layer[5 + k], high.At(u, v, k));
      grid_[static_cast<size_t>(gz) * gridW_ + gx] = d;
      sum += d;
    }
  if (debug_)
    printf("render: movement dust: the ground's dust over %dx%d cells of %.1f units (masks %dx%d%s), on average %.2f, in %.1f ms\n", gridW_, gridH_,
           cell_, low.w, low.h, haveHigh ? " and the second" : "", sum / static_cast<double>(grid_.size()),
           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
  // (MOHO64_DEBUG_DUST_GRID=<file.ppm>: the grid as a picture, north up, white the dustiest, what lies under the water
  // blue, for checking)
  if (const char* path = std::getenv("MOHO64_DEBUG_DUST_GRID")) {
    if (FILE* f = fopen(path, "wb")) {
      fprintf(f, "P6\n%d %d\n255\n", gridW_, gridH_);
      for (int gz = 0; gz < gridH_; ++gz)
        for (int gx = 0; gx < gridW_; ++gx) {
          const int d = static_cast<int>(std::clamp(grid_[static_cast<size_t>(gz) * gridW_ + gx], 0.0f, 1.0f) * 255 + 0.5f);
          const bool wet = groundHeight && groundHeight(static_cast<float>(gx) * cell_, static_cast<float>(gz) * cell_) < waterLevel;
          fputc(wet ? d / 4 : d, f);
          fputc(wet ? d / 4 : d, f);
          fputc(wet ? 96 + d / 2 : d, f);
        }
      fclose(f);
    }
  }
}

float MovementDust::DustAt(float x, float z) const {
  if (grid_.empty()) return 0;
  const float gx = std::clamp(x / cell_, 0.0f, static_cast<float>(gridW_ - 1)), gz = std::clamp(z / cell_, 0.0f, static_cast<float>(gridH_ - 1));
  const int x0 = std::min(static_cast<int>(gx), gridW_ - 2 < 0 ? 0 : gridW_ - 2), z0 = std::min(static_cast<int>(gz), gridH_ - 2 < 0 ? 0 : gridH_ - 2);
  const int x1 = std::min(x0 + 1, gridW_ - 1), z1 = std::min(z0 + 1, gridH_ - 1);
  const float fx = gx - static_cast<float>(x0), fz = gz - static_cast<float>(z0);
  auto at = [&](int i, int k) { return grid_[static_cast<size_t>(k) * gridW_ + i]; };
  return (at(x0, z0) * (1 - fx) + at(x1, z0) * fx) * (1 - fz) + (at(x0, z1) * (1 - fx) + at(x1, z1) * fx) * fz;
}

float MovementDust::Random() {
  seed_ ^= seed_ << 13;
  seed_ ^= seed_ >> 17;
  seed_ ^= seed_ << 5;
  return static_cast<float>(seed_ >> 8) / 16777216.0f;
}

void MovementDust::Throw(const Puff& p) {
  puffs_.push_back(p);
  while (puffs_.size() > static_cast<size_t>(kMostPuffs)) puffs_.pop_front();  // (the oldest let go first)
}

const MovementDust::Kind& MovementDust::KindOf(const std::string& blueprint) {
  auto known = kinds_.find(blueprint);
  if (known != kinds_.end()) return known->second;
  Kind& k = kinds_[blueprint];
  BpValue bp;
  if (readBlueprint && readBlueprint(blueprint, &bp)) {
    const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
    if (motion == "RULEUMT_Air") k.move = Kind::Air;
    else if (motion == "RULEUMT_Hover") k.move = Kind::Hover;
    else if (motion == "RULEUMT_Land" || motion == "RULEUMT_Amphibious" || motion == "RULEUMT_AmphibiousFloating") k.move = Kind::Land;
    else if (!motion.empty() && motion != "RULEUMT_None") k.move = Kind::Water;  // (ships and submarines: their wakes)
    // (a walker: footfalls of its own, or a walking animation; every land unit of the game without either rolls)
    k.walks = bp["Display"]["MovementEffects"]["Land"]["Footfall"].IsTable() || !bp["Display"]["AnimationWalk"].StringOr(std::string()).empty();
    k.maxSpeed = static_cast<float>(k.move == Kind::Air ? bp["Air"]["MaxAirspeed"].NumberOr(15) : bp["Physics"]["MaxSpeed"].NumberOr(3));
    for (const BpValue& b : bp["Display"]["MovementEffects"]["Air"]["Contrails"]["Bones"].items) {
      const std::string bone = Lower(b.StringOr(std::string()));
      if (!bone.empty() && k.engines.size() < 4) k.engines.push_back(bone);
    }
  }
  if (debug_)
    printf("render: movement dust: %s moves %s%s, at most %.1f a second, %zu contrail bones\n", blueprint.c_str(),
           k.move == Kind::Air ? "in the air" : k.move == Kind::Hover ? "hovering" : k.move == Kind::Land ? "on land" : k.move == Kind::Water ? "on water" : "not",
           k.walks ? " (walks)" : "", k.maxSpeed, k.engines.size());
  return k;
}

void MovementDust::Begin(double clock, bool on, const Weather& w, Vec3 eye) {
  if (lastClock_ >= 0 && clock < lastClock_ - 1) Clear();  // (a game started over, or played back from earlier)
  lastClock_ = clock;
  clock_ = clock;
  on_ = on;
  eye_ = eye;
  units = 0;
  if (!on) return;
  if (!gridMade_) MakeGrid();
  // what the weather leaves of the dust: none on wet ground (it dries in two minutes after rain), none while it rains,
  // none where snow lies, little while it snows
  const float wet = std::clamp(w.wet, 0.0f, 1.0f);
  dryness_ = std::pow(1 - wet, 1.5f) * std::clamp(1 - 4 * w.rain, 0.0f, 1.0f) * (1 - std::clamp(w.snowLying, 0.0f, 1.0f)) * (w.snow > 0 ? 0.3f : 1.0f);
  // contrails: longer and thicker in damp, cold air (clouds, fog, rain, snow, a cold climate, the night); shorter in a dry one
  float damp = 0.3f + 0.45f * w.clouds + 0.5f * w.fog + 0.25f * wet + 0.35f * w.rain + 0.3f * w.snow + 0.15f * (1 - w.daylight);
  if (w.cold) damp += 0.3f;
  if (w.dry) damp -= 0.25f;
  damp_ = std::clamp(damp, 0.15f, 1.4f);
  trailLife_ = 2.5f + 4.5f * damp_;
  trailWide_ = 0.7f + 0.9f * damp_;
}

void MovementDust::AddUnit(uint32_t id, const std::string& blueprint, const Model* model, Vec3 position, const Quat& orientation) {
  if (!on_ || !model || !model->ok || blueprint.empty()) return;
  ++units;
  const Kind& kind = KindOf(blueprint);
  if (kind.move == Kind::None || kind.move == Kind::Water) return;
  Unit& u = unitsSeen_[id];
  const double dt = clock_ - u.clock;
  // (first seen, after a gap, or moved farther than it can go: from here on)
  const bool jumped = u.clock >= 0 && dt >= 0 && Length(position - u.place) > std::max(kind.maxSpeed, 1.0f) * static_cast<float>(dt) * 3 + 2;
  if (u.kind != &kind || u.clock < 0 || dt < 0 || dt > 0.6 || jumped) {
    for (int slot : u.trails)
      if (slot >= 0 && slot < static_cast<int>(trails_.size()) && trails_[static_cast<size_t>(slot)].unit == id) trails_[static_cast<size_t>(slot)].live = false;
    u = Unit{};
    u.kind = &kind;
    u.place = position;
    u.clock = u.seen = clock_;
    return;
  }
  u.seen = clock_;
  if (dt < 1e-4) return;  // (a paused game, or the same moment drawn again)
  const Vec3 moved = position - u.place;
  const float step = static_cast<float>(dt);
  u.velocity = u.velocity + (moved * (1 / step) - u.velocity) * std::min(1.0f, step * 10);  // (smoothed over a tenth of a second)
  u.place = position;
  u.clock = clock_;
  if (kind.move == Kind::Air) {
    Contrails(id, u, model, position, orientation, Length(u.velocity));
    return;
  }
  if (Length(position - eye_) > farthest) return;  // (too far off to be seen)
  const float speed = std::sqrt(u.velocity.x * u.velocity.x + u.velocity.z * u.velocity.z);
  const float scale = model->scale;
  const Vec3 lo = model->boundsMin * scale, hi = model->boundsMax * scale;
  const float width = std::max(hi.x - lo.x, 0.3f), height = std::max(hi.y - lo.y, 0.2f);
  const float halfX = width * 0.5f, halfZ = std::max(hi.z - lo.z, 0.3f) * 0.5f;
  Vec3 forward = Rotate(orientation, {0, 0, 1});
  forward.y = 0;
  forward = Length(forward) > 1e-4f ? Normalize(forward) : Vec3{0, 0, 1};
  const Vec3 side{forward.z, 0, -forward.x};
  const float share = std::clamp(speed / std::max(kind.maxSpeed, 0.5f), 0.0f, 1.3f), most = std::min(share, 1.0f);
  const float ground = groundHeight ? groundHeight(position.x, position.z) : position.y;
  const bool overWater = waterLevel > ground + 0.05f;  // (the ground lies under the water's surface)
  if (kind.move == Kind::Land) {
    // vehicles and walkers on dry ground (not wading, not in the air, not standing)
    if (overWater || speed < 0.25f || position.y - ground > 1.0f) {
      u.carry = 0;
      return;
    }
    const Vec3 rear = position + Rotate(orientation, {0, 0, lo.z * 0.85f});
    const float dust = DustAt(rear.x, rear.z) * dryness_ * dustStrength;
    if (dust < 0.03f) {
      u.carry = 0;
      return;
    }
    // a pair of puffs (behind its two treads, or its two feet) each so far gone: the closer the dustier the ground
    const float spacing = (0.6f * width + 0.3f) / std::pow(std::min(dust, 1.0f), 0.4f) * (kind.walks ? 1.6f : 1.0f);
    u.carry += std::sqrt(moved.x * moved.x + moved.z * moved.z);
    for (int thrown = 0; u.carry >= spacing && thrown < 6; ++thrown) {
      u.carry -= spacing;
      for (int s = -1; s <= 1; s += 2) {
        const Vec3 at = position + Rotate(orientation, {static_cast<float>(s) * halfX * 0.7f, 0, lo.z * 0.85f});
        Puff p;
        p.x = at.x + side.x * (Random() - 0.5f) * 0.3f * width + forward.x * (Random() - 0.5f) * 0.2f * halfZ;
        p.z = at.z + side.z * (Random() - 0.5f) * 0.3f * width + forward.z * (Random() - 0.5f) * 0.2f * halfZ;
        p.lift = 0.08f * height + 0.05f;
        p.y = ground;
        const float up = (0.45f + 0.7f * most) * (0.6f + 0.6f * Random()) * std::clamp(height, 0.4f, 3.0f);
        p.velocity = forward * (-speed * 0.12f) + side * (static_cast<float>(s) * (0.15f + 0.35f * Random()) * most * std::clamp(width, 0.5f, 3.0f)) + Vec3{0, up, 0};
        p.born = clock_ - Random() * dt;
        p.life = (2.4f + 1.6f * Random()) * (0.75f + 0.5f * std::min(dust, 1.0f)) * (0.8f + 0.3f * most);
        p.size0 = std::clamp(0.28f * width, 0.1f, 1.6f) * (0.8f + 0.4f * Random());
        p.size1 = std::clamp((1.0f + 0.8f * most) * width * (0.7f + 0.6f * std::min(dust, 1.0f)), 0.4f, 6.0f) * (0.85f + 0.3f * Random());
        p.strength = 0.6f * std::pow(std::min(dust, 1.5f), 0.35f) * (0.45f + 0.55f * most) * (kind.walks ? 0.6f : 1.0f);
        p.kind = 0;
        p.seed = Random();
        Throw(p);
      }
    }
    return;
  }
  // hover units: a ring round the skirt, of spray over water and of faint dust over land
  const bool onWater = overWater && position.y >= waterLevel - 0.3f;
  const float faint = onWater ? 1.0f : std::max(DustAt(position.x, position.z), 0.15f) * dryness_ * dustStrength;
  if ((!onWater && (faint < 0.02f || position.y - ground > 1.5f)) || (onWater && sprayStrength <= 0)) {
    u.ring = u.trail = 0;
    return;
  }
  u.ring += (onWater ? (7 + 9 * share) * sprayStrength : 5 + 7 * share) * step;
  int thrown = 0;
  for (; u.ring >= 1 && thrown < 6; ++thrown) {
    u.ring -= 1;
    const float a = Random() * 6.2831853f;
    const Vec3 out = Rotate(orientation, {std::cos(a) * halfX * 1.05f, 0, std::sin(a) * halfZ * 1.05f});
    const float flat = std::sqrt(out.x * out.x + out.z * out.z);
    const Vec3 dir = flat > 1e-4f ? Vec3{out.x / flat, 0, out.z / flat} : forward;
    Puff p;
    p.x = position.x + out.x;
    p.z = position.z + out.z;
    p.y = onWater ? waterLevel : ground;
    if (onWater) {
      p.kind = 1;
      p.lift = 0.05f;
      p.velocity = dir * ((1.1f + 1.4f * most) * (0.7f + 0.6f * Random())) + u.velocity * 0.25f + Vec3{0, 0.9f + 0.7f * Random(), 0};
      p.life = 0.7f + 0.5f * Random();
      p.size0 = std::clamp(0.18f * width, 0.08f, 1.2f);
      p.size1 = std::clamp(0.5f * width, 0.25f, 3.0f) * (0.8f + 0.4f * Random());
      p.strength = 0.26f * (0.7f + 0.3f * most) * std::min(sprayStrength, 1.5f);
    } else {
      p.kind = 2;
      p.lift = 0.04f;
      p.velocity = dir * (1.0f + 1.0f * most) + u.velocity * 0.2f + Vec3{0, 0.25f + 0.3f * Random(), 0};
      p.life = 1.3f + 0.8f * Random();
      p.size0 = std::clamp(0.2f * width, 0.08f, 1.2f);
      p.size1 = std::clamp(0.8f * width, 0.3f, 4.0f);
      p.strength = 0.26f * std::sqrt(std::min(faint, 1.5f));
    }
    p.born = clock_ - Random() * dt;
    p.seed = Random();
    Throw(p);
  }
  // and a trail of spray behind a fast one over water
  if (!onWater || share < 0.25f) {
    u.trail = 0;
    return;
  }
  u.trail += 14 * share * sprayStrength * step;
  for (; u.trail >= 1 && thrown < 12; ++thrown) {
    u.trail -= 1;
    const Vec3 at = position + Rotate(orientation, {(Random() * 2 - 1) * halfX * 0.8f, 0, lo.z});
    Puff p;
    p.kind = 1;
    p.x = at.x;
    p.z = at.z;
    p.y = waterLevel;
    p.lift = 0.08f;
    p.velocity = u.velocity * 0.3f - forward * 0.8f + side * ((Random() - 0.5f) * 0.8f) + Vec3{0, 1.0f + 0.8f * Random(), 0};
    p.born = clock_ - Random() * dt;
    p.life = 0.9f + 0.5f * Random();
    p.size0 = std::clamp(0.25f * width, 0.1f, 1.5f);
    p.size1 = std::clamp(0.75f * width, 0.3f, 4.0f) * (0.8f + 0.4f * Random());
    p.strength = 0.24f * most * std::min(sprayStrength, 1.5f);
    p.seed = Random();
    Throw(p);
  }
}

void MovementDust::Contrails(uint32_t id, Unit& u, const Model* model, Vec3 position, const Quat& orientation, float speed) {
  const Kind& kind = *u.kind;
  if (kind.resolvedFor != model) {  // (its engines' bones in this model, found by their names once)
    kind.resolvedFor = model;
    kind.engineBones.clear();
    for (const std::string& e : kind.engines)
      for (size_t b = 0; b < model->bones.size(); ++b)
        if (Lower(model->bones[b].name) == e) {
          kind.engineBones.push_back(static_cast<int>(b));
          break;
        }
  }
  // where its engines are (as the model stands at rest: an aircraft's engines do not move on it)
  Vec3 engines[4];
  int n = 0;
  for (int b : kind.engineBones)
    if (n < 4) engines[n++] = position + Rotate(orientation, model->bones[static_cast<size_t>(b)].position * model->scale);
  if (n == 0) {  // (none named: two at its back, a third of its width out from its middle)
    const float across = (model->boundsMax.x - model->boundsMin.x) * model->scale / 3;
    const float y = (model->boundsMin.y + model->boundsMax.y) * 0.5f * model->scale, back = model->boundsMin.z * model->scale;
    engines[n++] = position + Rotate(orientation, {-across, y, back});
    engines[n++] = position + Rotate(orientation, {across, y, back});
  }
  // how strongly: by how high over the ground or the water (from contrailFrom up), and how near its top speed it flies
  const float surface = std::max(groundHeight ? groundHeight(position.x, position.z) : -1e9f, waterLevel);
  const float strength = Smooth(contrailFrom - 1, contrailFrom + 3, position.y - surface) * Smooth(0.35f, 0.7f, speed / std::max(kind.maxSpeed, 1.0f)) *
                         std::min(contrailStrength, 2.0f);
  for (int e = 0; e < 4; ++e) {
    int& slot = u.trails[e];
    Trail* t = slot >= 0 && slot < static_cast<int>(trails_.size()) && trails_[static_cast<size_t>(slot)].live && trails_[static_cast<size_t>(slot)].unit == id
                   ? &trails_[static_cast<size_t>(slot)]
                   : nullptr;
    if (e >= n || strength <= 0.02f) {  // (low or slow: it draws none, and lets go of the one it drew)
      if (t) t->live = false;
      slot = -1;
      continue;
    }
    if (!t) {
      if (liveTrails_ >= kMostTrails) continue;
      // (a place in the list: one whose contrail is all gone, else a new one)
      size_t free = trails_.size();
      for (size_t k = 0; k < trails_.size(); ++k)
        if (!trails_[k].live && trails_[k].points.empty()) {
          free = k;
          break;
        }
      if (free == trails_.size()) {
        if (trails_.size() >= static_cast<size_t>(2 * kMostTrails)) continue;
        trails_.emplace_back();
      }
      t = &trails_[free];
      t->unit = id;
      t->live = true;
      t->seed = Random() * 97;
      t->points.clear();
      t->points.push_back({engines[e], clock_, strength});
      slot = static_cast<int>(free);
      ++liveTrails_;
    }
    // the band follows the engine; a point is left behind every few world units, or every sixth of a second when slow
    t->head = {engines[e], clock_, strength};
    const Point& last = t->points.back();
    const float gone = Length(engines[e] - last.at);
    if (gone > 0.5f && (gone > 3.5f || clock_ - last.born > 0.16)) {
      t->points.push_back(t->head);
      while (t->points.size() > static_cast<size_t>(kTrailPoints)) t->points.pop_front();
    }
  }
}

bool MovementDust::Init() {
  if (tried_) return tried_ > 0;
  tried_ = -1;
  std::string error;
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "#define SAMPLES " + std::to_string(gSceneSamples) + "\n" +
                           kViewShaderBlock + kLightingShaderBlock;
  const std::string puffs = head + kPuffs, trails = head + kTrails;
  gfx::PipelineDesc pd;
  pd.name = "movement dust: puffs";
  pd.vertexSource = pd.pixelSource = puffs;
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;  // (the shaders test the scene's depth themselves)
  pd.depthTarget = false;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.blend = gfx::Blend::Alpha;
  pd.alphaWrite = false;  // (the original look's glow is left as it is, as the particles leave it)
  puffPipeline_ = device_->CreatePipeline(pd, &error);
  if (puffPipeline_) {
    pd.name = "movement dust: contrails";
    pd.vertexSource = pd.pixelSource = trails;
    trailPipeline_ = device_->CreatePipeline(pd, &error);
  }
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.stride = 16;
  bd.dynamic = true;
  if (trailPipeline_) rows_ = device_->CreateBuffer(bd);
  if (!rows_) {
    fprintf(stderr, "render: movement dust cannot be drawn (%s)\n", error.empty() ? "no buffer" : error.c_str());
    return false;
  }
  tried_ = 1;
  return true;
}

void MovementDust::Prepare(const View& view) {
  groundCount_ = sprayCount_ = segments_ = trailsDrawn_ = trailPoints_ = 0;
  if (!on_) return;
  // units not seen for a second are gone (dead, out of sight, the map changed): their contrails let go of
  for (auto it = unitsSeen_.begin(); it != unitsSeen_.end();) {
    if (clock_ - it->second.seen <= 1.0 && it->second.seen <= clock_) {
      ++it;
      continue;
    }
    for (int slot : it->second.trails)
      if (slot >= 0 && slot < static_cast<int>(trails_.size()) && trails_[static_cast<size_t>(slot)].unit == it->first)
        trails_[static_cast<size_t>(slot)].live = false;
    it = unitsSeen_.erase(it);
  }
  // what is over: the puffs older than the longest life (the others over are passed by below), the contrails' oldest points
  while (!puffs_.empty() && clock_ - puffs_.front().born > 6.0) puffs_.pop_front();
  liveTrails_ = 0;
  for (Trail& t : trails_) {
    while (!t.points.empty() && clock_ - t.points.front().born > trailLife_) t.points.pop_front();
    if (t.points.empty()) t.live = false;
    if (t.live) ++liveTrails_;
  }
  if (!Init()) return;  // (its shaders made the first frame it is on, not at the first puff: no stall in a fight)
  pixelSpan_ = 2 * std::tan(view.fovY * 0.5f) / static_cast<float>(std::max(view.height, 1));
  rowData_.clear();
  const float drift = std::fabs(wind[0]) + std::fabs(wind[1]);
  auto list = [&](bool spray) {
    int n = 0;
    for (const Puff& p : puffs_) {
      if ((p.kind == 1) != spray) continue;
      const float age = static_cast<float>(clock_ - p.born);
      if (age < 0 || age >= p.life) continue;
      // (seen at all, wherever its throw and the wind may have carried it by now?)
      const float reach = p.size1 + Length(p.velocity) * 0.5f + drift * age;
      if (!view.BoxVisible({p.x - reach, p.y - 1, p.z - reach}, {p.x + reach, p.y + p.lift + reach + 1, p.z + reach})) continue;
      rowData_.insert(rowData_.end(), {p.x, p.lift, p.z, age, p.velocity.x, p.velocity.y, p.velocity.z, p.life, p.size0, p.size1, p.strength,
                                       static_cast<float>(p.kind) + std::min(p.seed, 0.999f)});
      ++n;
    }
    return n;
  };
  groundCount_ = list(false);
  sprayFirst_ = groundCount_ * 3;
  sprayCount_ = list(true);
  trailFirst_ = (groundCount_ + sprayCount_) * 3;
  const float aloft = (std::fabs(windAloft[0]) + std::fabs(windAloft[1])) * trailLife_ + trailWide_;
  for (const Trail& t : trails_) {
    if (t.points.empty()) continue;
    const bool head = t.live && Length(t.head.at - t.points.back().at) > 0.05f;
    const size_t n = t.points.size() + (head ? 1 : 0);
    if (n < 2) continue;
    Vec3 lo = head ? t.head.at : t.points.front().at, hi = lo;
    for (const Point& p : t.points) {
      lo = {std::min(lo.x, p.at.x), std::min(lo.y, p.at.y), std::min(lo.z, p.at.z)};
      hi = {std::max(hi.x, p.at.x), std::max(hi.y, p.at.y), std::max(hi.z, p.at.z)};
    }
    if (!view.BoxVisible(lo - Vec3{aloft, aloft, aloft}, hi + Vec3{aloft, aloft, aloft})) continue;
    for (size_t k = 0; k < n; ++k) {
      const Point& p = k < t.points.size() ? t.points[k] : t.head;
      rowData_.insert(rowData_.end(), {p.at.x, p.at.y, p.at.z, static_cast<float>(clock_ - p.born), k + 1 < n ? 1.0f : 0.0f, p.strength, t.seed, 0.0f});
    }
    trailPoints_ += static_cast<int>(n);
    segments_ += static_cast<int>(n) - 1;
    ++trailsDrawn_;
  }
  if (!rowData_.empty()) device_->UpdateBuffer(rows_, rowData_.data(), static_cast<uint32_t>(rowData_.size() * sizeof(float)));
  if (debug_ >= 2 && clock_ - debugAt_ >= 1.0) {
    debugAt_ = clock_;
    printf("render: movement dust at %.1f s: %d units, %zu puffs kept, %d under the units and %d spray drawn, %d contrails (%d pieces); dry %.2f, damp %.2f\n",
           clock_, units, puffs_.size(), groundCount_, sprayCount_, trailsDrawn_, segments_, dryness_, damp_);
  }
}

void MovementDust::Draw(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                        float mapWidth, float mapHeight, bool lit, int first, int count) {
  if (count <= 0 || !heights || tried_ <= 0) return;
  // the camera's sideways and upward directions, from the rows of the view-projection matrix (as the particles take them)
  const Mat4& m = view.constants.viewProj;
  const Vec3 r = Normalize(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}), up = Normalize(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]});
  const float k[20] = {r.x, r.y, r.z, zNear, up.x, up.y, up.z, heightUnit, mapWidth, mapHeight, waterLevel, static_cast<float>(first),
                       wind[0], wind[1], lit ? 1.0f : 0.0f, groundColours ? 1.0f : 0.0f, farthest * 0.6f, 1 / (farthest * 0.4f), 0, 0};
  device_->SetPipeline(puffPipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k, sizeof k);
  device_->SetShaderBuffer(rows_);
  device_->SetTexture(0, heights);
  device_->SetTexture(1, groundColours);
  device_->SetTexture(3, sceneDepth);
  device_->Draw(6, 0, static_cast<uint32_t>(count));
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
  device_->SetTexture(3, {});
}

void MovementDust::DrawUnder(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                             float mapWidth, float mapHeight, bool lit) {
  Draw(view, sceneDepth, zNear, heights, heightUnit, groundColours, mapWidth, mapHeight, lit, 0, groundCount_);
}

void MovementDust::DrawAbove(const View& view, gfx::Texture sceneDepth, float zNear, gfx::Texture heights, float heightUnit, gfx::Texture groundColours,
                             float mapWidth, float mapHeight, bool lit) {
  if (!drawUnder) Draw(view, sceneDepth, zNear, heights, heightUnit, groundColours, mapWidth, mapHeight, lit, 0, groundCount_);
  Draw(view, sceneDepth, zNear, heights, heightUnit, groundColours, mapWidth, mapHeight, lit, sprayFirst_, sprayCount_);
  if (trailPoints_ <= 0 || tried_ <= 0) return;
  // (fresh: as wide as an eighth of a unit; at the last as wide as the damp air spreads it; showing about a third, more
  // in damp air; seen from 600 units off thinning out over another 600)
  const float k[16] = {static_cast<float>(trailFirst_), trailLife_, 0.12f, trailWide_, 0.34f * (0.7f + 0.3f * std::min(damp_, 1.0f)), 0.45f,
                       windAloft[0], windAloft[1], pixelSpan_, zNear, 600.0f, 1 / 600.0f, static_cast<float>(trailPoints_), 0, 0, 0};
  device_->SetPipeline(trailPipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k, sizeof k);
  device_->SetShaderBuffer(rows_);
  device_->SetTexture(3, sceneDepth);
  device_->Draw(6, 0, static_cast<uint32_t>(trailPoints_));
  device_->SetTexture(3, {});
}

// --- the viewer's test ---

bool MovementDustTest::Init(const std::string& spec, Vec3 at, float radius, const std::function<bool(const std::string&, BpValue*)>& readBlueprint,
                            std::string* error) {
  uint32_t r = 4242;
  auto next = [&r]() {
    r = r * 1664525u + 1013904223u;
    return static_cast<float>(r >> 8) / 16777216.0f;
  };
  for (size_t from = 0; from < spec.size();) {
    const size_t comma = spec.find(',', from);
    const std::string item = spec.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
    from = comma == std::string::npos ? spec.size() : comma + 1;
    const size_t eq = item.find('=');
    const std::string id = Lower(item.substr(0, eq));
    const int count = eq == std::string::npos ? 10 : atoi(item.c_str() + eq + 1);
    BpValue bp;
    if (id.empty() || count <= 0 || !readBlueprint || !readBlueprint(id, &bp)) {
      if (error) *error += (error->empty() ? "" : "; ") + id + " cannot be read";
      continue;
    }
    const std::string motion = bp["Physics"]["MotionType"].StringOr(std::string());
    const bool air = motion == "RULEUMT_Air", hover = motion == "RULEUMT_Hover";
    const float speed = static_cast<float>(air ? bp["Air"]["MaxAirspeed"].NumberOr(15) : bp["Physics"]["MaxSpeed"].NumberOr(3));
    const float elevation = static_cast<float>(bp["Physics"]["Elevation"].NumberOr(air ? 15 : 0));
    blueprints_.push_back(id);
    for (int i = 0; i < count; ++i) {
      Driver d;
      d.blueprint = static_cast<uint32_t>(blueprints_.size() - 1);
      const float a = next() * 6.2831853f, far = radius * std::sqrt(next());
      d.x = at.x + std::cos(a) * far;
      d.z = at.z + std::sin(a) * far;
      // (aircraft round wide loops: they are fast; the others round small ones)
      d.radius = air ? 18 + 14 * next() : 2.5f + 5.5f * next();
      d.speed = speed * (0.85f + 0.15f * next());
      d.angle = next() * 6.2831853f;
      d.turn = next() < 0.5f ? 1.0f : -1.0f;
      d.elevation = elevation;
      d.air = air;
      d.hover = hover;
      drivers_.push_back(d);
    }
    char b[128];
    snprintf(b, sizeof b, "%s%d of %s (%s, %.1f a second)", describe_.empty() ? "" : ", ", count, id.c_str(), air ? "flying" : hover ? "hovering" : "on land",
             speed);
    describe_ += b;
  }
  frame_.blueprints = &blueprints_;
  return !drivers_.empty();
}

const Frame& MovementDustTest::Update(double clock, const std::function<float(float, float)>& ground, float water) {
  frame_.units.clear();
  for (size_t k = 0; k < drivers_.size(); ++k) {
    const Driver& d = drivers_[k];
    // (where it is on its loop follows from the clock alone: a paused clock, a unit standing)
    const float angle = d.angle + d.turn * d.speed / d.radius * static_cast<float>(clock);
    Vec3 p{d.x + std::cos(angle) * d.radius, 0, d.z + std::sin(angle) * d.radius};
    const float g = ground(p.x, p.z);
    p.y = d.air || d.hover ? std::max(g, water) + d.elevation : g;
    // facing along the loop (a model's front is its +z)
    const float heading = std::atan2(-std::sin(angle) * d.turn, std::cos(angle) * d.turn);
    FrameUnit u;
    u.id = static_cast<uint32_t>(k + 1);
    u.blueprint = d.blueprint;
    u.position[0] = p.x, u.position[1] = p.y, u.position[2] = p.z;
    u.orientation[1] = std::sin(heading * 0.5f);
    u.orientation[3] = std::cos(heading * 0.5f);
    u.health = u.maxHealth = 1;
    u.layer = d.air ? 0x10u : d.hover && water > g ? 8u : 1u;
    frame_.units.push_back(u);
  }
  return frame_;
}

}  // namespace render
