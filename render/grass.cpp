#include "grass.h"

#include <algorithm>
#include <cmath>
#include <memory>

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
  float4 table;    // x: 1 where the map's cover table says how many blades grow (densityA's red), else by how grassy
};
Texture2D<float> heightMap : register(t0);
Texture2D cover : register(t1);          // Terrain::GroundColours: rgb the ground's colour, a how grassy
Texture2D groundNormals : register(t2);  // x in alpha, z in green
Texture2D<float> trampled : register(t3);  // over the square: 1 where what stands there presses the grass flat
Texture2D densityA : register(t4);       // Terrain::CoverDensity's first: blades, clumps, flowers, dry stalks

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
  float through : TEXCOORD5;  // the sun's light through the blade (World::leafLight; lighting.h, LeafThrough)
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
  o.up = o.shadow = o.through = 0;
  uint blade = id / 15, corner = id % 15;
  uint tuft = blade / 3, k = blade % 3;  // (three blades a cell, from close roots)
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(tuft % side, tuft / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));  // (the cell's whole-number place in the world: the pattern stays)
  uint h = GrassHash(uint(cellId.x) * 73856093u ^ GrassHash(uint(cellId.y) * 19349663u));
  float2 xz = cellAt + float2(GrassRand(h), GrassRand(GrassHash(h + 1))) * area.z;
  uint hb = GrassHash(h + 101 * (k + 1));  // (this blade of the tuft: its own root near the tuft's, its own shape)
  xz += (float2(GrassRand(hb), GrassRand(GrassHash(hb + 1))) - 0.5) * 0.07;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  float4 c = cover.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float ground = GroundAt(xz);
  if (ground < mapInfo.w + 0.08) return o;  // (none in or at the water)
  float3 root = float3(xz.x, ground, xz.y);
  float far = length(root - cameraPosition);
  // (fewer toward the square's edge, none at it: by the distance from what the camera looks at, its middle)
  float half = area.z * area.w * 0.5;
  float thin = saturate((1 - length(xz - (area.xy + half)) / half) * 3);
  const float grassy = table.x > 0.5 ? densityA.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0).r : c.a;
  if (GrassRand(GrassHash(h + 2)) > grassy * thin) return o;
  float height = blades.x * (0.55 + 0.45 * GrassRand(GrassHash(hb + 3))) * saturate(thin * 2.5);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, (xz - area.xy) / (area.z * area.w), 0);  // (pressed flat)
  float turn = GrassRand(GrassHash(hb + 4)) * 6.2832;
  float2 face = float2(cos(turn), sin(turn)), across = float2(-face.y, face.x);
  // its lean, and the wind the trees sway in (mesh_renderer.cpp: a slow swing and a quicker one, in gusts that
  // run across the land, harder in rain)
  float lean = 0.12 + 0.22 * GrassRand(GrassHash(hb + 5));
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
  float3 base = ToLinear(c.rgb) * (0.8 + 0.4 * GrassRand(GrassHash(hb + 6)));
  float dry = GrassRand(GrassHash(hb + 7)) < 0.22 ? 1 : 0;
  o.colour = lerp(base * lerp(0.9, 1.15, v), ToLinear(float3(0.62, 0.55, 0.32)) * 0.75, dry * v * 0.85);
  // lit as the ground under it, a little toward the blade's own face
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  o.normal = normalize(n + float3(face.x, 0, face.y) * 0.3);
  o.shadow = SunShadow(root + float3(0, height * 0.5, 0), n);
  // the sun's light through it (World::leafLight): the side the eye sees faces it; thinner, more toward the tip
  float3 toEye = normalize(cameraPosition - p), faceWay = float3(face.x, 0, face.y);
  o.through = leaves.x > 0.5 ? LeafThrough(dot(faceWay, toEye) < 0 ? -faceWay : faceWay, toEye) * (0.4 + 0.6 * v) : 0;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float occlusion = lerp(0.65, 1, i.up);  // (darker down among the blades)
  float3 through = LeafLight(i.colour, i.through, i.world, i.shadow, 0.8);
  return float4(ModernLight(i.colour, 0, 0.85, occlusion, through, normalize(i.normal), i.world, i.shadow), 1);
}
)hlsl";

struct GrassConstants {
  float area[4], blades[4], mapInfo[4], table[4];
};

