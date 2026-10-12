#include "terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "lighting.h"

namespace render {
namespace {

constexpr int kPatchQuads = 16;             // quads along one side of the grid mesh
constexpr int kLeafCells = 2 * kPatchQuads; // a level-0 node: 2 x 2 patches of one-cell quads
constexpr int kMaxLevels = 12;
constexpr int kMaxPatches = 1024;           // per draw call (the size of the shader's array)

// The blending and lighting below follow what the original game's terrain shaders compute (the looks it
// calls TTerrain and TTerrainXP), written anew:
//  - colour: the lower layer, then each stratum blended in by its mask channel (mask * 2 - 1, clamped),
//    then the upper (macro) layer by its own alpha;
//  - normal: the layers' normal maps blended by the raw mask values, then tilted from "straight up"
//    to the ground's own normal, which the map file stores as a texture;
//  - light: sun * max(0, N.L) + ambience (+ specular), scaled by the map's lighting multiplier, with the
//    "shadow fill" colour filling what the light leaves; then the water ramp tints by water depth.
const char kShader[] = R"hlsl(
cbuffer TerrainConstants : register(b1) {
  float4 mapSize;        // width, height, 1 / width, 1 / height
  float4 params;         // x: world height of the largest height value, y: 1 for the XP look
  float4 specularColor;
  float4 albedoTile[10]; // x: repeats per world unit
  float4 normalTile[9];
  float4 morph[12];      // per level: x: distance where sliding starts, y: 1 / (length of the slide)
};
cbuffer Patches : register(b2) {
  float4 patches[1024];  // x, z of the patch's corner, size of a quad, level
};

Texture2D<float> heightMap : register(t0);
Texture2D normalMap : register(t1);
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
Texture2D waterMap : register(t4);
Texture2D waterRamp : register(t5);
Texture2D albedoMaps[10] : register(t6);
Texture2D normalMaps[9] : register(t16);

float HeightAt(int2 c) {
  c = clamp(c, int2(0, 0), int2(mapSize.xy));
  return heightMap.Load(int3(c, 0)) * params.x;
}
float HeightBilinear(float2 p) {
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f);
  float a = HeightAt(c), b = HeightAt(c + int2(1, 0)), d = HeightAt(c + int2(0, 1)), e = HeightAt(c + int2(1, 1));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

struct VOut {
  float4 position : SV_Position;
  float3 world : TEXCOORD0;
};

VOut VS(float2 grid : POSITION, uint instance : SV_InstanceID) {
  float4 patch = patches[instance];
  float2 p0 = min(patch.xy + grid * patch.z, mapSize.xy);
  float h0 = HeightBilinear(p0);
  // slide the odd vertices onto their even neighbours as this level's range runs out
  float4 m = morph[int(patch.w)];
  float k = saturate((distance(float3(p0.x, h0, p0.y), cameraPosition) - m.x) * m.y);
  float2 odd = frac(grid * 0.5) * 2.0;
  float2 p = min(patch.xy + (grid - odd * k) * patch.z, mapSize.xy);
  VOut o;
  o.world = float3(p.x, HeightBilinear(p), p.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

float4 PS(VOut i) : SV_Target {
  float2 xz = i.world.xz;
  float2 uv = xz * mapSize.zw;
  float xp = params.y;
  float4 rawLow = maskLow.Sample(samplerLinearClamp, uv);
  float4 rawHigh = maskHigh.Sample(samplerLinearClamp, uv) * xp;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);

  #define ALBEDO(n) albedoMaps[n].Sample(samplerAniso, xz * albedoTile[n].x)
  #define NORMAL(n) (normalMaps[n].Sample(samplerAniso, xz * normalTile[n].x) * 2 - 1)
  float4 albedo = ALBEDO(0);
  albedo = lerp(albedo, ALBEDO(1), low.x);
  albedo = lerp(albedo, ALBEDO(2), low.y);
  albedo = lerp(albedo, ALBEDO(3), low.z);
  albedo = lerp(albedo, ALBEDO(4), low.w);
  albedo = lerp(albedo, ALBEDO(5), high.x);
  albedo = lerp(albedo, ALBEDO(6), high.y);
  albedo = lerp(albedo, ALBEDO(7), high.z);
  albedo = lerp(albedo, ALBEDO(8), high.w);
  float4 upper = ALBEDO(9);
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);

  float4 n = NORMAL(0);
  n = lerp(n, NORMAL(1), rawLow.x);
  n = lerp(n, NORMAL(2), rawLow.y);
  n = lerp(n, NORMAL(3), rawLow.z);
  n = lerp(n, NORMAL(4), rawLow.w);
  n = lerp(n, NORMAL(5), rawHigh.x);
  n = lerp(n, NORMAL(6), rawHigh.y);
  n = lerp(n, NORMAL(7), rawHigh.z);
  n = lerp(n, NORMAL(8), rawHigh.w);
  n.xyz = normalize(n.xyz);
  // the layers' normal, y up
  float3 detail = float3(n.x, sqrt(saturate(1 - dot(n.xy, n.xy))), n.y);
  // the ground's normal: x in the texture's alpha, z in its green
  float4 stored = normalMap.Sample(samplerLinearClamp, uv);
  float3 base;
  base.xz = stored.ag * 2 - 1;
  base.y = sqrt(saturate(1 - dot(base.xz, base.xz)));
  // the rotation that takes "up" to the ground's normal, applied to the layers' normal
  float3 h = normalize(base + float3(0, 1, 0));
  float3 axisX = float3(1 - 2 * h.x * h.x, 2 * h.x * h.y, -2 * h.x * h.z);
  float3 axisZ = float3(-2 * h.z * h.x, 2 * h.z * h.y, 1 - 2 * h.z * h.z);
  float3 N = float3(dot(detail, axisX), dot(detail, base), dot(detail, axisZ));

  // under water: tinted toward the ramp's colour by depth
  float depth = waterMap.Sample(samplerLinearClamp, uv).g;
  float4 ramp = waterRamp.Sample(samplerLinearClamp, float2(depth, 0.5));
  float shadow = SunShadow(i.world, base);
  // how shiny the ground is: the plain look keeps it in the albedo's alpha inverted, the XP look as it is
  float gloss = lerp(1 - albedo.a, albedo.a, xp);

  if (look > 0.5) {
    // the modern look: the ground is a rough, non-metal surface
    float roughness = 1 - 0.5 * gloss * saturate(specularColor.x * 2);
    float3 lit = ModernLight(ToLinear(albedo.rgb), 0, roughness, 1, 0, N, i.world, shadow);
    // the under-water tint is mixed the way the original mixes it (in screen colours, relative to
    // full sunlight), so that shallows keep the map's water colours
    float full = max(lightMultiplier * modern.x, 0.01);
    float3 screen = lerp(pow(max(lit / full, 0), 1 / 2.2), ramp.rgb, ramp.a);
    return float4(ToLinear(screen) * full, 1);
  }

  float3 sun = sunDirection;
  float3 toPixel = normalize(i.world - cameraPosition);
  float sunDotN = dot(sun, N);
  float3 diffuse = sunColor * saturate(sunDotN) * shadow + sunAmbience;
  // the plain look: one specular value
  float specularPlain = pow(saturate(dot(sun - 2 * sunDotN * N, toPixel)), 80) * specularColor.x * gloss;
  float3 lightPlain = diffuse + specularPlain;
  lightPlain = lightMultiplier * lightPlain + shadowFill * (1 - lightPlain);
  float3 colorPlain = lightPlain * albedo.rgb;
  // the XP look: coloured specular added to the albedo
  float3 specularXp = pow(saturate(dot(reflect(toPixel, N), sun)), 80) * gloss * specularColor.a * specularColor.rgb;
  float3 lightXp = lightMultiplier * diffuse + shadowFill * (1 - diffuse);
  float3 colorXp = lightXp * (albedo.rgb + specularXp);
  float3 color = lerp(colorPlain, colorXp, xp);
  return float4(lerp(color, ramp.rgb, ramp.a), 1);
}
)hlsl";

struct TerrainConstants {
  float mapSize[4];
  float params[4];
  float specularColor[4];
  float albedoTile[10][4];
  float normalTile[9][4];
  float morph[12][4];
};

// Is any point of the box within `radius` of `center`?
bool SphereTouchesBox(Vec3 center, float radius, Vec3 lo, Vec3 hi) {
  float dx = std::max({lo.x - center.x, 0.0f, center.x - hi.x});
  float dy = std::max({lo.y - center.y, 0.0f, center.y - hi.y});
  float dz = std::max({lo.z - center.z, 0.0f, center.z - hi.z});
  return dx * dx + dy * dy + dz * dz <= radius * radius;
}

}  // namespace

