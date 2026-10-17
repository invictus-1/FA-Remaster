#include "terrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "json.h"
#include "lighting.h"
#include "ray_scene.h"
#include "remasters.h"

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
//  - the look it calls TTerrainGlow (a campaign map's glowing ground) is TTerrain with one change:
//    the second stratum's picture drifts in a slow small circle, and its alpha says how much that
//    stratum glows instead of how dull it is.
const char kShader[] = R"hlsl(
cbuffer TerrainConstants : register(b1) {
  float4 mapSize;        // width, height, 1 / width, 1 / height
  float4 params;         // x: world height of the largest height value, y: 1 for the XP look, z: 1 for the glowing look,
                         // w: 1 = the ground goes on beyond the map's edge (the map mirrored)
  float4 specularColor;
  float4 albedoTile[10]; // x: repeats per world unit; y: 1 = the layer has its third map (materialMaps)
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
Texture2DArray materialMaps : register(t25);  // per layer: r creases' shading, g roughness, b height

float HeightAt(int2 c) {
  c = clamp(c, int2(0, 0), int2(mapSize.xy));
  return heightMap.Load(int3(c, 0)) * params.x;
}
float HeightBilinear(float2 p) {
  p = MirrorUv(p * mapSize.zw) * mapSize.xy;  // (beyond the map: the map mirrored; on it: as it is)
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
  float2 p0 = patch.xy + grid * patch.z;
  if (params.w < 0.5) p0 = min(p0, mapSize.xy);
  float h0 = HeightBilinear(p0);
  // slide the odd vertices onto their even neighbours as this level's range runs out
  float4 m = morph[int(patch.w)];
  float k = saturate((distance(float3(p0.x, h0, p0.y), cameraPosition) - m.x) * m.y);
  float2 odd = frac(grid * 0.5) * 2.0;
  float2 p = patch.xy + (grid - odd * k) * patch.z;
  if (params.w < 0.5) p = min(p, mapSize.xy);
  VOut o;
  o.world = float3(p.x, HeightBilinear(p), p.y);
  o.position = mul(viewProj, float4(o.world, 1));
  return o;
}

// A layer's normal map as read: x, y, z from red, green, blue (from 0..1 to -1..1). A two-channel one (BC5,
// whose blue reads 0, which no three-channel normal map has: its z would point down) gets z from x and y.
// `up`: a remastered material's normal (two channels, +y toward the top of its texture): its y turned to the stock
// layers' way
float4 LayerNormal(float4 raw, float up) {
  float4 n = raw * 2 - 1;
  if (raw.b < 0.002) n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));
  if (up > 0.5) n.y = -n.y;
  return n;
}