// The plants on cards (GroundCover::SetPlants): six corners a card, up to six cards a spot (a clump's), a spot a cell.
const char kPlantShader[] = R"hlsl(
cbuffer Plants : register(b1) {
  float4 area;      // xy: the first cell's world x, z; z: a cell's size; w: cells along a side
  float4 mapInfo;   // xy: the map's width and height; z: world units of the heights' largest value; w: the water's level
  float4 info;      // x: spots a cell where the table says 1; y: (unused); z: flowers' patches' size; w: their cover
  float4 lean;      // x: how far a card leans back toward the camera
  float4 cells[48]; // per cell three: uv rectangle; frame, low, high height, height (metres); sway, clump's least, most,
                    // its world units a metre
  float4 order[4];  // the cells by group (16 indices)
  float4 groups[6]; // per group: where its cells start in `order`, how many
};
Texture2D<float> heightMap : register(t0);
Texture2D cover : register(t1);          // Terrain::GroundColours (unused here but for its place)
Texture2D groundNormals : register(t2);  // x in alpha, z in green
Texture2D<float> trampled : register(t3);
Texture2D densityA : register(t4);       // blades, clumps, flowers, dry stalks
Texture2D densityB : register(t5);       // ferns, dry cells' share, weeds' share
Texture2D atlas : register(t6);          // the plants' albedo (sRGB values; alpha their coverage, tested at a half)
Texture2D atlasNormals : register(t7);   // card space: x in alpha, y (up) in green (BC5 read as the game's DXT5)