bool Terrain::Load(const MapData* map, std::string* error) {
  map_ = map;
  const int w = map->width, h = map->height;

  if (!pipeline_) {
    std::string source = std::string(kViewShaderBlock) + kLightingShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "terrain";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.colorFormat = kSceneFormat;
    pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    // into a shadow map: the same vertices, depth only
    pd.name = "terrain, shadow";
    pd.pixelSource = {};
    pd.depthOnly = true;
    pd.cull = gfx::Cull::None;
    shadowPipeline_ = device_->CreatePipeline(pd, error);
    if (!shadowPipeline_) return false;

    std::vector<float> vertices;
    for (int z = 0; z <= kPatchQuads; ++z)
      for (int x = 0; x <= kPatchQuads; ++x) {
        vertices.push_back(static_cast<float>(x));
        vertices.push_back(static_cast<float>(z));
      }
    std::vector<uint16_t> indices;
    const int row = kPatchQuads + 1;
    for (int z = 0; z < kPatchQuads; ++z)
      for (int x = 0; x < kPatchQuads; ++x) {
        uint16_t a = static_cast<uint16_t>(z * row + x), b = static_cast<uint16_t>(a + row);
        // counter-clockwise seen from above (x east, z south)
        for (uint16_t i : {a, b, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 1), b, static_cast<uint16_t>(b + 1)}) indices.push_back(i);
      }
    gfx::BufferDesc vb;
    vb.stride = 8;
    vb.size = static_cast<uint32_t>(vertices.size() * sizeof(float));
    vb.data = vertices.data();
    gridVertices_ = device_->CreateBuffer(vb);
    gfx::BufferDesc ib;
    ib.kind = gfx::BufferKind::Index;
    ib.stride = 2;
    ib.size = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
    ib.data = indices.data();
    gridIndices_ = device_->CreateBuffer(ib);
    gridIndexCount_ = static_cast<uint32_t>(indices.size());
  }

  gfx::TextureDesc hd;
  hd.width = w + 1;
  hd.height = h + 1;
  hd.format = gfx::Format::R16;
  hd.data = map->heights.data();
  hd.dataSize = map->heights.size() * sizeof(uint16_t);
  heightTexture_ = device_->CreateTexture(hd);
  if (!heightTexture_) {
    if (error) *error = "cannot create the height texture";
    return false;
  }
  auto embedded = [&](const std::string& dds, const char* what, gfx::Texture fallback) {
    gfx::Texture t = dds.empty() ? gfx::Texture{} : textures_->FromDds(dds, what);
    return t ? t : fallback;
  };
  normalMap_ = embedded(map->normalMap, "the map's normal map", textures_->FlatNormal());
  maskLow_ = embedded(map->maskLow, "the map's first layer mask", textures_->Black());
  maskHigh_ = embedded(map->maskHigh, "the map's second layer mask", textures_->Black());
  waterMap_ = embedded(map->waterMap, "the map's water map", textures_->Black());
  waterRamp_ = map->water.present ? textures_->Get(map->water.ramp, textures_->Black()) : textures_->Black();
  for (int i = 0; i < 10; ++i) albedo_[i] = textures_->Get(map->albedo[i].texture, textures_->Black());
  for (int i = 0; i < 9; ++i) normal_[i] = textures_->Get(map->normal[i].texture, textures_->FlatNormal());
  shaderKind_ = map->shader == "TTerrainXP" ? 1 : 0;
  if (map->shader != "TTerrain" && map->shader != "TTerrainXP")
    fprintf(stderr, "render: the map asks for the terrain look \"%s\", which is not written yet; using TTerrain\n", map->shader.c_str());

  // the tree of nodes: per level, each node's lowest and highest ground (for culling and for distances)
  levels_.clear();
  for (int k = 0; k < kMaxLevels; ++k) {
    Level level;
    level.nodeCells = kLeafCells << k;
    level.nx = (w + level.nodeCells - 1) / level.nodeCells;
    level.nz = (h + level.nodeCells - 1) / level.nodeCells;
    level.lo.assign(static_cast<size_t>(level.nx) * static_cast<size_t>(level.nz), std::numeric_limits<float>::max());
    level.hi.assign(level.lo.size(), std::numeric_limits<float>::lowest());
    if (k == 0) {
      for (int nz = 0; nz < level.nz; ++nz)
        for (int nx = 0; nx < level.nx; ++nx) {
          uint16_t lo = 65535, hi = 0;
          const int x1 = std::min(w, (nx + 1) * kLeafCells), z1 = std::min(h, (nz + 1) * kLeafCells);
          for (int z = nz * kLeafCells; z <= z1; ++z) {
            const uint16_t* p = map->heights.data() + static_cast<size_t>(z) * static_cast<size_t>(w + 1);
            for (int x = nx * kLeafCells; x <= x1; ++x) {
              lo = std::min(lo, p[x]);
              hi = std::max(hi, p[x]);
            }
          }
          size_t at = static_cast<size_t>(nz) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx);
          level.lo[at] = static_cast<float>(lo) * map->heightScale;
          level.hi[at] = static_cast<float>(hi) * map->heightScale;
        }
    } else {
      const Level& below = levels_.back();
      for (int nz = 0; nz < below.nz; ++nz)
        for (int nx = 0; nx < below.nx; ++nx) {
          size_t from = static_cast<size_t>(nz) * static_cast<size_t>(below.nx) + static_cast<size_t>(nx);
          size_t to = static_cast<size_t>(nz / 2) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx / 2);
          level.lo[to] = std::min(level.lo[to], below.lo[from]);
          level.hi[to] = std::max(level.hi[to], below.hi[from]);
        }
    }
    levels_.push_back(std::move(level));
    if (levels_.back().nx == 1 && levels_.back().nz == 1) break;
  }
  return true;
}