float4 Ground(VOut i) {
  float2 xz = i.world.xz;
  float2 uv = MirrorUv(xz * mapSize.zw);
  // (in a mirrored copy of the map the stored slopes point the other way)
  float2 mirrored = 1 - 2 * abs(fmod(floor(xz * mapSize.zw), 2));
  float xp = params.y;
  float4 rawLow = maskLow.Sample(samplerLinearClamp, uv);
  float4 rawHigh = maskHigh.Sample(samplerLinearClamp, uv) * xp;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);

  // (read with the place's own steps, so that a layer can be skipped where it is not there at all:
  // inside such a branch the sampler cannot work the steps out itself; the temporal upscaler's bias, view.h,
  // as steps that much smaller)
  float2 stepX = ddx(xz) * exp2(temporal.x), stepY = ddy(xz) * exp2(temporal.x);
  #define ALBEDO(n) albedoMaps[n].SampleGrad(samplerAniso, xz * albedoTile[n].x, stepX * albedoTile[n].x, stepY * albedoTile[n].x)
  #define NORMAL(n) LayerNormal(normalMaps[n].SampleGrad(samplerAniso, xz * normalTile[n].x, stepX * normalTile[n].x, stepY * normalTile[n].x), normalTile[n].y)
  // a layer laid over what is there only where it is there: a share of nothing changes nothing, and a
  // map's layers each cover a part of it only (most pixels need two or three of the ten)
  #define OVER(value, read, share) [branch] if ((share) > 0) value = lerp(value, read, share)
  // A remastered map's layers (modern look): where a layer has its third map, it does not fade
  // evenly into what lies under it: its high places come through first (how much of it shows is
  // its mask pushed by its height), and it brings its own roughness and crease shading.
  // `matter`: x creases' shading, y roughness, z how much of the pixel is such a layer's at all.
  float3 matter = 0;
  [branch] if (look > 0.5 && albedoTile[0].y > 0.5) {
    float3 m = materialMaps.SampleGrad(samplerAniso, float3(xz * albedoTile[0].x, 0), stepX * albedoTile[0].x, stepY * albedoTile[0].x).rgb;
    matter = float3(m.r, m.g, 1);
  }
  #define RICH(n, mask, raw) \
    [branch] if (look > 0.5 && albedoTile[n].y > 0.5 && raw > 0.002) { \
      float3 m = materialMaps.SampleGrad(samplerAniso, float3(xz * albedoTile[n].x, n), stepX * albedoTile[n].x, stepY * albedoTile[n].x).rgb; \
      mask = saturate((m.b + mask * 1.35 - 1) / 0.35); \
      raw = saturate((m.b + raw * 1.35 - 1) / 0.35); \
      matter = lerp(matter, float3(m.r, m.g, 1), mask); \
    } else { \
      matter = lerp(matter, 0, mask); \
    }
  RICH(1, low.x, rawLow.x)
  RICH(2, low.y, rawLow.y)
  RICH(3, low.z, rawLow.z)
  RICH(4, low.w, rawLow.w)
  RICH(5, high.x, rawHigh.x)
  RICH(6, high.y, rawHigh.y)
  RICH(7, high.z, rawHigh.z)
  RICH(8, high.w, rawHigh.w)
  float4 albedo = ALBEDO(0);
  OVER(albedo, ALBEDO(1), low.x);
  float4 second = ALBEDO(2);
  float3 glowing = 0;
  if (params.z > 0.5) {
    // the glowing look: the second stratum's picture goes round a circle of a hundredth of itself,
    // once in fifty seconds; how much it glows is its alpha, read with the circle's two parts
    // swapped (so the glow wanders against the picture), and it counts as fully shiny
    float2 drift;
    sincos(time * 0.125, drift.x, drift.y);
    drift *= 0.01;
    second = albedoMaps[2].Sample(samplerAniso, xz * albedoTile[2].x + drift);
    glowing = second.rgb * (albedoMaps[2].Sample(samplerAniso, xz * albedoTile[2].x + drift.yx).a * low.y);
    second.a = 0;
  }
  albedo = lerp(albedo, second, low.y);
  OVER(albedo, ALBEDO(3), low.z);
  OVER(albedo, ALBEDO(4), low.w);
  OVER(albedo, ALBEDO(5), high.x);
  OVER(albedo, ALBEDO(6), high.y);
  OVER(albedo, ALBEDO(7), high.z);
  OVER(albedo, ALBEDO(8), high.w);
  float4 upper = ALBEDO(9);
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);

  float4 n = NORMAL(0);
  OVER(n, NORMAL(1), rawLow.x);
  OVER(n, NORMAL(2), rawLow.y);
  OVER(n, NORMAL(3), rawLow.z);
  OVER(n, NORMAL(4), rawLow.w);
  OVER(n, NORMAL(5), rawHigh.x);
  OVER(n, NORMAL(6), rawHigh.y);
  OVER(n, NORMAL(7), rawHigh.z);
  OVER(n, NORMAL(8), rawHigh.w);
  n.xyz = normalize(n.xyz);
  // the layers' normal, y up
  float3 detail = float3(n.x, sqrt(saturate(1 - dot(n.xy, n.xy))), n.y);
  // the ground's normal: x in the texture's alpha, z in its green
  float4 stored = normalMap.Sample(samplerLinearClamp, uv);
  float3 base;
  base.xz = (stored.ag * 2 - 1) * mirrored;
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
    // (a remastered map's layers say their own; the upper, map-wide layer has none)
    roughness = lerp(roughness, matter.y, matter.z);
    float creases = lerp(1, matter.x, matter.z);
