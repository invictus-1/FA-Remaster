#include "beyond_land.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "jobs.h"
#include "lighting.h"
#include "terrain.h"

namespace render {
namespace {

// The land's surface: placed by its points (the ring's heights in a texture for the shadows), coloured from the map's
// ground, lit as the ground is, and lost in the far distance's haze.
const char kShader[] = R"hlsl(
cbuffer Land : register(b1) {
  float4 landRing;  // x, y the map's middle (world x, z); z the innermost ring's radius; w log(outermost / innermost)
  float4 landGrid;  // x points round, y rings out; z the sea's level; w the highest the land rises over it
  float4 landLook;  // x the haze's 1/e distance; y the snowline over the sea with no snow lying; z where the land may begin
                    // (world units past the map's edge); w over how far past that it comes out of the haze
  float4 landMore;  // x the shadows' softness; y 1 = the map's colours were gathered (palette), 0: the colours below;
                    // z the height over the sea over which the haze thins to 1/e; w the weather's thick air (per world unit)
};
Texture2D<float> ringHeights : register(t0);  // the ring's heights: its points round across, its rings out down
Texture2D palette : register(t1);             // 4 x 1: the map's ground low by the water, level, high, steep (as stored)

struct VIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float2 land : TEXCOORD0;  // x how far inland (0 at the coast .. 1), y how much of it is a mountain's rock
};
struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float3 land : TEXCOORD2;  // x, y as above; z the sun's light (1) or its shadow (0)
};

// The ring's height at a place (bilinear between its points, read one by one: a 32-bit float picture is not filtered
// by every card); off the ring: the sea's floor.
float RingHeight(float2 xz) {
  float2 p = xz - landRing.xy;
  float v = log(max(length(p), 1e-3) / landRing.z) / landRing.w;
  [branch] if (v <= 0 || v >= 1) return landGrid.z - 2;
  float across = atan2(p.y, p.x) * (landGrid.x / 6.2831853), along = v * (landGrid.y - 1);
  float a0 = floor(across), ta = across - a0;
  int count = int(landGrid.x), ia = int(a0 - landGrid.x * floor(a0 / landGrid.x)) % count, ib = (ia + 1) % count;
  int i0 = min(int(along), int(landGrid.y) - 2);
  float ti = along - i0;
  float h00 = ringHeights.Load(int3(ia, i0, 0)), h10 = ringHeights.Load(int3(ib, i0, 0));
  float h01 = ringHeights.Load(int3(ia, i0 + 1, 0)), h11 = ringHeights.Load(int3(ib, i0 + 1, 0));
  return lerp(lerp(h00, h10, ta), lerp(h01, h11, ta), ti);
}

// The sun's light at a point of the land: steps toward the sun, each farther than the one before, the ring's height
// there against the line's; how near the line comes to the ground for its distance softens the shadow's edge.
float LandShadow(float3 p, float3 n) {
  float3 L = sunDirection;
  if (L.y < 0.02) return 1;  // (the sun at the horizon or under it: its light is gone anyway)
  p += n * 1.5;
  float lit = 1, t = 12;
  [loop] for (int k = 0; k < 20; ++k) {
    float3 q = p + L * t;
    if (q.y > landGrid.z + landGrid.w) break;  // (above the highest ridge: nothing farther off is in the way)
    lit = min(lit, (q.y - RingHeight(q.xz)) / (t * landMore.x));
    if (lit <= 0) break;
    t *= 1.32;
  }
  return smoothstep(0, 1, saturate(lit));
}

VOut VS(VIn v) {
  VOut o;
  o.world = v.position;
  o.position = mul(viewProj, float4(v.position, 1));
  o.normal = v.normal;
  o.land = float3(v.land, LandShadow(v.position, v.normal));
  return o;
}