void Terrain::AddPatch(const View& view, int level, float x, float z, float size, float lo, float hi) {
  const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
  if (x >= w || z >= h) return;
  if (!view.BoxVisible({x, lo, z}, {std::min(x + size, w), hi, std::min(z + size, h)})) return;
  patches_.push_back({x, z, size / kPatchQuads, static_cast<float>(level)});
}

void Terrain::AddNode(const View& view, int level, int ix, int iz) {
  const Level& L = levels_[static_cast<size_t>(level)];
  const float half = static_cast<float>(L.nodeCells) * 0.5f;
  const float x = static_cast<float>(ix * L.nodeCells), z = static_cast<float>(iz * L.nodeCells);
  const size_t at = static_cast<size_t>(iz) * static_cast<size_t>(L.nx) + static_cast<size_t>(ix);
  for (int q = 0; q < 4; ++q) AddPatch(view, level, x + half * static_cast<float>(q & 1), z + half * static_cast<float>(q >> 1), half, L.lo[at], L.hi[at]);
}

// Chooses what to draw for one node. False: the node is beyond this level's range, the caller draws
// its area at its own, coarser level.
bool Terrain::Select(const View& view, int level, int ix, int iz) {
  const Level& L = levels_[static_cast<size_t>(level)];
  const size_t at = static_cast<size_t>(iz) * static_cast<size_t>(L.nx) + static_cast<size_t>(ix);
  const float x = static_cast<float>(ix * L.nodeCells), z = static_cast<float>(iz * L.nodeCells);
  const Vec3 lo{x, L.lo[at], z};
  const Vec3 hi{std::min(x + static_cast<float>(L.nodeCells), static_cast<float>(map_->width)), L.hi[at],
                std::min(z + static_cast<float>(L.nodeCells), static_cast<float>(map_->height))};
  if (!SphereTouchesBox(view.eye, ranges_[static_cast<size_t>(level)], lo, hi)) return false;
  if (!view.BoxVisible(lo, hi)) return true;
  if (level == 0 || !SphereTouchesBox(view.eye, ranges_[static_cast<size_t>(level) - 1], lo, hi)) {
    AddNode(view, level, ix, iz);
    return true;
  }
  const Level& C = levels_[static_cast<size_t>(level) - 1];
  for (int q = 0; q < 4; ++q) {
    int cx = ix * 2 + (q & 1), cz = iz * 2 + (q >> 1);
    if (cx >= C.nx || cz >= C.nz) continue;
    if (!Select(view, level - 1, cx, cz)) {
      size_t cat = static_cast<size_t>(cz) * static_cast<size_t>(C.nx) + static_cast<size_t>(cx);
      AddPatch(view, level, static_cast<float>(cx * C.nodeCells), static_cast<float>(cz * C.nodeCells), static_cast<float>(C.nodeCells),
               C.lo[cat], C.hi[cat]);
    }
  }
  return true;
}