uint PlantHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float PlantRand(uint h) { return float(h & 0xffffffu) / 16777216.0; }
float PlantGround(float2 xz) {
  float2 p = clamp(xz, 0, mapInfo.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  int2 most = int2(mapInfo.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * mapInfo.z;
}
// flowers in loose patches: a smooth noise of the place, so many patches across (info.z apart), info.w of the ground
float PlantPatch(float2 xz) {
  float2 g = xz / max(info.z, 0.1);
  float2 i = floor(g), f = g - i;
  f = f * f * (3 - 2 * f);
  float a = PlantRand(PlantHash(uint(int(i.x)) * 73856093u ^ PlantHash(uint(int(i.y)) * 19349663u)));
  float b = PlantRand(PlantHash(uint(int(i.x) + 1) * 73856093u ^ PlantHash(uint(int(i.y)) * 19349663u)));
  float c = PlantRand(PlantHash(uint(int(i.x)) * 73856093u ^ PlantHash(uint(int(i.y) + 1) * 19349663u)));
  float d = PlantRand(PlantHash(uint(int(i.x) + 1) * 73856093u ^ PlantHash(uint(int(i.y) + 1) * 19349663u)));
  float n = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
  float cover = max(info.w, 0.05);
  return saturate((n - (1 - cover)) / 0.08) / cover;  // (the same flowers in all, gathered in the patches)
}
uint OrderAt(uint i) { return uint(order[i / 4][i % 4]); }

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float3 right : TEXCOORD2;
  float3 up : TEXCOORD3;
  float3 toward : TEXCOORD4;   // the card's face, toward the camera
  float3 ground : TEXCOORD5;   // the ground's normal under it
  float2 shade : TEXCOORD6;    // x: its sun shadow; y: how high up the plant (0 the foot, 1 its top)
};

static const float2 kCard[6] = {float2(0, 1), float2(1, 1), float2(0, 0), float2(0, 0), float2(1, 1), float2(1, 0)};

VOut VS(uint id : SV_VertexID) {
  VOut o = (VOut)0;
  o.position = float4(0, 0, -1, 1);  // (no plant: behind the eye)
  uint spot = id / 36, card = (id / 6) % 6, corner = id % 6;
  uint side = uint(area.w);
  float2 cellAt = area.xy + float2(spot % side, spot / side) * area.z;
  int2 cellId = int2(floor(cellAt / area.z + 0.5));
  uint h = PlantHash(uint(cellId.x) * 83492791u ^ PlantHash(uint(cellId.y) * 2971215073u));
  float2 xz = cellAt + float2(PlantRand(h), PlantRand(PlantHash(h + 1))) * area.z;
  if (any(xz < 0) || any(xz > mapInfo.xy)) return o;
  float4 a = densityA.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  float4 b = densityB.SampleLevel(samplerLinearClamp, xz / mapInfo.xy, 0);
  // which group grows here, if any: clumps, flowers (in patches), dry stalks, ferns, by the table
  float k = info.x;
  float pc = a.g * k, pf = a.b * k * PlantPatch(xz), ps = a.a * k, pn = b.r * k;
  float r = PlantRand(PlantHash(h + 2));
  uint group;
  if (r < pc) {
    float s = PlantRand(PlantHash(h + 3));
    group = s < b.b ? 2 : (PlantRand(PlantHash(h + 4)) < b.g ? 1 : 0);  // (weeds, else dry grass by the dry share, else grass)
  } else if (r < pc + pf) group = 3;
  else if (r < pc + pf + ps) group = 4;
  else if (r < pc + pf + ps + pn) group = 5;
  else return o;
  float4 g = groups[group];
  if (g.y < 0.5) return o;
  uint cell = OrderAt(uint(g.x) + PlantHash(h + 5) % uint(g.y));
  float4 uvRect = cells[cell * 3], size = cells[cell * 3 + 1], more = cells[cell * 3 + 2];
  uint count = uint(more.y) + PlantHash(h + 6) % uint(max(more.z - more.y + 1, 1));
  if (card >= count) return o;
  // this card of the spot: its own foot near the spot's, its own height and side
  uint hc = PlantHash(h * 31 + card * 977 + 7);
  float2 foot = xz;
  if (card > 0) {
    float angle = PlantRand(hc) * 6.2832, reach = size.x * more.w * 0.35 * sqrt(PlantRand(PlantHash(hc + 1)));
    foot += float2(cos(angle), sin(angle)) * reach;
  }
  float ground = PlantGround(foot);
  if (ground < mapInfo.w + 0.08) return o;  // (none in or at the water)
  float3 root = float3(foot.x, ground, foot.y);
  // fewer toward the square's edge, none at it; pressed flat where something stands
  float half = area.z * area.w * 0.5;
  float thin = saturate((1 - length(foot - (area.xy + half)) / half) * 3);
  float height = lerp(size.y, size.z, PlantRand(PlantHash(hc + 2))) * more.w * saturate(thin * 2.5);
  height *= 1 - 0.9 * trampled.SampleLevel(samplerLinearClamp, (foot - area.xy) / (area.z * area.w), 0);
  if (height <= 0.001) return o;
  float frame = size.x * height / max(size.w, 0.001);  // (the card: the cell's frame, as tall as the plant is)
  // facing the camera, leaning back toward it
  float2 to = cameraPosition.xz - foot;
  to = length(to) > 1e-4 ? normalize(to) : float2(0, 1);
  float3 toward = float3(to.x, 0, to.y);
  float3 right = float3(to.y, 0, -to.x);
  float3 up = normalize(float3(0, 1, 0) - toward * lean.x);  // (its top back from the camera: its face up toward it)
  // the trees' wind (as the blades'), the more toward the top by the cell's sway
  float beat = dot(foot, float2(0.11, 0.07));
  float swing = sin(time * 1.3 + beat) * 0.6 + sin(time * 2.9 + beat * 1.7) * 0.4;
  float gust = (0.35 + 0.9 * falling.x + 0.3 * falling.y) * (0.45 + 1.3 * CloudNoise(float3((foot - float2(0.915, 0.403) * (time * 7)) * 0.02, time * 0.07)));
  float2 bend = float2(0.915, 0.403) * (swing * gust * 0.35 * more.x);
  float2 c = kCard[corner];
  bool flip = (hc & 64u) != 0;
  float above = 1 - c.y;  // (0 the foot, 1 the card's top)
  float3 p = root + right * ((c.x - 0.5) * frame) + up * (above * frame);
  p.xz += bend * above * above * frame;
  o.position = mul(viewProj, float4(p, 1));
  o.world = p;
  o.uv = lerp(uvRect.xy, uvRect.zw, float2(flip ? 1 - c.x : c.x, c.y));
  o.right = flip ? -right : right;
  o.up = up;
  o.toward = toward;
  float4 stored = groundNormals.SampleLevel(samplerLinearClamp, foot / mapInfo.xy, 0);
  float3 n;
  n.xz = stored.ag * 2 - 1;
  n.y = sqrt(saturate(1 - dot(n.xz, n.xz)));
  o.ground = n;
  o.shade = float2(SunShadow(root + float3(0, height * 0.5, 0), n), above * frame / max(height, 0.001));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 c = atlas.Sample(samplerAniso, i.uv);
  clip(c.a - 0.5);
  float2 t = atlasNormals.Sample(samplerAniso, i.uv).ag * 2 - 1;
  float3 card = normalize(i.right * t.x + i.up * t.y + i.toward * sqrt(saturate(1 - dot(t, t))));
  float3 n = normalize(lerp(i.ground, card, 0.5));  // (the card's own normals at half strength: the visuals track's advice)
  float occlusion = lerp(0.7, 1, saturate(i.shade.y));
  // (the sun's light through its leaves, World::leafLight: by the card's own way, the side the eye sees)
  float3 albedo = ToLinear(c.rgb);
  float3 through = LeafLight(albedo, LeafThrough(card, normalize(cameraPosition - i.world)), i.world, i.shade.x, 0.7);
  return float4(ModernLight(albedo, 0, 0.85, occlusion, through, n, i.world, i.shade.x), 1);
}
)hlsl";