// What the land goes over into far off, toward `d`: below the horizon what the sky's pass shows there (the far
// distance the world beyond the edge is lost in, lighting.h's BeyondBack); above it the sky's own haze, as the sky's
// pass goes over from the one to the other (sky.cpp), so that a far ridge is lost in what is behind it.
float3 LandFar(float3 d) {
  float3 back = BeyondBack(d);
  float below = saturate((0.01 - d.y) * 60);
  [branch] if (below >= 1) return back;
  float3 sky;
  if (skyZenith.w > 0.5) {
    sky = HorizonHaze(d) * pow(max(skyGlow.w, 0.006), 0.43);
  } else {
    float3 picture = environmentSky.SampleLevel(samplerLinear, d, 0).rgb;
    sky = look > 0.5 ? pow(max(picture, 0), 2.2) * (lightMultiplier * modern.x) : picture;
  }
  [branch] if (air.x > 0.5) {
    float4 a = AirAt(cameraPosition + d * (air.y * exp(air.z) * 0.999));
    sky = sky * a.a + a.rgb;
  }
  return lerp(sky, back, below);
}

float4 PS(VOut i) : SV_Target {
  float3 d = normalize(i.world - cameraPosition);
  float3 far = LandFar(d);
  float over = i.world.y - landGrid.z;  // (over the sea)
  [branch] if (over <= 0) return float4(far, 1);  // (the sea out there: the far distance, as round it)
  float3 N = normalize(i.normal);
  // its colours: the map's own ground where it is low by the water, level, high and steep, a little uneven
  float3 shore = float3(0.62, 0.58, 0.47), level = float3(0.36, 0.38, 0.26), high = float3(0.42, 0.40, 0.34), steep = float3(0.45, 0.44, 0.42);
  if (landMore.y > 0.5) {
    shore = palette.Load(int3(0, 0, 0)).rgb;
    level = palette.Load(int3(1, 0, 0)).rgb;
    high = palette.Load(int3(2, 0, 0)).rgb;
    steep = palette.Load(int3(3, 0, 0)).rgb;
  }
  float vary = CloudNoise(float3(i.world.xz * 0.004, 1.7)) * 0.6 + CloudNoise(float3(i.world.xz * 0.017, 4.1)) * 0.4;
  float3 c = lerp(level, high, saturate(smoothstep(0.05, 0.35, over / landGrid.w) * 0.85 + (vary - 0.5) * 0.6));
  c = lerp(shore, c, smoothstep(0.6, 3.5, over));
  c = lerp(c, steep, saturate(smoothstep(0.86, 0.62, N.y) + i.land.y * 0.5));
  c *= 0.86 + 0.28 * vary;
  // snow: on the highest ridges, and lower down the more of it lies; not on what is too steep to hold it
  float snowFrom = lerp(landLook.y, -6, saturate(weather.y)) + (vary - 0.5) * 30;
  float snowy = smoothstep(snowFrom - 10, snowFrom + 10, over) * smoothstep(0.45, 0.72, N.y);
  float3 albedo = lerp(ToLinear(c), float3(0.50, 0.53, 0.58), snowy);
  float3 lit = ModernLight(albedo, 0, lerp(0.92, 0.7, snowy), 0.75 + 0.25 * saturate(N.y), 0, N, i.world, i.land.z);
  // lost in the haze with the distance, and in the weather's thick air (half of it: the land far off is to be made
  // out still in snow, as faint shapes); the coast goes over into the sea's haze, and where the land begins it comes
  // out of the haze, so it has no rim
  // (the haze thins with the height over the sea: what lies between a high eye and the land far below it is less
  // than its distance says, so from the strategic view the land is still there)
  float low = max(over, 0), eye = max(cameraPosition.y - landGrid.z, 0), thins = landMore.z;
  float mean = abs(eye - low) > 1 ? thins * (exp(-low / thins) - exp(-eye / thins)) / (eye - low) : exp(-low / thins);
  float keep = exp(-length(i.world - cameraPosition) * mean * (1 / landLook.x + 0.5 * landMore.w));
  keep *= smoothstep(0, 2.5, over);
  keep *= smoothstep(landLook.z, landLook.z + landLook.w, OutsideMap(i.world));
  return float4(lerp(far, lit, keep), 1);
}
)hlsl";