#ifdef MODERN_WATER
    [branch] if (shore.x > 0.5) {
      // the beach the waves have just wetted: darker and shinier in a band over the water's level, its edge
      // uneven
      float uneven = (CloudNoise(float3(i.world.xz * 0.7, 2.5)) - 0.5) * shore.z * 0.8;
      float over = i.world.y - shore.y;
      float wet = saturate(1 - (over + uneven) / shore.z) * saturate(over * 20 + 1);  // (not under the water: its own colours)
      albedo.rgb *= lerp(1, 0.6, wet);
      roughness = lerp(roughness, 0.22, wet * 0.85);
    }
#endif
    // (ground that glows gives its own light, by day and by night)
    float3 lit = ModernLight(ToLinear(albedo.rgb), 0, roughness, creases, ToLinear(glowing) * 2.5, N, i.world, shadow);
    // the under-water tint is mixed the way the original mixes it (in screen colours, relative to
    // full sunlight), so that shallows keep the map's water colours
#ifdef MODERN_WATER
    return float4(lit, 1);  // (the modern water colours what is under it itself, by how deep it is)
#endif
    float full = max(lightMultiplier * modern.x * (skyZenith.w > 0.5 ? skyGlow.w * lerp(1, 0.45, skyHorizon.w) : 1), 0.001);  // (a computed sky: as bright as the day is)
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
  // (the glowing look: the original marks the pixel to be spread as glow; here it is added)
  float3 color = lerp(colorPlain, colorXp, xp) + glowing * 0.6;
  // its glow, in the picture's alpha, for the original's bloom (render 28): TTerrain 0.01 plus its
  // specular times SpecularColor.w, the XP look 0.01, the glowing look its glow; under the water none
  // (the original's water lays alpha 0 under its plane)
  float glowAlpha = 0.01 + lerp(pow(saturate(dot(sun - 2 * sunDotN * N, toPixel)), 80) * gloss * specularColor.w, 0, xp) +
                    max(glowing.r, max(glowing.g, glowing.b));
  return float4(lerp(color, ramp.rgb, ramp.a), ramp.a > 0.001 ? 0 : saturate(glowAlpha));
}

float4 PS(VOut i) : SV_Target {
  float4 c = Ground(i);
  return float4(BeyondMap(c.rgb, i.world), c.a);
}
)hlsl";

// The ground's colour for the ray tracing scene (Terrain::GroundColours): the shader's blend of the layers, each
// by its texture's average (its smallest level), the upper one read at a level whose texels are about the
// picture's.
const char kGroundColours[] = R"hlsl(
cbuffer TerrainConstants : register(b1) {
  float4 mapSize;
  float4 params;
  float4 specularColor;
  float4 albedoTile[10];
};
Texture2D maskLow : register(t2);
Texture2D maskHigh : register(t3);
Texture2D albedoMaps[10] : register(t6);
RWTexture2D<float4> colours : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  colours.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 xz = (id.xy + 0.5) / float2(w, h) * mapSize.xy;
  float2 uv = xz * mapSize.zw;
  float4 rawLow = maskLow.SampleLevel(samplerLinearClamp, uv, 0);
  float4 rawHigh = maskHigh.SampleLevel(samplerLinearClamp, uv, 0) * params.y;
  float4 low = saturate(rawLow * 2 - 1), high = saturate(rawHigh * 2 - 1);
  #define AVERAGE(n) albedoMaps[n].SampleLevel(samplerLinear, float2(0.5, 0.5), 16)
  // (and how grassy each layer is, by how green its average is against its red and blue: a meadow's grass, not
  // dirt, rock or sand; the ground cover reads it in the alpha)
  #define GRASSY(c) saturate((c.g - max(c.r, c.b)) / max(c.g, 0.05) * 6)
  float4 albedo = AVERAGE(0);
  float grassy = GRASSY(albedo);
  #define LAYER(n, w) { float4 a = AVERAGE(n); albedo = lerp(albedo, a, w); grassy = lerp(grassy, GRASSY(a), w); }
  LAYER(1, low.x);
  LAYER(2, low.y);
  LAYER(3, low.z);
  LAYER(4, low.w);
  LAYER(5, high.x);
  LAYER(6, high.y);
  LAYER(7, high.z);
  LAYER(8, high.w);
  uint tw, th, levels;
  albedoMaps[9].GetDimensions(0, tw, th, levels);
  float across = tw * albedoTile[9].x * mapSize.x / w;  // (the upper layer's texels over one of the picture's)
  float4 upper = albedoMaps[9].SampleLevel(samplerLinear, xz * albedoTile[9].x, max(log2(max(across, 1)), 0));
  albedo.rgb = lerp(albedo.rgb, upper.rgb, upper.a);
  colours[id.xy] = float4(albedo.rgb, grassy);
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