struct PlantConstants {
  float area[4], mapInfo[4], info[4], lean[4];
  float cells[48][4];
  float order[4][4];
  float groups[6][4];
};

// Over the grass's square, how flat what stands there presses it: each texel the most of the discs round it (full
// within a disc, nothing a quarter of a world unit outside it). A thread for each texel.
const char kTrample[] = R"hlsl(
cbuffer Trample : register(b1) {
  float4 area;        // as the grass's: the first cell's x, z; a cell's size; cells a side
  float4 count;       // x: how many discs
  float4 discs[256];  // x, z, radius
};
RWTexture2D<float> trampled : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  trampled.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 p = area.xy + (id.xy + 0.5) / float2(w, h) * (area.z * area.w);
  float flat = 0;
  for (uint i = 0; i < uint(count.x); ++i) flat = max(flat, saturate((discs[i].z + 0.25 - length(p - discs[i].xy)) / 0.25));
  trampled[id.xy] = flat;
}
)hlsl";

struct TrampleConstants {
  float area[4], count[4];
  float discs[256][4];
};
constexpr int kTrampleSize = 256;

}  // namespace

GroundCover::~GroundCover() {
  for (gfx::Pipeline p : {pipeline_, trample_, plantPipeline_})
    if (p) device_->Destroy(p);
  if (trampled_) device_->Destroy(trampled_);
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
  if (!pipeline_) return false;
  const std::string plants = std::string(kViewShaderBlock) + kLightingShaderBlock + kPlantShader;
  pd.name = "ground cover: plants";
  pd.vertexSource = pd.pixelSource = plants;
  plantPipeline_ = device_->CreatePipeline(pd, error);
  if (!plantPipeline_) return false;
  const std::string trample = std::string(kViewShaderBlock) + kTrample;
  gfx::ComputeDesc cd;
  cd.name = "ground cover: pressed flat";
  cd.source = trample;
  trample_ = device_->CreateCompute(cd, error);
  if (!trample_) return false;
  gfx::TargetDesc td;
  td.storage = true;
  td.width = td.height = kTrampleSize;
  td.format = gfx::Format::R8;
  trampled_ = device_->CreateTarget(td);
  return static_cast<bool>(trampled_);
}

