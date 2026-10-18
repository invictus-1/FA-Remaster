#include "bounce.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "ray_scene.h"

namespace render {
namespace {

// What the passes share: this frame's turn of the rays' directions, the map's size, the frame's number, how far a ray
// looks, what a probe keeps; which probe a picture's tile holds now, and its number as the grid knows it (to tell a
// probe that has come in from one that was there).
const char kCommon[] = R"hlsl(
cbuffer Bounce : register(b1) {
  float4 turn0, turn1, turn2;  // the rays' turn this frame (rows)
  float4 map;                  // x, y: the map's width and height; z: the frame's number; w: how far a ray looks
  float4 blend;                // x: what a probe keeps of what it held; y: the spacing's step (in a probe's number)
};
float3 RayDirection(uint i) {  // ray i of 64: evenly over the sphere (spherical Fibonacci), turned
  float y = 1 - (2 * i + 1) / 64.0, r = sqrt(saturate(1 - y * y)), phi = i * 2.39996323;
  float3 d = float3(cos(phi) * r, y, sin(phi) * r);
  return normalize(float3(dot(turn0.xyz, d), dot(turn1.xyz, d), dot(turn2.xyz, d)));
}
int2 ColumnOfTile(int2 tile) {  // the grid's column a tile holds now
  int2 first = int2(bounce.zw);
  return first + (((tile - first) % 32) + 32) % 32;
}
uint Identity(int2 column) {  // (its column and the spacing's step: a probe of another is not the same probe)
  return (uint(column.x) & 0x3fff) | ((uint(column.y) & 0x3fff) << 14) | ((uint(blend.y) & 3) << 28) | 0x80000000u;
}
)hlsl";

// Each probe's rays (a thread each: x the ray, y the probe): what they meet lit by the sun and by the probes' light of
// the frame before, the sky where they meet nothing; with how far they went.
const char kTrace[] = R"hlsl(
TextureCube mapSkyCube : register(t29);  // (the map's own sky, where there is no computed one)
RWTexture2D<float4> rayResults : register(u0);
[numthreads(64, 1, 1)] void Trace(uint3 id : SV_DispatchThreadID) {
  uint ray = id.x, probe = id.y;
  int2 tile = int2(probe % 32, (probe / 32) % 32);
  int layer = probe / 1024;
  float3 from = BounceProbe(ColumnOfTile(tile), layer), d = RayDirection(ray);
  uint seed = RayHash(probe * 64 + ray + uint(map.z) * 7919);
  RayHit met;
  float t;
  float3 radiance;
  bool hit = RayTrace(from, d, 0.05, map.w, 0xff, seed, met);
  t = met.t;
  if (hit && !met.front && RayInstance(met.instance).a > 1.5) {
    radiance = 0;  // (the back of something, not a leaf: the probe may be inside it; its distance told as negative)
    t = -t;
  } else if (hit) {
    float3 p = from + d * t, albedo, n;
    RaySurfaceAt(met, p, d, map.xy, albedo, n);
    radiance = RayLitAt(p, n, albedo, seed, 48);  // (round about by the probes of the frame before: light bounces again)
  } else {
    t = map.w;
    radiance = (skyZenith.w > 0.5 ? ComputedSky(d) : RayLinear(mapSkyCube.SampleLevel(samplerLinear, d, 3).rgb)) * modern.y;
  }
  rayResults[uint2(ray, probe)] = float4(radiance, t);
}
)hlsl";

