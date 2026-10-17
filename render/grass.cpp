#include "grass.h"

#include <algorithm>
#include <cmath>

#include "lighting.h"

namespace render {
namespace {

// Fifteen corners a blade (three segments, the last a point): no vertex data; a blade's number gives its cell.
const char kShader[] = R"hlsl(
cbuffer Grass : register(b1) {
  float4 area;     // xy: the first cell's world x, z; z: a cell's size; w: cells along a side
  float4 blades;   // x: the tallest blade; y: its width at the foot; z: how far from the eye it thins out to nothing;
                   // w: world units of the heights' largest value
  float4 mapInfo;  // xy: the map's width and height; z: world units a pixel covers at distance 1; w: the water's level
};
Texture2D<float> heightMap : register(t0);
Texture2D cover : register(t1);          // Terrain::GroundColours: rgb the ground's colour, a how grassy
Texture2D groundNormals : register(t2);  // x in alpha, z in green

uint GrassHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float GrassRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }

float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * blades.w;
}

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float3 colour : TEXCOORD1;  // linear
  float3 normal : TEXCOORD2;
  float up : TEXCOORD3;       // 0 at the foot, 1 at the tip
  float shadow : TEXCOORD4;
};

// (level along the blade: 0 the foot .. 3 the tip; side 0 / 1)
static const uint2 kCorner[15] = {uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1), uint2(1, 0), uint2(1, 1),
                                  uint2(2, 0), uint2(2, 0), uint2(1, 1), uint2(2, 1), uint2(2, 0), uint2(2, 1), uint2(3, 0)};
static const float kUp[4] = {0, 0.4, 0.75, 1};
static const float kWide[4] = {1, 0.75, 0.45, 0};

VOut VS(uint id : SV_VertexID) {
  VOut o;
  o.position = float4(0, 0, -1, 1);  // (no blade: behind the eye)
  o.world = o.colour = o.normal = 0;
  o.up = o.shadow = 0;
  uint blade = id / 15, corner = id % 15;
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(blade % side, blade / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));  // (the cell's whole-number place in the world: the pattern stays)
  uint h = GrassHash(uint(cellId.x) * 73856093u ^ GrassHash(uint(cellId.y) * 19349663u));
  float2 xz = cellAt + float2(GrassRand(h), GrassRand(GrassHash(h + 1))) * area.z;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  float4 c = cover.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float ground = GroundAt(xz);
  if (ground < mapInfo.w + 0.08) return o;  // (none in or at the water)
  float3 root = float3(xz.x, ground, xz.y);
  float far = length(root - cameraPosition);
  float thin = saturate(1.6 - far / blades.z * 1.6);  // (fewer far off, none at the reach)
  if (GrassRand(GrassHash(h + 2)) > c.a * thin) return o;
  float height = blades.x * (0.55 + 0.45 * GrassRand(GrassHash(h + 3))) * saturate(thin * 2.5);
  float turn = GrassRand(GrassHash(h + 4)) * 6.2832;
  float2 face = float2(cos(turn), sin(turn)), across = float2(-face.y, face.x);
  // its lean, and the wind the trees sway in (mesh_renderer.cpp: a slow swing and a quicker one, in gusts that
  // run across the land, harder in rain)
  float lean = 0.12 + 0.22 * GrassRand(GrassHash(h + 5));
  float2 leanWay = float2(cos(turn * 1.7 + 1), sin(turn * 1.7 + 1));
  float beat = dot(xz, float2(0.11, 0.07));
  float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((xz - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
  float2 bend = leanWay * lean + float2(0.915, 0.403) * (swing * gust * 0.45);
  uint2 at = kCorner[corner];
  float v = kUp[at.x];
  // (at least most of a pixel wide, or far blades would flicker in and out between pixels)
  float wide = max(blades.y, mapInfo.z * far * 0.8) * kWide[at.x];
  float3 p = root + float3(bend.x * v * v * height, v * height, bend.y * v * v * height);
  p.xz += across * ((at.y ? 0.5 : -0.5) * wide);
  o.position = mul(viewProj, float4(p, 1));
  o.world = p;
  o.up = v;
  // its colour: the ground's under it, varied; a share of the blades drier toward the tip (late summer)
  float3 base = ToLinear(c.rgb) * (0.8 + 0.4 * GrassRand(GrassHash(h + 6)));
  float dry = GrassRand(GrassHash(h + 7)) < 0.22 ? 1 : 0;
  o.colour = lerp(base, ToLinear(float3(0.62, 0.55, 0.32)) * 0.75, dry * v * 0.85);
  // lit as the ground under it, a little toward the blade's own face
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  o.normal = normalize(n + float3(face.x, 0, face.y) * 0.3);
  o.shadow = SunShadow(root + float3(0, height * 0.5, 0), n);
  return o;
}

float4 PS(VOut i) : SV_Target {
  float occlusion = lerp(0.4, 1, i.up);  // (darker down among the blades)
  return float4(ModernLight(i.colour, 0, 0.85, occlusion, 0, normalize(i.normal), i.world, i.shadow), 1);
}
)hlsl";

struct GrassConstants {
  float area[4], blades[4], mapInfo[4];
};

}  // namespace

GroundCover::~GroundCover() {
  if (pipeline_) device_->Destroy(pipeline_);
}

bool GroundCover::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
  gfx::PipelineDesc pd;
  pd.name = "ground cover: grass";
  pd.vertexSource = pd.pixelSource = source;
  pd.cull = gfx::Cull::None;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pipeline_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(pipeline_);
}

void GroundCover::Draw(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover,
                       gfx::Texture normals, float mapWidth, float mapHeight, float waterLevel) {
  drawnBlades = 0;
  if (!pipeline_ || !heights || !cover || !normals || camera.distance > farthest || view.height <= 0) return;
  // a square round what the camera looks at, as wide as it sees near there (no more than about 370 cells a side)
  const float reach = std::clamp(camera.distance * 0.75f, 8.0f, 28.0f);
  const int side = static_cast<int>(std::ceil(2 * reach / cell));
  const float x0 = std::floor((camera.target.x - reach) / cell) * cell, z0 = std::floor((camera.target.z - reach) / cell) * cell;
  GrassConstants k{};
  k.area[0] = x0;
  k.area[1] = z0;
  k.area[2] = cell;
  k.area[3] = static_cast<float>(side);
  k.blades[0] = bladeHeight;
  k.blades[1] = bladeWidth;
  k.blades[2] = reach * 1.4f;
  k.blades[3] = heightUnit;
  k.mapInfo[0] = mapWidth;
  k.mapInfo[1] = mapHeight;
  k.mapInfo[2] = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  k.mapInfo[3] = waterLevel;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, heights);
  device_->SetTexture(1, cover);
  device_->SetTexture(2, normals);
  drawnBlades = side * side;
  device_->Draw(static_cast<uint32_t>(drawnBlades) * 15);
  for (int slot = 0; slot < 3; ++slot) device_->SetTexture(slot, {});
}

}  // namespace render