// The map's colours where its ground is low by the water, level, high and steep (the palette the land far off is
// coloured from): Terrain's GroundColours by the map's heights there, summed by one group of threads.
const char kPalette[] = R"hlsl(
cbuffer Palette : register(b1) {
  float4 info;  // x the sea's level; y the map's highest ground; z the world units of a height texel's 1
};
Texture2D colours : register(t0);         // Terrain::GroundColours: over the map at half its size, as stored
Texture2D<float> heights : register(t1);  // Terrain::HeightTexture: one per grid point
RWTexture2D<float4> palette : register(u0);
groupshared float4 sum0[256], sum1[256], sum2[256], sum3[256];
[numthreads(256, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h, hw, hh;
  colours.GetDimensions(w, h);
  heights.GetDimensions(hw, hh);
  float4 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  float highFrom = info.x + (info.y - info.x) * 0.45;
  for (uint k = id.x; k < w * h; k += 256) {
    uint2 c = uint2(k % w, k / w);
    int2 at = int2(clamp(c * 2 + 1, uint2(1, 1), uint2(hw - 2, hh - 2)));
    float y = heights.Load(int3(at, 0)) * info.z;
    if (y < info.x - 0.3) continue;  // (under the water)
    float dx = (heights.Load(int3(at + int2(1, 0), 0)) - heights.Load(int3(at - int2(1, 0), 0))) * info.z * 0.5;
    float dz = (heights.Load(int3(at + int2(0, 1), 0)) - heights.Load(int3(at - int2(0, 1), 0))) * info.z * 0.5;
    float4 colour = float4(colours.Load(int3(c, 0)).rgb, 1);
    if (dx * dx + dz * dz > 0.36) s3 += colour;  // steeper than about 31 degrees
    else if (y < info.x + 2.5) s0 += colour;     // low by the water
    else if (y < highFrom) s1 += colour;         // level
    else s2 += colour;                           // high
  }
  sum0[id.x] = s0;
  sum1[id.x] = s1;
  sum2[id.x] = s2;
  sum3[id.x] = s3;
  GroupMemoryBarrierWithGroupSync();
  for (uint n = 128; n > 0; n >>= 1) {
    if (id.x < n) {
      sum0[id.x] += sum0[id.x + n];
      sum1[id.x] += sum1[id.x + n];
      sum2[id.x] += sum2[id.x + n];
      sum3[id.x] += sum3[id.x + n];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (id.x == 0) {
    float4 all = sum0[0] + sum1[0] + sum2[0] + sum3[0];
    float3 mean = all.a > 0 ? all.rgb / all.a : float3(0.4, 0.4, 0.35);
    palette[uint2(0, 0)] = float4(sum0[0].a > 0 ? sum0[0].rgb / sum0[0].a : mean, 1);
    palette[uint2(1, 0)] = float4(sum1[0].a > 0 ? sum1[0].rgb / sum1[0].a : mean, 1);
    palette[uint2(2, 0)] = float4(sum2[0].a > 0 ? sum2[0].rgb / sum2[0].a : mean, 1);
    palette[uint2(3, 0)] = float4(sum3[0].a > 0 ? sum3[0].rgb / sum3[0].a : mean, 1);
  }
}
)hlsl";

struct LandConstants {
  float ring[4], grid[4], look[4], more[4];
};
struct PaletteConstants {
  float info[4];
};
struct LandVertex {
  float position[3], normal[3], land[2];
};

// --- the noises the land is made of (the same on every machine: integer hashes and plain arithmetic) ---
uint32_t Mix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}
float Lattice(int x, int z, uint32_t seed) {
  return static_cast<float>(Mix(static_cast<uint32_t>(x) * 0x9e3779b1u ^ Mix(static_cast<uint32_t>(z) * 0x85ebca77u + seed)) & 0xffffffu) / 16777215.0f;
}
// Smooth noise 0..1 over a lattice of one unit.
float ValueNoise(float x, float z, uint32_t seed) {
  const float fx = std::floor(x), fz = std::floor(z);
  const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  float tx = x - fx, tz = z - fz;
  tx = tx * tx * tx * (tx * (tx * 6 - 15) + 10);
  tz = tz * tz * tz * (tz * (tz * 6 - 15) + 10);
  const float a = Lattice(ix, iz, seed), b = Lattice(ix + 1, iz, seed), c = Lattice(ix, iz + 1, seed), d = Lattice(ix + 1, iz + 1, seed);
  return a + (b - a) * tx + (c - a) * tz + (a - b - c + d) * tx * tz;
}
// Several of those, each twice as fine and half as strong as the one before and turned (so that the lattice's rows
// do not line up): about -1..1, mostly within -0.6..0.6.
float Fbm(float x, float z, uint32_t seed, int octaves) {
  float sum = 0, amp = 0.5f, all = 0;
  for (int o = 0; o < octaves; ++o) {
    sum += (ValueNoise(x, z, seed + static_cast<uint32_t>(o) * 1013u) * 2 - 1) * amp;
    all += amp;
    const float nx = 1.6f * x - 1.2f * z, nz = 1.2f * x + 1.6f * z;
    x = nx + 17.3f;
    z = nz - 9.1f;
    amp *= 0.5f;
  }
  return sum / all;
}
// Ridges: sharp crests where the noise crosses its middle, the finer octaves strongest on the crests (0..1).
float Ridged(float x, float z, uint32_t seed, int octaves) {
  float sum = 0, amp = 0.5f, all = 0, weight = 1;
  for (int o = 0; o < octaves; ++o) {
    float n = 1 - std::fabs(ValueNoise(x, z, seed + static_cast<uint32_t>(o) * 7919u) * 2 - 1);
    n *= n * weight;
    weight = std::clamp(n * 1.8f, 0.0f, 1.0f);
    sum += n * amp;
    all += amp;
    const float nx = 1.6f * x - 1.2f * z, nz = 1.2f * x + 1.6f * z;
    x = nx - 5.7f;
    z = nz + 3.9f;
    amp *= 0.5f;
  }
  return sum / all;
}
float Smooth(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

}  // namespace

bool BeyondLand::Set(const std::string& list, std::string* why) {
  struct Named {
    const char* name;
    float* value;
  } named[] = {{"band", &band}, {"edgeShadow", &edgeShadow}, {"edgeWidth", &edgeWidth}, {"edgeGrey", &edgeGrey}, {"haze", &haze}, {"hazeHeight", &hazeHeight},
               {"begin", &begin}, {"end", &end}, {"hills", &hills}, {"mountains", &mountains}, {"islands", &islands},
               {"farLand", &farLand}, {"snowline", &snowline}, {"softness", &softness}};
  size_t at = 0;
  while (at < list.size()) {
    size_t stop = list.find(',', at);
    if (stop == std::string::npos) stop = list.size();
    const std::string item = list.substr(at, stop - at);
    at = stop + 1;
    const size_t eq = item.find('=');
    bool found = false;
    for (const Named& n : named)
      if (eq != std::string::npos && item.compare(0, eq, n.name) == 0 && eq == std::strlen(n.name)) {
        *n.value = static_cast<float>(std::atof(item.c_str() + eq + 1));
        found = true;
      }
    if (!found) {
      *why = "not a name=value of band, edgeShadow, edgeWidth, edgeGrey, haze, hazeHeight, begin, end, hills, mountains, islands, farLand, snowline, "
             "softness: " + item;
      return false;
    }
  }
  return true;
}

void BeyondLand::Forget(const MapData& map, const std::string& path) {
  made_ = paletteMade_ = false;
  indexCount_ = 0;
  if (device_) {
    for (gfx::Buffer* b : {&vertices_, &indices_})
      if (*b) device_->Destroy(*b);
    for (gfx::Texture* t : {&heights_, &palette_})
      if (*t) device_->Destroy(*t);
  }
  vertices_ = indices_ = {};
  heights_ = palette_ = {};
  read_ = map.width > 0 && map.height > 0 && !map.heights.empty();
  if (!read_) return;
  const float W = static_cast<float>(map.width), H = static_cast<float>(map.height);
  float lowest = std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::lowest();
  uint32_t seed = 2166136261u;  // (from the map file's name, whatever folder it is in, and its size)
  for (char c : path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1))
    seed = (seed ^ static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)))) * 16777619u;
  seed = (seed ^ static_cast<uint32_t>(map.width)) * 16777619u;
  seed = (seed ^ static_cast<uint32_t>(map.height)) * 16777619u;
  for (uint16_t raw : map.heights) {
    const float h = static_cast<float>(raw) * map.heightScale;
    lowest = std::min(lowest, h);
    highest = std::max(highest, h);
  }
  seed_ = seed;
  highest_ = highest;
  sea_ = map.water.present ? map.water.elevation : lowest - 1;
  mapSize_[0] = W;
  mapSize_[1] = H;
  middle_[0] = W * 0.5f;
  middle_[1] = H * 0.5f;

  // What the edges hold: along them (a few cells in, past what a map's maker leaves at the very edge), land or sea,
  // and how high the land stands. Then for each of 256 directions from the map's middle: of the stretch of edge that
  // way (a tenth of the way round to either side), how much is land and how high, smoothed round the circle.
  const float perimeter = 2 * (W + H);
  const int along = std::max(512, static_cast<int>(perimeter / 2));
  std::vector<float> isLand(static_cast<size_t>(along)), lift(static_cast<size_t>(along));
  const float inset = std::min(4.0f, std::min(W, H) * 0.05f);
  for (int s = 0; s < along; ++s) {
    float p = (static_cast<float>(s) + 0.5f) / static_cast<float>(along) * perimeter, x, z;
    if (p < W) x = p, z = inset;                                   // the north edge, west to east
    else if (p < W + H) x = W - inset, z = p - W;                  // the east edge, north to south
    else if (p < 2 * W + H) x = W - (p - W - H), z = H - inset;    // the south edge, east to west
    else x = inset, z = H - (p - 2 * W - H);                       // the west edge, south to north
    const float h = map.Height(std::clamp(x, inset, W - inset), std::clamp(z, inset, H - inset));
    isLand[static_cast<size_t>(s)] = h > sea_ + 0.5f ? 1.0f : 0.0f;
    lift[static_cast<size_t>(s)] = std::clamp((h - sea_) / std::max(highest - sea_, 1.0f), 0.0f, 1.0f);
  }
  const int window = std::max(1, along / 10);
  for (int k = 0; k < kDirections; ++k) {
    const float angle = (static_cast<float>(k) + 0.5f) / kDirections * 6.2831853f;
    const float dx = std::cos(angle), dz = std::sin(angle);
    // where a line from the middle that way leaves the map, as a place along its edge
    const float tx = std::fabs(dx) > 1e-6f ? W * 0.5f / std::fabs(dx) : 1e30f, tz = std::fabs(dz) > 1e-6f ? H * 0.5f / std::fabs(dz) : 1e30f;
    const float t = std::min(tx, tz), x = middle_[0] + dx * t, z = middle_[1] + dz * t;
    float p;
    if (tz <= tx && dz < 0) p = x;
    else if (tx < tz && dx > 0) p = W + z;
    else if (tz <= tx) p = W + H + (W - x);
    else p = 2 * W + H + (H - z);
    const int s0 = static_cast<int>(p / perimeter * static_cast<float>(along));
    float land = 0, high = 0, weights = 0, landWeights = 0;
    for (int o = -window; o <= window; ++o) {
      const size_t s = static_cast<size_t>(((s0 + o) % along + along) % along);
      const float w = 1 - std::fabs(static_cast<float>(o)) / static_cast<float>(window + 1);
      land += isLand[s] * w;
      weights += w;
      high += lift[s] * isLand[s] * w;
      landWeights += isLand[s] * w;
    }
    landward_[static_cast<size_t>(k)] = land / weights;
    highness_[static_cast<size_t>(k)] = landWeights > 0 ? high / landWeights : 0;
  }
  {  // (smoothed round the circle: land beyond is a coast that turns, not a wedge)
    std::array<float, kDirections> a{}, b{};
    for (int k = 0; k < kDirections; ++k) {
      float sa = 0, sb = 0, sw = 0;
      for (int o = -12; o <= 12; ++o) {
        const size_t j = static_cast<size_t>(((k + o) % kDirections + kDirections) % kDirections);
        const float w = std::exp(-static_cast<float>(o * o) / 50.0f);
        sa += landward_[j] * w;
        sb += highness_[j] * w;
        sw += w;
      }
      a[static_cast<size_t>(k)] = sa / sw;
      b[static_cast<size_t>(k)] = sb / sw;
    }
    landward_ = a;
    highness_ = b;
  }
}