int Terrain::MaterialLayers() const {
  int n = 0;
  for (bool has : material_) n += has ? 1 : 0;
  return n;
}

gfx::Texture Terrain::GroundColours() {
  if (groundColours_ || !map_ || map_->width <= 0 || map_->height <= 0 || !maskLow_) return groundColours_;
  if (!groundColoursPass_) {
    const std::string source = std::string(kViewShaderBlock) + kGroundColours;
    gfx::ComputeDesc cd;
    cd.name = "terrain: the ground's colour for the rays";
    cd.source = source;
    std::string error;
    groundColoursPass_ = device_->CreateCompute(cd, &error);
    if (!groundColoursPass_) {
      fprintf(stderr, "render: %s\n", error.c_str());
      return {};
    }
  }
  gfx::TargetDesc td;
  td.storage = true;
  td.width = std::max(map_->width / 2, 1);
  td.height = std::max(map_->height / 2, 1);
  td.format = gfx::Format::RGBA8;
  groundColours_ = device_->CreateTarget(td);
  if (!groundColours_) return {};
  const MapData& m = *map_;
  TerrainConstants c = {};
  c.mapSize[0] = static_cast<float>(m.width);
  c.mapSize[1] = static_cast<float>(m.height);
  c.mapSize[2] = 1.0f / static_cast<float>(m.width);
  c.mapSize[3] = 1.0f / static_cast<float>(m.height);
  c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
  for (int i = 0; i < 10; ++i) c.albedoTile[i][0] = albedoScale_[i] > 0 ? 1.0f / albedoScale_[i] : 0;
  device_->SetPipeline(groundColoursPass_);
  device_->SetConstants(1, &c, sizeof c);
  device_->SetTexture(2, maskLow_);
  device_->SetTexture(3, maskHigh_);
  for (int i = 0; i < 10; ++i) device_->SetTexture(6 + i, albedo_[i]);
  device_->SetStorage(0, groundColours_);
  device_->Dispatch(static_cast<uint32_t>((td.width + 7) / 8), static_cast<uint32_t>((td.height + 7) / 8), 1);
  device_->SetStorage(0, {});
  for (int i = 0; i < 16; ++i) device_->SetTexture(i, {});
  return groundColours_;
}