// A probe's light (a thread for each texel inside its 6 x 6) or sight (14 x 14): its rays toward that side, each by
// how much it faces it (the light) or sharply (the sight: its distance and its square), blended with what it held -
// nothing of that where the probe has just come into the grid.
const char kBlend[] = R"hlsl(
Texture2D rayResults : register(t3);
Texture2D<uint> identities : register(t4);
#ifdef SIGHT
RWTexture2D<float2> atlas : register(u0);
static const uint kInside = 14, kTile = 16;
#else
RWTexture2D<float4> atlas : register(u0);
static const uint kInside = 6, kTile = 8;
#endif
[numthreads(8, 8, 1)] void Blend(uint3 id : SV_DispatchThreadID) {
  uint2 tile = id.xy / kInside, local = id.xy % kInside;
  if (tile.x >= 32 || tile.y >= 128) return;
  uint probe = tile.x + tile.y * 32;
  float3 dir = BounceDir((local + 0.5) / kInside * 2 - 1);
  float3 sum = 0;
  float weight = 0, backs = 0;
  for (uint r = 0; r < 64; ++r) {
    float4 got = rayResults.Load(int3(r, probe, 0));
    float facing = max(dot(dir, RayDirection(r)), 0);
    backs += got.a < 0 ? 1 : 0;
#ifdef SIGHT
    float w = pow(facing, 50);
    float d = min(got.a < 0 ? -got.a * 0.2 : got.a, map.w);  // (a back seen: a wall close by)
    sum += float3(d, d * d, 0) * w;
#else
    float w = facing;
    sum += got.rgb * w;
#endif
    weight += w;
  }
  float outside = backs > 16 ? 0 : 1;  // (more than a quarter of its rays met backs: the probe is inside something)
  bool fresh = identities.Load(int3(tile, 0)) != Identity(ColumnOfTile(int2(tile.x, tile.y % 32)));
  float keep = fresh ? 0 : blend.x;
  uint2 at = tile * kTile + 1 + local;
#ifdef SIGHT
  float2 now = weight > 0 ? sum.xy / weight : float2(map.w, map.w * map.w);
  atlas[at] = lerp(now, atlas[at], keep);
#else
  float3 now = weight > 0 ? sum / weight : 0;
  atlas[at] = float4(lerp(now, atlas[at].rgb, keep), outside);
#endif
}
)hlsl";

// The border round each probe's tile (for reading it smoothly): each border texel the inside texel that lies next to
// it across the octahedral square's edge.
const char kBorder[] = R"hlsl(
#ifdef SIGHT
RWTexture2D<float2> atlas : register(u0);
static const int kInside = 14, kTile = 16;
#else
RWTexture2D<float4> atlas : register(u0);
static const int kInside = 6, kTile = 8;
#endif
[numthreads(8, 8, 1)] void Border(uint3 id : SV_DispatchThreadID) {
  int2 tile = id.xy / kTile, p = id.xy % kTile;
  if (tile.x >= 32 || tile.y >= 128) return;
  const int n = kInside, last = kInside + 1;
  bool edgeX = p.x == 0 || p.x == last, edgeY = p.y == 0 || p.y == last;
  if (!edgeX && !edgeY) return;
  int2 from;
  if (edgeX && edgeY) from = int2(p.x == 0 ? n : 1, p.y == 0 ? n : 1);
  else if (edgeY) from = int2(last - p.x, p.y == 0 ? 1 : n);
  else from = int2(p.x == 0 ? 1 : n, last - p.y);
  atlas[tile * kTile + p] = atlas[tile * kTile + from];
}
)hlsl";

// Each probe's number, kept for the next frame (a thread each).
const char kKeep[] = R"hlsl(
RWTexture2D<uint> identities : register(u0);
[numthreads(8, 8, 1)] void Keep(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 32 || id.y >= 128) return;
  identities[id.xy] = Identity(ColumnOfTile(int2(id.x, id.y % 32)));
}
)hlsl";