void BeyondLand::SetView(ViewConstants* c) {
  thickAir = 0;
  c->beyondLand[0] = std::max(edgeWidth, 1.0f);
  c->beyondLand[1] = std::max(band, 0.01f);
  c->beyondLand[2] = std::clamp(edgeShadow, 0.0f, 1.0f);
  c->beyondLand[3] = std::clamp(edgeGrey, 0.0f, 1.0f);
}

bool BeyondLand::MakePipelines() {
  // (no snow from ModernLight: its patches are laid at a size for the map, which far off would only flicker; the land
  // lays its own, by its height)
  const std::string source = std::string("#define SURFACE_SNOW 0.0\n") + kViewShaderBlock + kLightingShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.name = "the land far off";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}, {"NORMAL", 0, gfx::AttrType::Float3, 12}, {"TEXCOORD", 0, gfx::AttrType::Float2, 24}};
  pd.stride = sizeof(LandVertex);
  pd.cull = gfx::Cull::None;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  std::string error;
  pipeline_ = device_->CreatePipeline(pd, &error);
  if (!pipeline_) {
    fprintf(stderr, "render: the land far off cannot be drawn: %s\n", error.c_str());
    return false;
  }
  gfx::ComputeDesc cd;
  cd.name = "the land far off: the map's colours";
  const std::string palette = std::string(kViewShaderBlock) + kPalette;
  cd.source = palette;
  palettePass_ = device_->CreateCompute(cd, &error);
  if (!palettePass_) fprintf(stderr, "render: the land far off takes plain colours: %s\n", error.c_str());
  return true;
}