void Terrain::Draw(const View& view, bool shadowPass) {
  if (!shadowPass) drawnPatches = drawnTriangles = 0;
  if (!map_ || levels_.empty()) return;

  // level k is used up to the distance where its quads (2^k cells) shrink to quadPixels on screen
  const float base = static_cast<float>(view.height) / (2 * std::tan(view.fovY * 0.5f) * std::max(1.0f, quadPixels));
  const int top = static_cast<int>(levels_.size()) - 1;
  ranges_.resize(levels_.size());
  for (int k = 0; k <= top; ++k) ranges_[static_cast<size_t>(k)] = k == top ? std::numeric_limits<float>::max() : base * static_cast<float>(1 << k);
  patches_.clear();
  const Level& T = levels_[static_cast<size_t>(top)];
  for (int iz = 0; iz < T.nz; ++iz)
    for (int ix = 0; ix < T.nx; ++ix) Select(view, top, ix, iz);
  if (patches_.empty()) return;

  const MapData& m = *map_;
  TerrainConstants c = {};
  c.mapSize[0] = static_cast<float>(m.width);
  c.mapSize[1] = static_cast<float>(m.height);
  c.mapSize[2] = 1.0f / static_cast<float>(m.width);
  c.mapSize[3] = 1.0f / static_cast<float>(m.height);
  c.params[0] = 65535.0f * m.heightScale;
  c.params[1] = static_cast<float>(shaderKind_);
  c.specularColor[0] = m.lighting.specular.x;
  c.specularColor[1] = m.lighting.specular.y;
  c.specularColor[2] = m.lighting.specular.z;
  c.specularColor[3] = m.lighting.specular.w;
  for (int i = 0; i < 10; ++i) c.albedoTile[i][0] = m.albedo[i].scale > 0 ? 1.0f / m.albedo[i].scale : 0;
  for (int i = 0; i < 9; ++i) c.normalTile[i][0] = m.normal[i].scale > 0 ? 1.0f / m.normal[i].scale : 0;
  for (int k = 0; k <= top && k < 12; ++k) {
    if (k == top) {
      c.morph[k][0] = 1e30f;
      c.morph[k][1] = 0;
    } else {
      const float end = ranges_[static_cast<size_t>(k)] * 0.98f, start = ranges_[static_cast<size_t>(k)] * 0.7f;
      c.morph[k][0] = start;
      c.morph[k][1] = 1.0f / (end - start);
    }
  }

  device_->SetPipeline(shadowPass ? shadowPipeline_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(0, heightTexture_);
  if (!shadowPass) {
    device_->SetTexture(1, normalMap_);
    device_->SetTexture(2, maskLow_);
    device_->SetTexture(3, maskHigh_);
    device_->SetTexture(4, waterMap_);
    device_->SetTexture(5, waterRamp_);
    for (int i = 0; i < 10; ++i) device_->SetTexture(6 + i, albedo_[i]);
    for (int i = 0; i < 9; ++i) device_->SetTexture(16 + i, normal_[i]);
  }
  device_->SetVertexBuffer(gridVertices_);
  device_->SetIndexBuffer(gridIndices_);
  // the shader's array is always handed over whole, so the list is padded to full batches
  const size_t count = patches_.size();
  patches_.resize((count + kMaxPatches - 1) / kMaxPatches * kMaxPatches, Patch{});
  for (size_t at = 0; at < count; at += kMaxPatches) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kMaxPatches, count - at));
    device_->SetConstants(2, patches_.data() + at, kMaxPatches * static_cast<uint32_t>(sizeof(Patch)));
    device_->DrawIndexed(gridIndexCount_, 0, 0, n);
  }
  if (shadowPass) return;
  drawnPatches = static_cast<int>(count);
  drawnTriangles = drawnPatches * kPatchQuads * kPatchQuads * 2;
}

}  // namespace render