void GroundCover::Prepare(const View& view, const Camera& camera, const std::vector<float>& discs) {
  prepared_ = false;
  // (a cinematic camera, cinecam.h: by how high the eye is over the ground ahead, and the square ahead of the eye,
  // from a little behind it to some fifty units on, where a low eye sees the grass)
  const Vec3 eye = camera.Eye();
  if (!pipeline_ || !trample_ || !trampled_ || (camera.cinematic ? eye.y - camera.target.y : camera.distance) > farthest) return;
  // a square round what the camera looks at, as wide as it sees near there (no more than about 370 cells a side)
  const float reach = camera.cinematic ? 28.0f : std::clamp(camera.distance * 0.75f, 8.0f, 28.0f);
  const int side = static_cast<int>(std::ceil(2 * reach / cell));
  const float middleX = camera.cinematic ? eye.x - std::sin(camera.heading) * reach * 0.7f : camera.target.x;
  const float middleZ = camera.cinematic ? eye.z - std::cos(camera.heading) * reach * 0.7f : camera.target.z;
  area_[0] = std::floor((middleX - reach) / cell) * cell;
  area_[1] = std::floor((middleZ - reach) / cell) * cell;
  area_[2] = cell;
  area_[3] = static_cast<float>(side);
  auto k = std::make_unique<TrampleConstants>();
  std::copy(area_, area_ + 4, k->area);
  const float wide = cell * static_cast<float>(side);
  int n = 0;
  for (size_t i = 0; i + 2 < discs.size() && n < 256; i += 3) {
    const float x = discs[i], z = discs[i + 1], r = discs[i + 2];
    if (x + r < area_[0] || z + r < area_[1] || x - r > area_[0] + wide || z - r > area_[1] + wide) continue;
    k->discs[n][0] = x;
    k->discs[n][1] = z;
    k->discs[n][2] = r;
    ++n;
  }
  k->count[0] = static_cast<float>(n);
  device_->SetPipeline(trample_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, k.get(), static_cast<uint32_t>(sizeof(float) * 8 + sizeof(float) * 4 * std::max(n, 1)));
  device_->SetStorage(0, trampled_);
  device_->Dispatch(kTrampleSize / 8, kTrampleSize / 8, 1);
  device_->SetStorage(0, {});
  prepared_ = true;
}

bool GroundCover::SetPlants(const JsonValue& cover, TextureCache* textures, const std::string& folder) {
  plants_ = PlantData{};
  atlas_ = atlasNormals_ = {};
  const JsonValue& cells = cover["cells"];
  if (cells.kind != JsonValue::Object) return false;
  // the groups' cells (a cell's group, and for grass whether it is one of the dry cells)
  std::vector<std::string> dry;
  for (const JsonValue& d : cover["density"]["dry_cells"].items) dry.push_back(d.StringOr(std::string()));
  std::vector<int> byGroup[6];
  float tallest = 0;
  for (const auto& [name, c] : cells.members) {
    const int index = static_cast<int>(c["index"].NumberOr(-1));
    if (index < 0 || index >= 16 || c["uv"].items.size() != 4) continue;
    const std::string group = c["group"].StringOr(std::string());
    const int g = group == "clump" ? (std::find(dry.begin(), dry.end(), name) != dry.end() ? 1 : 0)
                : group == "weed" ? 2 : group == "flower" ? 3 : group == "stalks" ? 4 : group == "fern" ? 5 : -1;
    if (g < 0) continue;
    float(&cell)[3][4] = plants_.cells[index];
    for (int k = 0; k < 4; ++k) cell[0][k] = static_cast<float>(c["uv"].items[static_cast<size_t>(k)].NumberOr(0));
    const JsonValue& range = c["height_range_m"];
    const float height = static_cast<float>(c["height_m"].NumberOr(0));
    const float low = range.items.size() == 2 ? static_cast<float>(range.items[0].NumberOr(height)) : height;
    const float high = range.items.size() == 2 ? static_cast<float>(range.items[1].NumberOr(height)) : height;
    cell[1][0] = static_cast<float>(c["frame_m"].NumberOr(height));
    cell[1][1] = low;
    cell[1][2] = high;
    cell[1][3] = height;
    const JsonValue& clump = c["clump"];
    cell[2][0] = static_cast<float>(c["sway"].NumberOr(0.5));
    cell[2][1] = clump.items.size() == 2 ? static_cast<float>(clump.items[0].NumberOr(1)) : 1.0f;
    cell[2][2] = clump.items.size() == 2 ? static_cast<float>(clump.items[1].NumberOr(1)) : 1.0f;
    if (height <= 0 || cell[1][0] <= 0) continue;
    tallest = std::max(tallest, high);
    byGroup[g].push_back(index);
  }
  int at = 0;
  for (int g = 0; g < 6; ++g) {
    plants_.groups[g][0] = static_cast<float>(at);
    plants_.groups[g][1] = static_cast<float>(byGroup[g].size());
    for (int index : byGroup[g]) {
      plants_.order[at / 4][at % 4] = static_cast<float>(index);
      ++at;
    }
  }
  plants_.count = at;
  if (!at || tallest <= 0) return false;
  plants_.tallest = tallest;  // (the tallest's real height: each cell's scale is worked out as it is drawn, Draw)
  const JsonValue& patches = cover["density"]["flowers_patches"];
  plants_.patchScale = static_cast<float>(patches["patch_scale_units"].NumberOr(12));
  plants_.patchCover = static_cast<float>(patches["patch_cover"].NumberOr(0.35));
  atlas_ = textures->Get(folder + cover["albedo"].StringOr(std::string()));
  atlasNormals_ = textures->Get(folder + cover["normal"].StringOr(std::string()), textures->FlatNormal());
  return static_cast<bool>(atlas_);
}