void BeyondLand::Prepare(gfx::Device* device, bool on, Terrain& terrain) {
  now_ = false;
  if (!on || failed_ || !read_ || terrain.beyondReach <= 0) return;
  device_ = device;
  if (!tried_) {
    tried_ = true;
    failed_ = !MakePipelines();
    if (failed_) return;
  }
  if (!made_) {
    Make(terrain.beyondReach);
    made_ = true;
  }
  if (!paletteMade_ && palettePass_ && heights_) {  // (once: it dispatches)
    paletteMade_ = true;
    const gfx::Texture colours = terrain.GroundColours();
    if (colours) {
      gfx::TargetDesc td;
      td.storage = true;
      td.width = 4;
      td.height = 1;
      td.format = gfx::Format::RGBA16F;
      palette_ = device_->CreateTarget(td);
    }
    if (palette_) {
      PaletteConstants c = {{sea_, highest_, terrain.HeightUnit(), 0}};
      device_->SetPipeline(palettePass_);
      device_->SetConstants(1, &c, sizeof c);
      device_->SetTexture(0, colours);
      device_->SetTexture(1, terrain.HeightTexture());
      device_->SetStorage(0, palette_);
      device_->Dispatch(1);
      device_->SetStorage(0, {});
      device_->SetTexture(0, {});
      device_->SetTexture(1, {});
    }
  }
  now_ = indexCount_ > 0;
}