// A turn of directions, different every frame (from its number): rows of a rotation about an axis.
void Turn(uint32_t frame, float rows[3][4]) {
  auto hash = [](uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x & 0xffffff) / 16777216.0f;
  };
  const float u = hash(frame * 3 + 1), v = hash(frame * 3 + 2), w = hash(frame * 3 + 3);
  const float z = 2 * u - 1, r = std::sqrt(std::max(1 - z * z, 0.0f)), phi = 6.2831853f * v;
  const Vec3 axis{r * std::cos(phi), r * std::sin(phi), z};
  const float angle = 6.2831853f * w, c = std::cos(angle), s = std::sin(angle), t = 1 - c;
  const float m[3][3] = {{t * axis.x * axis.x + c, t * axis.x * axis.y - s * axis.z, t * axis.x * axis.z + s * axis.y},
                         {t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z - s * axis.x},
                         {t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x, t * axis.z * axis.z + c}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) rows[i][j] = m[i][j];
    rows[i][3] = 0;
  }
}

}  // namespace

BounceLight::~BounceLight() {
  for (gfx::Pipeline p : {trace_, light_, sight_, lightBorder_, sightBorder_, keep_}) device_->Destroy(p);
  for (gfx::Texture t : {rays_, lightAtlas_, sightAtlas_, columns_, identities_}) device_->Destroy(t);
}

bool BounceLight::Init(std::string* error) {
  if (!device_->RayTracing()) {
    if (error) *error = "no ray tracing on this device or backend";
    return false;
  }
  const std::string common = std::string(kViewShaderBlock) + kRayShaderBlock + kCommon;
  const std::string trace = common + kTrace, light = common + kBlend, sight = common + "#define SIGHT 1\n" + kBlend,
                    lightBorder = common + kBorder, sightBorder = common + "#define SIGHT 1\n" + kBorder, keep = common + kKeep;
  auto make = [&](const char* name, const std::string& source, const char* entry) {
    gfx::ComputeDesc cd;
    cd.name = name;
    cd.source = source;
    cd.entry = entry;
    return device_->CreateCompute(cd, error);
  };
  trace_ = make("bounce light: the probes' rays", trace, "Trace");
  light_ = trace_ ? make("bounce light: the probes' light", light, "Blend") : gfx::Pipeline{};
  sight_ = light_ ? make("bounce light: the probes' sight", sight, "Blend") : gfx::Pipeline{};
  lightBorder_ = sight_ ? make("bounce light: the light's borders", lightBorder, "Border") : gfx::Pipeline{};
  sightBorder_ = lightBorder_ ? make("bounce light: the sight's borders", sightBorder, "Border") : gfx::Pipeline{};
  keep_ = sightBorder_ ? make("bounce light: the probes' numbers", keep, "Keep") : gfx::Pipeline{};
  if (!keep_) return false;
  gfx::TargetDesc td;
  td.storage = true;
  td.width = kRays;
  td.height = kColumns * kColumns * kLayers;
  td.format = gfx::Format::RGBA16F;
  rays_ = device_->CreateTarget(td);
  td.width = kColumns * 8;
  td.height = kColumns * kLayers * 8;
  lightAtlas_ = device_->CreateTarget(td);
  td.width = kColumns * 16;
  td.height = kColumns * kLayers * 16;
  td.format = gfx::Format::RG16F;
  sightAtlas_ = device_->CreateTarget(td);
  td.width = kColumns;
  td.height = kColumns * kLayers;
  td.format = gfx::Format::R32UI;
  identities_ = device_->CreateTarget(td);
  gfx::TextureDesc cd;
  cd.width = cd.height = kColumns;
  cd.format = gfx::Format::R32F;
  cd.data = heights_;
  cd.dataSize = sizeof heights_;
  columns_ = device_->CreateTexture(cd);
  return rays_ && lightAtlas_ && sightAtlas_ && identities_ && columns_;
}