void GroundCover::Draw(const View& view, const Camera& camera, gfx::Texture heights, float heightUnit, gfx::Texture cover,
                       gfx::Texture normals, float mapWidth, float mapHeight, float waterLevel, gfx::Texture densityA, gfx::Texture densityB) {
  drawnBlades = drawnPlants = 0;
  if (!prepared_ || !heights || !cover || !normals || view.height <= 0) return;
  const int side = static_cast<int>(area_[3]);
  const float reach = 0.5f * area_[2] * area_[3];
  GrassConstants k{};
  std::copy(area_, area_ + 4, k.area);
  k.blades[0] = bladeHeight;
  k.blades[1] = bladeWidth;
  k.blades[2] = reach * 1.4f;
  k.blades[3] = heightUnit;
  k.mapInfo[0] = mapWidth;
  k.mapInfo[1] = mapHeight;
  k.mapInfo[2] = 2 * std::tan(camera.fovY * 0.5f) / static_cast<float>(view.height);
  k.mapInfo[3] = waterLevel;
  const bool table = densityA && densityB;
  k.table[0] = table ? 1.0f : 0.0f;
  device_->SetPipeline(pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k, sizeof k);
  device_->SetTexture(0, heights);
  device_->SetTexture(1, cover);
  device_->SetTexture(2, normals);
  device_->SetTexture(3, trampled_);
  device_->SetTexture(4, table ? densityA : cover);
  drawnBlades = side * side * 3;
  device_->Draw(static_cast<uint32_t>(drawnBlades) * 15);
  // the plants, where the map has them: their spots over the same square, a little coarser
  if (table && plantDensity > 0 && plantPipeline_ && atlas_ && plants_.count) {
    auto p = std::make_unique<PlantConstants>();
    const int plantSide = static_cast<int>(std::ceil(2 * reach / plantCell));
    p->area[0] = std::floor(area_[0] / plantCell) * plantCell;  // (the blades' square, by the plants' cells)
    p->area[1] = std::floor(area_[1] / plantCell) * plantCell;
    p->area[2] = plantCell;
    p->area[3] = static_cast<float>(plantSide);
    p->mapInfo[0] = mapWidth;
    p->mapInfo[1] = mapHeight;
    p->mapInfo[2] = heightUnit;
    p->mapInfo[3] = waterLevel;
    p->info[0] = plantDensity * plantCell * plantCell;
    p->info[1] = 0;
    p->info[2] = plants_.patchScale;
    p->info[3] = plants_.patchCover;
    p->lean[0] = plantLean;
    std::copy(&plants_.cells[0][0][0], &plants_.cells[0][0][0] + 16 * 3 * 4, &p->cells[0][0]);
    // each cell's scale: the low plants `plantBoost` times the factor that brings the tallest to `plantTallest`, but
    // none taller than that (the readability rule): the stalks and the tall flowers at the cap, the grass and the
    // small flowers bigger than their real proportion, so that they read from the game's camera (the visuals
    // track's advice)
    const float metre = plantTallest / std::max(plants_.tallest, 1e-3f);
    for (int i = 0; i < 16; ++i) p->cells[i * 3 + 2][3] = std::min(metre * plantBoost, plantTallest / std::max(plants_.cells[i][1][2], 1e-3f));
    std::copy(&plants_.order[0][0], &plants_.order[0][0] + 16, &p->order[0][0]);
    std::copy(&plants_.groups[0][0], &plants_.groups[0][0] + 24, &p->groups[0][0]);
    device_->SetPipeline(plantPipeline_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, p.get(), sizeof(PlantConstants));
    device_->SetTexture(5, densityB);
    device_->SetTexture(6, atlas_);
    device_->SetTexture(7, atlasNormals_);
    drawnPlants = plantSide * plantSide;
    device_->Draw(static_cast<uint32_t>(drawnPlants) * 36);
  }
  for (int slot = 0; slot < 8; ++slot) device_->SetTexture(slot, {});
}

}  // namespace render