// The visuals track's remastered ground (remasters.h, kRemasteredTerrain): where its remap.json lists this map and a
// layer's albedo, the layer takes the material's albedo, normal (+y up: normalUp_) and ARH (creases, roughness,
// height: its path into `arh`), at the map's own scale for the layer times scale_mul, or at "scale".
void Terrain::Remaster(const MapData& m, std::vector<std::string>* arh) {
  remasteredLayers_ = 0;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string folder = kRemasteredTerrain;
  auto file = textures_->ReadFile(folder + "/remap.json");
  if (!file) return;
  JsonValue remap;
  std::string why;
  if (!ReadJson(*file, &remap, &why)) {
    fprintf(stderr, "render: the remastered ground's remap.json cannot be read: %s\n", why.c_str());
    return;
  }
  const JsonValue& layers = remap["maps"][lower(mapPath)];
  if (layers.kind != JsonValue::Object) return;
  const char* tier = ultraGround ? "8k" : "4k";
  for (int i = 0; i < 10; ++i) {
    const JsonValue& entry = layers[lower(m.albedo[i].texture)];
    const std::string name = entry["material"].StringOr(std::string());
    const std::string dir = remap["materials"][name]["dir"].StringOr(std::string());
    if (name.empty() || dir.empty()) continue;
    const std::string base = folder + dir + "/" + name + "_";
    if (!textures_->ReadFile(base + "albedo_" + tier + ".dds")) {
      fprintf(stderr, "render: the remastered ground's %s has no %s set\n", name.c_str(), tier);
      continue;
    }
    albedo_[i] = textures_->Get(base + "albedo_" + tier + ".dds", albedo_[i]);
    const double scale = entry["scale"].NumberOr(0), mul = entry["scale_mul"].NumberOr(1);
    albedoScale_[i] = scale > 0 ? static_cast<float>(scale) : m.albedo[i].scale * static_cast<float>(mul);
    if (i < 9) {
      normal_[i] = textures_->Get(base + "normal_" + tier + ".dds", normal_[i]);
      normalScale_[i] = albedoScale_[i];  // (the material's normal lies on its own albedo)
      normalUp_[i] = true;
      (*arh)[static_cast<size_t>(i)] = base + "arh_" + tier + ".dds";
    }
    ++remasteredLayers_;
  }
  if (remasteredLayers_) printf("render: %d of the map's ground layers remastered (%s)\n", remasteredLayers_, tier);
}

bool Terrain::Load(const MapData* map, std::string* error) {
  map_ = map;
  if (groundColours_) device_->Destroy(groundColours_);  // (GroundColours: made again for this map when asked for)
  groundColours_ = {};
  const int w = map->width, h = map->height;

  if (!pipeline_) {
    // (the modern look has its own water: water.cpp)
    std::string source = std::string("#define MODERN_WATER 1\n") + "#define SURFACE_PUDDLES 1.0\n#define SURFACE_TRACKS 1\n" + kViewShaderBlock + RayShaders() +
                         kLightingShaderBlock + kShader;
    gfx::PipelineDesc pd;
    pd.name = "terrain";
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float2, 0}};
    pd.stride = 8;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pipeline_ = device_->CreatePipeline(pd, error);
    if (!pipeline_) return false;
    // into a shadow map: the same vertices, depth only
    pd.name = "terrain, shadow";
    pd.pixelSource = {};
    pd.depthOnly = true;
    pd.samples = 1;
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
  for (int i = 0; i < 10; ++i) albedoScale_[i] = map->albedo[i].scale;
  for (int i = 0; i < 9; ++i) normalScale_[i] = map->normal[i].scale, normalUp_[i] = false;
  {  // a remastered map's third maps: beside each layer's `<name>_albedo.dds` a `<name>_arh.dds`
    std::vector<std::string> paths(9);
    for (size_t i = 0; i < 9; ++i) {
      std::string lower = map->albedo[i].texture;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t at = lower.rfind("_albedo.dds");
      if (at != std::string::npos) paths[i] = map->albedo[i].texture.substr(0, at) + "_arh.dds";
    }
    Remaster(*map, &paths);  // (the visuals track's remastered ground, where it lists this map)
    std::vector<bool> present;
    materials_ = textures_->Layers(paths, &present);
    for (size_t i = 0; i < 9; ++i) material_[i] = materials_ && present[i];
    if (MaterialLayers()) printf("render: %d of the map's ground layers have a third map (creases, roughness, height)\n", MaterialLayers());
  }
  shaderKind_ = map->shader == "TTerrainXP" ? 1 : map->shader == "TTerrainGlow" ? 2 : 0;
  if (map->shader != "TTerrain" && map->shader != "TTerrainXP" && map->shader != "TTerrainGlow")
    fprintf(stderr, "render: the map asks for the terrain look \"%s\", which is not written yet; using TTerrain\n", map->shader.c_str());

  // the tree of nodes: per level, each node's lowest and highest ground (for culling and for distances)
  // (a node of the finest level holds the points [n * kLeafCells, (n + 1) * kLeafCells] each way;
  // each coarser node the four below it)
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