void BounceLight::Place(Vec3 target, float distance, const std::function<float(float, float)>& groundHeight, ViewConstants* c) {
  if (followZoom) {  // (a step taken a fifth past its line, either way)
    const float lines[3] = {30, 60, 120};
    while (level_ < 3 && distance > lines[level_] * 1.2f) ++level_;
    while (level_ > 0 && distance < lines[level_ - 1] / 1.2f) --level_;
    spacing = 4.0f * static_cast<float>(1 << level_);
  }
  firstX_ = static_cast<int>(std::floor(target.x / spacing)) - kColumns / 2 + 1;
  firstZ_ = static_cast<int>(std::floor(target.z / spacing)) - kColumns / 2 + 1;
  for (int j = 0; j < kColumns; ++j)
    for (int i = 0; i < kColumns; ++i) {
      const int cx = firstX_ + i, cz = firstZ_ + j;
      const int tx = ((cx % kColumns) + kColumns) % kColumns, tz = ((cz % kColumns) + kColumns) % kColumns;
      heights_[tz * kColumns + tx] = groundHeight(static_cast<float>(cx) * spacing, static_cast<float>(cz) * spacing);
    }
  c->bounce[0] = 1;
  c->bounce[1] = spacing;
  c->bounce[2] = static_cast<float>(firstX_);
  c->bounce[3] = static_cast<float>(firstZ_);
  std::copy(layers, layers + kLayers, c->bounceLayers);
  placed_ = true;
}

void BounceLight::Update(const View& view, RayScene& rays, gfx::Texture groundNormals, gfx::Texture groundColours, gfx::Texture mapSky) {
  if (!placed_ || !keep_ || !rays.Scene()) return;
  placed_ = false;
  device_->UpdateTexture(columns_, 0, 0, kColumns, kColumns, heights_, kColumns * sizeof(float));
  struct {
    float turn[3][4];
    float map[4], blend[4];
  } k = {};
  Turn(frame_++, k.turn);
  k.map[0] = rays.MapWidth();
  k.map[1] = rays.MapHeight();
  k.map[2] = static_cast<float>(frame_ & 0xffffff);
  k.map[3] = reach;
  k.blend[0] = keep;
  k.blend[1] = static_cast<float>(level_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k, sizeof k);
  // the rays, reading the probes as they were
  device_->SetPipeline(trace_);
  rays.Bind(groundNormals, groundColours);
  device_->SetTexture(kEnvironmentSlot, mapSky);
  Bind();
  device_->SetStorage(0, rays_);
  device_->Dispatch(1, kColumns * kColumns * kLayers, 1);
  device_->SetStorage(0, {});
  rays.Unbind();
  for (int slot : {kBounceLightSlot, kBounceSightSlot}) device_->SetTexture(slot, {});
  // their light and sight blended into the probes', then the tiles' borders, then the probes' numbers kept
  device_->SetTexture(3, rays_);
  device_->SetTexture(4, identities_);
  device_->SetPipeline(light_);
  device_->SetStorage(0, lightAtlas_);
  device_->Dispatch((kColumns * 6 + 7) / 8, (kColumns * kLayers * 6 + 7) / 8, 1);
  device_->SetPipeline(sight_);
  device_->SetStorage(0, sightAtlas_);
  device_->Dispatch((kColumns * 14 + 7) / 8, (kColumns * kLayers * 14 + 7) / 8, 1);
  device_->SetTexture(3, {});
  device_->SetTexture(4, {});
  device_->SetPipeline(lightBorder_);
  device_->SetStorage(0, lightAtlas_);
  device_->Dispatch(kColumns * 8 / 8, kColumns * kLayers * 8 / 8, 1);
  device_->SetPipeline(sightBorder_);
  device_->SetStorage(0, sightAtlas_);
  device_->Dispatch(kColumns * 16 / 8, kColumns * kLayers * 16 / 8, 1);
  device_->SetPipeline(keep_);
  device_->SetStorage(0, identities_);
  device_->Dispatch(kColumns / 8, kColumns * kLayers / 8, 1);
  device_->SetStorage(0, {});
}

void BounceLight::Bind() {
  device_->SetTexture(kBounceLightSlot, lightAtlas_);
  device_->SetTexture(kBounceSightSlot, sightAtlas_);
  device_->SetTexture(kBounceColumnsSlot, columns_);
}

}  // namespace render