// The land far off, made for the map read (Forget) the first time it is wanted: the ring's points (on the workers),
// which way land lies from what the map's edges hold, their normals, and the triangles that are above the sea.
void BeyondLand::Make(float reach) {
  const auto started = std::chrono::steady_clock::now();
  const float W = mapSize_[0], H = mapSize_[1];
  const float size = std::max(W, H);
  // (the land's features as large as the map's, within reason: round a small map they are not toys, round a huge one
  // not continents)
  const float scale = std::clamp(size, 384.0f, 2048.0f);
  const uint32_t seed = seed_;
  const std::array<float, kDirections>& landward = landward_;
  const std::array<float, kDirections>& highness = highness_;
  start_ = reach + std::max(begin, 0.0f) * size;
  inner_ = std::min(W, H) * 0.5f + start_;
  outer_ = std::max(inner_ * 1.5f, std::sqrt(W * W + H * H) * 0.5f + std::max(end, begin + 0.5f) * size);

  float landAround = 0;  // (how much of the map's edges is land, all round)
  for (float l : landward) landAround += l;
  landAround /= kDirections;

  // The ring's points: `round_` round, `rings_` out, each ring a little farther than the one before (the same share),
  // so that they lie about as far apart round as out, closer together near the map.
  round_ = 768;
  rings_ = 160;
  const float logRatio = std::log(outer_ / inner_);
  std::vector<float> height(static_cast<size_t>(round_) * static_cast<size_t>(rings_));
  std::vector<float> landShare(height.size()), rock(height.size());
  const float islandShare = std::max(islands, 0.0f);
  Jobs::Shared().Run(rings_, [&](int i) {
    const float r = inner_ * std::exp(logRatio * static_cast<float>(i) / static_cast<float>(rings_ - 1));
    const float outward = static_cast<float>(i) / static_cast<float>(rings_ - 1);
    for (int a = 0; a < round_; ++a) {
      const float angle = static_cast<float>(a) / static_cast<float>(round_) * 6.2831853f;
      const float x = middle_[0] + r * std::cos(angle), z = middle_[1] + r * std::sin(angle);
      // which way: the edges' land and its height that way (between the two nearest directions)
      const float at = angle / 6.2831853f * kDirections - 0.5f;
      const int k0 = static_cast<int>(std::floor(at));
      const float f = at - static_cast<float>(k0);
      const size_t j0 = static_cast<size_t>((k0 % kDirections + kDirections) % kDirections), j1 = (j0 + 1) % kDirections;
      // how far past the map's edge (its rectangle)
      const float ox = std::max({-x, x - W, 0.0f}), oz = std::max({-z, z - H, 0.0f});
      const float out = std::sqrt(ox * ox + oz * oz);
      // (far out, what the edges hold all round more than what they hold that way)
      const float farOut = std::clamp(farLand, 0.0f, 1.0f) * Smooth(0.15f, 0.7f, outward);
      float bias = landward[j0] + (landward[j1] - landward[j0]) * f;
      bias += (landAround - bias) * farOut;
      const float high = highness[j0] + (highness[j1] - highness[j0]) * f;
      const float u = x / scale, v = z / scale;
      // land or sea: the edges' land that way, with coasts and bays from a broad noise; out at sea, here and there the
      // highest of a finer noise rises above it as an island, as often as `islands` says. None before `start_`; from
      // there the land comes in over most of a map's size, so that its coasts are the noise's and not a line along the
      // map's edge; and sea again at the ring's far end, where the land sinks away.
      const float wide = Fbm(u * 0.45f, v * 0.45f, seed, 5);
      const float mainland = (bias - 0.5f) * 1.7f + wide * 1.4f;
      float continent = mainland;
      if (islandShare > 0) continent = std::max(continent, (Fbm(u * 1.2f, v * 1.2f, seed + 53u, 4) - 0.5f + 0.08f * islandShare) * 3.0f);
      continent -= 1.2f * (1 - Smooth(0, 0.5f * scale, out - start_));
      continent -= 3.0f * (1 - Smooth(-0.15f * scale, 0, out - start_));
      continent -= 2.0f * Smooth(0.8f, 1.0f, outward);
      float h, inland = 0, rocky = 0;
      if (continent > 0) {
        inland = Smooth(0, 0.6f, continent);
        const float rolling = hills * inland * (0.35f + 0.65f * (Fbm(u * 3.2f, v * 3.2f, seed + 11u, 5) * 0.5f + 0.5f));
        // ridges of mountains inland (on the mainland: an island has hills), more of them farther out and where the
        // map's edge stands high, the peaks of a range not all as high
        const float farther = std::clamp((out - start_) / (1.5f * scale), 0.0f, 1.0f);
        const float range = Smooth(0.45f, 0.75f, Fbm(u * 0.6f, v * 0.6f, seed + 31u, 3) * 0.5f + 0.5f + (high - 0.3f) * 0.4f + farther * 0.25f);
        const float massif = 0.4f + 0.6f * Smooth(0.3f, 0.8f, Fbm(u * 0.35f, v * 0.35f, seed + 41u, 3) * 0.5f + 0.5f);
        const float ridge = Ridged(u * 1.7f, v * 1.7f, seed + 23u, 6);
        const float mountain = mountains * std::pow(ridge, 1.6f) * range * massif * Smooth(0.3f, 0.9f, std::min(inland, Smooth(0, 0.6f, mainland)));
        h = sea_ + 1.2f + rolling + mountain;
        rocky = std::clamp(mountain / std::max(mountains * 0.5f, 1.0f), 0.0f, 1.0f);
      } else {
        h = sea_ - 2 - 20 * std::min(-continent, 1.0f);  // (the sea's floor: not drawn; the shadows' march reads it)
      }
      const size_t n = static_cast<size_t>(i) * static_cast<size_t>(round_) + static_cast<size_t>(a);
      height[n] = h;
      landShare[n] = inland;
      rock[n] = rocky;
    }
  });

  // the points, with their normals from their neighbours round and out
  auto place = [&](int a, int i) {
    a = (a % round_ + round_) % round_;
    i = std::clamp(i, 0, rings_ - 1);
    const float r = inner_ * std::exp(logRatio * static_cast<float>(i) / static_cast<float>(rings_ - 1));
    const float angle = static_cast<float>(a) / static_cast<float>(round_) * 6.2831853f;
    return Vec3{middle_[0] + r * std::cos(angle), std::max(height[static_cast<size_t>(i) * static_cast<size_t>(round_) + static_cast<size_t>(a)], sea_ - 2), middle_[1] + r * std::sin(angle)};
  };
  std::vector<LandVertex> points(height.size());
  for (int i = 0; i < rings_; ++i)
    for (int a = 0; a < round_; ++a) {
      const Vec3 p = place(a, i);
      Vec3 n = Cross(place(a + 1, i) - place(a - 1, i), place(a, i + 1) - place(a, i - 1));
      if (n.y < 0) n = n * -1.0f;
      n = Normalize(n);
      const size_t k = static_cast<size_t>(i) * static_cast<size_t>(round_) + static_cast<size_t>(a);
      points[k] = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, {landShare[k], rock[k]}};
    }
  // the triangles that have a corner above the sea
  std::vector<uint32_t> list;
  float highestMade = sea_;
  for (int i = 0; i + 1 < rings_; ++i)
    for (int a = 0; a < round_; ++a) {
      const uint32_t a1 = static_cast<uint32_t>((a + 1) % round_), r0 = static_cast<uint32_t>(i * round_), r1 = r0 + static_cast<uint32_t>(round_);
      const uint32_t q[4] = {r0 + static_cast<uint32_t>(a), r0 + a1, r1 + static_cast<uint32_t>(a), r1 + a1};
      float most = -1e30f;
      for (uint32_t c : q) most = std::max(most, points[c].position[1]);
      if (most <= sea_ + 0.05f) continue;
      highestMade = std::max(highestMade, most);
      for (uint32_t c : {q[0], q[2], q[1], q[1], q[2], q[3]}) list.push_back(c);
    }
  top_ = std::max(highestMade - sea_, 1.0f);
  indexCount_ = static_cast<uint32_t>(list.size());
  if (!list.empty()) {
    gfx::BufferDesc vb;
    vb.stride = sizeof(LandVertex);
    vb.size = static_cast<uint32_t>(points.size() * sizeof(LandVertex));
    vb.data = points.data();
    vertices_ = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 4;
    ib.size = static_cast<uint32_t>(list.size() * sizeof(uint32_t));
    ib.data = list.data();
    indices_ = device_->CreateBuffer(ib);
    gfx::TextureDesc td;
    td.width = round_;
    td.height = rings_;
    td.format = gfx::Format::R32F;
    td.data = height.data();
    td.dataSize = height.size() * sizeof(float);
    heights_ = device_->CreateTexture(td);
    if (!vertices_ || !indices_ || !heights_) indexCount_ = 0;
  }
  printf("render: the land far off: %u triangles, from %.0f to %.0f world units round the map's middle, the map's edges %.0f%% land "
         "(made in %.0f ms)\n",
         indexCount_ / 3, inner_, outer_, 100 * landAround,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void BeyondLand::Draw(const View& view) {
  if (!now_ || !indexCount_) return;
  LandConstants c = {};
  c.ring[0] = middle_[0];
  c.ring[1] = middle_[1];
  c.ring[2] = inner_;
  c.ring[3] = std::log(outer_ / inner_);
  c.grid[0] = static_cast<float>(round_);
  c.grid[1] = static_cast<float>(rings_);
  c.grid[2] = sea_;
  c.grid[3] = top_;
  c.look[0] = std::max(haze, 1.0f);
  c.look[1] = std::max(mountains, 1.0f) * snowline;
  c.look[2] = start_;
  c.look[3] = 0.15f * std::max(mapSize_[0], mapSize_[1]);
  c.more[0] = std::max(softness, 1e-3f);
  c.more[1] = palette_ && paletteMade_ ? 1.0f : 0.0f;
  c.more[2] = std::max(hazeHeight, 1.0f);
  c.more[3] = std::max(thickAir, 0.0f);
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, heights_);
  device_->SetTexture(1, palette_ ? palette_ : heights_);
  device_->SetVertexBuffer(vertices_);
  device_->SetIndexBuffer(indices_);
  device_->DrawIndexed(indexCount_);
}

}  // namespace render