// Where a box of the map (x0..x1, z0..z1) lies in the world for the copy of the map being gone
// through (tileX_, tileZ_: 0, 0 the map itself; an odd one is mirrored).
void Terrain::InWorld(float x0, float z0, float x1, float z1, float lo, float hi, Vec3* a, Vec3* b) const {
  const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
  const float ox = static_cast<float>(tileX_) * w, oz = static_cast<float>(tileZ_) * h;
  const bool mx = (tileX_ & 1) != 0, mz = (tileZ_ & 1) != 0;
  *a = {ox + (mx ? w - x1 : x0), lo, oz + (mz ? h - z1 : z0)};
  *b = {ox + (mx ? w - x0 : x1), hi, oz + (mz ? h - z0 : z1)};
}

void Terrain::AddPatch(const View& view, int level, float x, float z, float size, float lo, float hi) {
  const float w = static_cast<float>(map_->width), h = static_cast<float>(map_->height);
  if (x >= w || z >= h) return;
  Vec3 a, b;
  InWorld(x, z, std::min(x + size, w), std::min(z + size, h), lo, hi, &a, &b);
  if (!view.BoxVisible(a, b)) return;
  if (tileX_ || tileZ_) {
    // (a copy of the map: only as far out as the ground is drawn at all)
    const float dx = std::max({-b.x, a.x - w, 0.0f}), dz = std::max({-b.z, a.z - h, 0.0f});
    if (dx * dx + dz * dz > beyondReach * beyondReach) return;
    // (the patch whole, not cut at the copy's edge: what laps over is the same ground again)
    InWorld(x, z, x + size, z + size, lo, hi, &a, &b);
  }
  patches_.push_back({a.x, a.z, size / kPatchQuads, static_cast<float>(level)});
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
  Vec3 lo, hi;
  InWorld(x, z, std::min(x + static_cast<float>(L.nodeCells), static_cast<float>(map_->width)),
          std::min(z + static_cast<float>(L.nodeCells), static_cast<float>(map_->height)), L.lo[at], L.hi[at], &lo, &hi);
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

void Terrain::HeightsChanged(int x0, int z0, int x1, int z1) {
  if (!map_ || levels_.empty() || !heightTexture_) return;
  const int w = map_->width, h = map_->height;
  x0 = std::max(x0, 0), z0 = std::max(z0, 0), x1 = std::min(x1, w), z1 = std::min(z1, h);
  if (x0 > x1 || z0 > z1) return;
  // the height texture's rectangle, from the map's heights as they are now
  device_->UpdateTexture(heightTexture_, x0, z0, x1 - x0 + 1, z1 - z0 + 1,
                         map_->heights.data() + static_cast<size_t>(z0) * static_cast<size_t>(w + 1) + static_cast<size_t>(x0),
                         static_cast<uint32_t>((w + 1) * sizeof(uint16_t)));
  // the finest nodes those points are in (a point on a node's edge is in the nodes on both sides), anew
  Level& leaf = levels_[0];
  const int nx0 = std::max(0, (x0 - 1) / kLeafCells), nx1 = std::min(leaf.nx - 1, x1 / kLeafCells);
  const int nz0 = std::max(0, (z0 - 1) / kLeafCells), nz1 = std::min(leaf.nz - 1, z1 / kLeafCells);
  for (int nz = nz0; nz <= nz1; ++nz)
    for (int nx = nx0; nx <= nx1; ++nx) {
      uint16_t lo = 65535, hi = 0;
      const int px1 = std::min(w, (nx + 1) * kLeafCells), pz1 = std::min(h, (nz + 1) * kLeafCells);
      for (int z = nz * kLeafCells; z <= pz1; ++z) {
        const uint16_t* p = map_->heights.data() + static_cast<size_t>(z) * static_cast<size_t>(w + 1);
        for (int x = nx * kLeafCells; x <= px1; ++x) {
          lo = std::min(lo, p[x]);
          hi = std::max(hi, p[x]);
        }
      }
      const size_t at = static_cast<size_t>(nz) * static_cast<size_t>(leaf.nx) + static_cast<size_t>(nx);
      leaf.lo[at] = static_cast<float>(lo) * map_->heightScale;
      leaf.hi[at] = static_cast<float>(hi) * map_->heightScale;
    }
  // and the coarser nodes above them, each from the four below it
  int ax0 = nx0, ax1 = nx1, az0 = nz0, az1 = nz1;
  for (size_t k = 1; k < levels_.size(); ++k) {
    const Level& below = levels_[k - 1];
    Level& level = levels_[k];
    ax0 /= 2, ax1 /= 2, az0 /= 2, az1 /= 2;
    for (int nz = az0; nz <= std::min(az1, level.nz - 1); ++nz)
      for (int nx = ax0; nx <= std::min(ax1, level.nx - 1); ++nx) {
        float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
        for (int cz = nz * 2; cz <= std::min(nz * 2 + 1, below.nz - 1); ++cz)
          for (int cx = nx * 2; cx <= std::min(nx * 2 + 1, below.nx - 1); ++cx) {
            const size_t from = static_cast<size_t>(cz) * static_cast<size_t>(below.nx) + static_cast<size_t>(cx);
            lo = std::min(lo, below.lo[from]);
            hi = std::max(hi, below.hi[from]);
          }
        const size_t to = static_cast<size_t>(nz) * static_cast<size_t>(level.nx) + static_cast<size_t>(nx);
        level.lo[to] = lo;
        level.hi[to] = hi;
      }
  }
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
  // the map, and (not into shadow maps) the eight copies of it round it that the ground beyond
  // its edge is made of
  const int round = beyondReach > 0 && !shadowPass ? 1 : 0;
  for (tileZ_ = -round; tileZ_ <= round; ++tileZ_)
    for (tileX_ = -round; tileX_ <= round; ++tileX_)
      for (int iz = 0; iz < T.nz; ++iz)
        for (int ix = 0; ix < T.nx; ++ix) Select(view, top, ix, iz);
  tileX_ = tileZ_ = 0;
  if (patches_.empty()) return;

  const MapData& m = *map_;
  TerrainConstants c = {};
  c.mapSize[0] = static_cast<float>(m.width);
  c.mapSize[1] = static_cast<float>(m.height);
  c.mapSize[2] = 1.0f / static_cast<float>(m.width);
  c.mapSize[3] = 1.0f / static_cast<float>(m.height);
  c.params[0] = 65535.0f * m.heightScale;
  c.params[1] = shaderKind_ == 1 ? 1.0f : 0.0f;
  c.params[2] = shaderKind_ == 2 ? 1.0f : 0.0f;
  c.params[3] = beyondReach > 0 ? 1.0f : 0.0f;
  c.specularColor[0] = m.lighting.specular.x;
  c.specularColor[1] = m.lighting.specular.y;
  c.specularColor[2] = m.lighting.specular.z;
  c.specularColor[3] = m.lighting.specular.w;
  for (int i = 0; i < 10; ++i) c.albedoTile[i][0] = albedoScale_[i] > 0 ? 1.0f / albedoScale_[i] : 0;
  for (int i = 0; i < 9; ++i) c.albedoTile[i][1] = richGround && material_[i] ? 1.0f : 0.0f;
  for (int i = 0; i < 9; ++i) c.normalTile[i][0] = normalScale_[i] > 0 ? 1.0f / normalScale_[i] : 0;
  for (int i = 0; i < 9; ++i) c.normalTile[i][1] = normalUp_[i] ? 1.0f : 0.0f;
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
    if (materials_) device_->SetTexture(25, materials_);
    device_->SetTexture(26, tracksMap ? tracksMap : textures_->Black());
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
