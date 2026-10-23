#include "tarmacs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hitch_log.h"  // [hitches]
#include "decals.h"  // [padsnow] (kMarkSnowLying)
#include "json.h"
#include "lighting.h"
#include "ray_scene.h"
#include "remasters.h"

namespace render {
namespace {

constexpr size_t kMaxPads = 256;   // per draw call (the size of the shader's array)
constexpr int kCoverSize = 256;    // the cleared cover's picture, texels a side (as the grass's trampled picture)
constexpr size_t kMaxCoverPads = 128;  // per draw of it

// A pad is drawn as the decals are (decals.cpp): the far faces of its box, each pixel's place on the ground from the
// scene's depth, given up outside its footprint. Then it is the material's picture at that place, read through its
// relief close by, lit with the modern light.
const char kShaderHead[] = R"hlsl(
struct Pad {
  float4 centre;   // xyz; w: half the box's height
  float4 shape;    // x, y: half its size along its own x and z (its extent taken in); z, w: its heading's cos, sin
  float4 options;  // x: how much of it is there (its fade); y: how much of the lying snow covers it (as a decal's mark);
                   // z: its relief's depth, world units from its lowest to its highest point; w: unused
};
cbuffer DecalCamera : register(b1) {  // (as the decals' own, decals.cpp)
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // x, y: 1 / the map's size; z: 1 = the map has water; w: the water's level
#ifdef DECAL_SNOW_GROUND
  float4 snowWind;     // [padsnow] as the decals' (decals.cpp): the wind's way, 1 / the map's size
  float4 snowTune;     // [padsnow] Terrain::snowTune
  float4 snowMore;     // [padsnow] x 1 = the snow's materials are drawn
#endif
};
cbuffer Pads : register(b2) {
  Pad pads[256];
};
cbuffer PadLook : register(b3) {
  float4 padParallax;     // x 1 = its relief close by; y the distance from the eye it is full within; z 1 / the length it fades
                          // out over; w how much the relief shades itself from the sun (the ground's: terrain.cpp)
  float4 tracksLevel[3];  // deep tracks' field (deep_tracks.h, TrackFieldConstants), as the ground's shader has them
  float4 tracksAtlas[3];
  float4 tracksInfo;      // x, y: 1 / the atlas's size; z: 1 = deep tracks are read
};
// (the decals' pass's own slots: t1, t3, t4 as the decals bind them; t26 is the ground's picture of its tracks, lighting.h)
Texture2D padAlbedo : register(t0);    // the material's albedo (sRGB values; alpha the pad's coverage, its worn rim)
Texture2D padNormal : register(t2);    // its normal: x in red, y in green, y up the picture (BC5)
Texture2D waterMap : register(t3);
Texture2D waterRamp : register(t4);
Texture2D padMaterial : register(t5);  // r what its creases let in of the sky's light, g roughness, b height (0.5 the ground's surface)
#ifdef DECAL_SNOW_GROUND
Texture2D snowShelter : register(t6);  // [padsnow] the snow ground's shelter (as the decals')
#endif
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

)hlsl";

// ([padsnow] the pads' shader is the head above and the tail below; with the snow ground decals.cpp's MarkSnowLying between)
const char kShaderTail[] = R"hlsl(struct VOut {
  float4 position : SV_Position;
  nointerpolation float4 centre : TEXCOORD0;
  nointerpolation float4 shape : TEXCOORD1;
  nointerpolation float4 options : TEXCOORD2;
};

VOut VS(float3 corner : POSITION, uint instance : SV_InstanceID) {
  Pad n = pads[instance];
  float2 flat = corner.xz * n.shape.xy;
  float3 world = n.centre.xyz + float3(flat.x * n.shape.z + flat.y * n.shape.w, corner.y * n.centre.w, -flat.x * n.shape.w + flat.y * n.shape.z);
  VOut o;
  o.position = mul(viewProj, float4(world, 1));
  o.centre = n.centre;
  o.shape = n.shape;
  o.options = n.options;
  return o;
}

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / depth;
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}

// A place's x, z in the pad's picture (0..1 across it: u along its own x, v along its own z, as a decal's).
float2 PadUv(float2 xz, float4 centre, float4 shape) {
  float2 d = xz - centre.xz;
  return float2(d.x * shape.z - d.y * shape.w, d.x * shape.w + d.y * shape.z) / shape.xy * 0.5 + 0.5;
}

// The pad's relief close by (World::groundParallax): the ground's march (terrain.cpp, GroundParallax), copied for one
// material. Its third map's blue holds how high each point of the picture stands (0.5 the ground's surface, which the
// marks laid on it lie on); the place whose picture a pixel shows is found by following the eye's ray down through that
// relief: even steps, then the crossing between the last two halved three times; and how much of the sun's light its
// own high places leave it.
struct PadRelief {
  float4 centre, shape;
  float deep, full;  // its depth as faded, and whole (world units)
  float2 gx, gy;     // the picture's steps (a level less sharp than the pictures': steadier, and no thin walls)
};
float PadHeightAt(PadRelief r, float2 xz) {
  return (padMaterial.SampleGrad(samplerLinearClamp, saturate(PadUv(xz, r.centre, r.shape)), r.gx, r.gy).b - 0.5) * r.deep;
}
float2 PadParallax(PadRelief r, float3 world, float3 base, float2 stepX, float2 stepY, out float sunSeen) {
  sunSeen = 1;
  float3 V = world - cameraPosition;
  float dist = length(V);
  V /= max(dist, 1e-4);
  float facing = max(-dot(V, base), 0.1);
  float2 along = (V.xz + base.xz * facing) / facing;  // (the ray's way over the ground per unit of depth)
  float pixel = max(max(length(stepX), length(stepY)), 1e-6);
  float across = r.full * length(along) / pixel;      // (pixels it crosses the whole relief over)
  float fade = saturate((padParallax.y - dist) * padParallax.z + 1) * saturate(across - 0.5);
  [branch] if (fade <= 0) return world.xz;
  r.deep = r.full * fade;
  float half = 0.5 * r.deep;
  float2 from = world.xz - along * half, way = along * (2 * half);
  int steps = int(clamp(ceil(across * fade * 0.5), 4, 24));
  float t = 0, gap = half - PadHeightAt(r, from), lastT = 0, lastGap = gap;
  [loop] for (int k = 1; k <= steps; ++k) {
    t = float(k) / steps;
    gap = half * (1 - 2 * t) - PadHeightAt(r, from + way * t);
    if (gap <= 0) break;
    lastT = t, lastGap = gap;
  }
  [loop] for (int h = 0; h < 3 && gap < 0; ++h) {
    float middle = 0.5 * (lastT + t), g = half * (1 - 2 * middle) - PadHeightAt(r, from + way * middle);
    if (g > 0) lastT = middle, lastGap = g;
    else t = middle, gap = g;
  }
  float hit = gap < 0 ? lerp(lastT, t, lastGap / max(lastGap - gap, 1e-6)) : t;
  float2 at = from + way * hit;
  [branch] if (padParallax.w > 0 && sunDirection.y > 0) {
    float rise = dot(sunDirection, base);
    [branch] if (rise > 0.02) {
      float2 toSun = (sunDirection.xz - base.xz * rise) / max(rise, 0.15);
      float z = half * (1 - 2 * hit), room = half - z, most = 0;
      int sunSteps = int(clamp(ceil(room * length(toSun) / pixel * 0.5), 2, 6));
      [loop] for (int j = 1; j <= sunSteps; ++j) {
        float climb = room * j / sunSteps;
        most = max(most, PadHeightAt(r, at + toSun * climb) - (z + climb));
      }
      sunSeen = lerp(1, saturate(1 - most / (0.25 * r.full)), padParallax.w);
    }
  }
  return at;
}

// Deep tracks (deep_tracks.h) over the pad, read as the ground's shader reads them (terrain.cpp: TrackTap, TrackInside,
// TrackShape, TrackLevel, copied), the pad taken as hard ground: scuffed, a little darker where wheels and treads pass and
// darker and smoother where many have, the snow on it packed along them (ModernLight, from deepTrack).
float2 TrackTap(int level, float2 xz) {
  float4 L = tracksLevel[level], A = tracksAtlas[level];
  float2 s = xz * L.z;
  float2 c = A.z > 0.5 ? s - L.w * floor(s / L.w) : clamp(s, 0.5, L.w - 0.5);
  return tracksMap.SampleLevel(samplerLinearClamp, (A.xy + c) * tracksInfo.xy, 0).rg;
}
float TrackInside(int level, float2 xz) {
  float4 L = tracksLevel[level];
  float2 s = xz * L.z - L.xy;
  return L.w > 0 ? min(min(s.x, s.y), min(L.w - s.x, L.w - s.y)) : -1;
}
float2 TrackShape(float2 pressed) {
  float t = 1 - exp(-1.5 * pressed.x);
  return float2(t, (1 - exp(-1.5 * pressed.y)) * (1 - t));
}
void TrackLevel(int level, float2 xz, out float2 shape, out float4 slope) {
  float e = tracksAtlas[level].w;
  float2 a = TrackShape(TrackTap(level, xz + float2(e, 0))), b = TrackShape(TrackTap(level, xz - float2(e, 0)));
  float2 c = TrackShape(TrackTap(level, xz + float2(0, e))), d = TrackShape(TrackTap(level, xz - float2(0, e)));
  shape = (a + b + c + d) * 0.25;
  slope = float4(a.x - b.x, c.x - d.x, a.y - b.y, c.y - d.y) / (2 * e);
}
void PadTracks(float2 xz, float2 stepX, float2 stepY, inout float3 albedo, inout float roughness) {
  float pixel = max(length(stepX), length(stepY));
  float in0 = TrackInside(0, xz), in1 = TrackInside(1, xz);
  float w0 = saturate((in0 - 8) / 96) * saturate((2.5 * tracksAtlas[0].w - pixel) / (1.5 * tracksAtlas[0].w));
  float w1 = saturate((in1 - 8) / 96) * saturate((2.5 * tracksAtlas[1].w - pixel) / (1.5 * tracksAtlas[1].w));
  float2 c0 = 0, c1 = 0;
  [branch] if (w0 > 0) c0 = TrackTap(0, xz);
  [branch] if (w0 < 1 && w1 > 0) c1 = TrackTap(1, xz);
  float2 c2 = TrackTap(2, xz);
  if (max(max(c0.x + c0.y, c1.x + c1.y), c2.x) < 0.004) return;  // (nothing pressed here: most of it)
  float2 shape = 0, s1 = 0;
  float4 slope = 0, g1 = 0;
  [branch] if (w0 > 0) TrackLevel(0, xz, shape, slope);
  [branch] if (w0 < 1 && w1 > 0) TrackLevel(1, xz, s1, g1);
  shape = lerp(s1, shape, w0);
  slope = lerp(g1, slope, w0);
  float raw = lerp(c1.x, c0.x, w0);  // (how often it has been pressed)
  float relief = w0 + (1 - w0) * w1;
  float far = TrackShape(c2).x * 0.6;
  float t = lerp(far, shape.x, relief), q = shape.y * relief;
  float worn = smoothstep(1.5, 5.0, raw) * relief;
  albedo *= lerp(1, 0.86, t) * lerp(1, 0.9, worn);
  roughness = lerp(roughness, roughness * 0.8, max(t, worn) * 0.6);
  deepTrack = float4(t, q, relief, 1);
  deepSlope = slope * relief;
}

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  if (depth <= 0) discard;  // nothing drawn here
  float3 world = WorldAt(i.position.xy, depth);
  // the ground's slope from how its place changes from pixel to pixel (as the decals take it), and the steps the
  // pictures are read with (the temporal upscaler's bias as steps that much smaller, as the ground's)
  float3 base = normalize(cross(ddy(world), ddx(world)));
  if (base.y < 0) base = -base;
  float bias = exp2(temporal.x);
  float2 uv = PadUv(world.xz, i.centre, i.shape);
  float2 stepX = ddx(world.xz) * bias, stepY = ddy(world.xz) * bias, gx = ddx(uv) * bias, gy = ddy(uv) * bias;
  if (abs(world.y - i.centre.y) > i.centre.w || any(abs(uv - 0.5) > 0.5)) discard;
  // its relief close by: where the eye's ray meets it
  float2 at = world.xz;
  float sunSeen = 1;
  [branch] if (padParallax.x > 0.5 && i.options.z > 0 && distance(world, cameraPosition) < padParallax.y + 1 / padParallax.z) {
    PadRelief r;
    r.centre = i.centre;
    r.shape = i.shape;
    r.deep = r.full = i.options.z;
    r.gx = gx * 2, r.gy = gy * 2;
    at = PadParallax(r, world, base, stepX, stepY, sunSeen);
    uv = PadUv(at, i.centre, i.shape);
  }
  float4 c = padAlbedo.SampleGrad(samplerAniso, uv, gx, gy);
  c.a *= i.options.x * (all(abs(uv - 0.5) <= 0.5) ? 1 : 0);
  if (c.a <= 0.003) discard;
#ifdef DECAL_SNOW_GROUND
  // [padsnow] (Tarmacs::snowShelter) the snow that lies over the pad is the ground's own: the pad thins out where the snow lies
  // over it (on the ground's own slope, at the ground's surface), and is lit bare (below). Only where it surely lies (the
  // reckoning's upper half): where the snow thins, the ground's breaks up by its layers' heights close by, which the pad
  // cannot know, and a pad gone there would show the ground's bare earth in its place; it shows its own slabs instead, an
  // apron kept clearer than the field round it.
  [branch] if (weather.y > 0 && i.options.y > 0) {
    c.a *= 1 - smoothstep(0.5, 0.95, MarkSnowLying(world, base, weather.y * i.options.y));
    if (c.a <= 0.003) discard;
  }
#endif
  float3 arh = padMaterial.SampleGrad(samplerAniso, uv, gx, gy).rgb;
  // its own normal (y up the picture: toward the pad's -z; a mirrored pad's mirrored), turned as the pad is, then put on
  // the ground's slope as the ground's layers are (terrain.cpp)
  float2 t = padNormal.SampleGrad(samplerAniso, uv, gx, gy).rg * 2 - 1;
  float3 inPad = float3(t.x * sign(i.shape.x), sqrt(saturate(1 - dot(t, t))), -t.y * sign(i.shape.y));
  float3 detail = float3(inPad.x * i.shape.z + inPad.z * i.shape.w, inPad.y, -inPad.x * i.shape.w + inPad.z * i.shape.z);
  float3 h = normalize(base + float3(0, 1, 0));
  float3 axisX = float3(1 - 2 * h.x * h.x, 2 * h.x * h.y, -2 * h.x * h.z);
  float3 axisZ = float3(-2 * h.z * h.x, 2 * h.z * h.y, 1 - 2 * h.z * h.z);
  float3 N = normalize(float3(dot(detail, axisX), dot(detail, base), dot(detail, axisZ)));
  float3 albedo = ToLinear(c.rgb);
  float roughness = arh.g;
  [branch] if (tracksInfo.z > 0.5) PadTracks(at, stepX, stepY, albedo, roughness);
  // rain: the water stands where the pad is lower than round about (its height against its own mean over a third of a
  // world unit, a coarse level of the same map): its joints first, then the dips in its slabs and along their foot, more
  // of them as it gets wetter (patchy: the pad is not quite level); not on the gravel's broad apron, which drains; on
  // level ground only. lighting.h takes it as a puddle.
  [branch] if (weather.x > 0) {
    uint w, hgt, levels;
    padMaterial.GetDimensions(0, w, hgt, levels);
    float around = padMaterial.SampleLevel(samplerLinearClamp, uv, log2(max(0.33 * w / (2 * abs(i.shape.x)), 1))).b;
    float hollow = around - arh.b + (CloudNoise(float3(at * 0.9, 6.1)) - 0.5) * 0.06;
    float wet = saturate(weather.x);
    deepWater = smoothstep(lerp(0.16, 0.02, wet), lerp(0.2, 0.06, wet), hollow) * smoothstep(0.93, 0.985, base.y) * saturate(wet * 1.6 - 0.2);
  }
  float shadow = SunShadow(world, base) * sunSeen;
#ifdef DECAL_SNOW_GROUND
  markSnow = 0;  // [padsnow]
#else
  markSnow = i.options.y;  // (as a decal's mark: laid on lying snow, it shows on it until new snow covers it)
#endif
  float3 lit = ModernLight(albedo, 0, roughness, arh.r, 0, N, world, shadow);
#ifdef MSAA
  return float4(lit, c.a);  // (the modern water colours what is under it itself)
#endif
  // (as the decals: under water tinted toward the water ramp's colour by depth)
  float4 tint = 0;
  if (mapInfo.z > 0.5) tint = waterRamp.Sample(samplerLinearClamp, float2(waterMap.Sample(samplerLinearClamp, world.xz * mapInfo.xy).g, 0.5));
  float full = max(lightMultiplier * modern.x * (skyZenith.w > 0.5 ? skyGlow.w * lerp(1, 0.45, skyHorizon.w) : 1), 0.001);
  float3 screen = lerp(pow(max(lit / full, 0), 1 / 2.2), tint.rgb, tint.a);
  return float4(ToLinear(screen) * full, c.a);
}
)hlsl";

// The ground cover cleared under the pads (ClearCover): each pad's coverage drawn over the grass's square, added.
const char kCoverShader[] = R"hlsl(
cbuffer Cover : register(b1) {
  float4 area;          // the grass's square: its first cell's x, z; a cell's size; cells a side
  float4 covers[256];   // two a pad: x, z of its middle, half its size along its own x and z; its heading's cos, sin, its fade,
                        // the level its picture is read at
};
Texture2D padAlbedo : register(t0);  // (its alpha: the pad's coverage)
struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  nointerpolation float2 more : TEXCOORD1;  // its fade, the level
};
static const float2 kCorner[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
VOut VS(uint id : SV_VertexID) {
  uint k = id / 6;
  float4 a = covers[k * 2], b = covers[k * 2 + 1];
  float2 corner = kCorner[id % 6], flat = corner * a.zw;
  float2 xz = a.xy + float2(flat.x * b.x + flat.y * b.y, -flat.x * b.y + flat.y * b.x);
  float wide = area.z * area.w;
  VOut o;
  o.position = float4((xz.x - area.x) / wide * 2 - 1, 1 - (xz.y - area.y) / wide * 2, 0, 1);
  o.uv = corner * 0.5 + 0.5;
  o.more = b.zw;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float a = padAlbedo.SampleLevel(samplerLinearClamp, i.uv, i.more.y).a * i.more.x;
  if (a < 0.003) discard;
  return float4(1, 1, 1, a);
}
)hlsl";

struct PadLook {
  float parallax[4];
  float tracksLevel[3][4], tracksAtlas[3][4], tracksInfo[4];
};

}  // namespace

Tarmacs::~Tarmacs() {
  for (gfx::Pipeline p : {pipeline_, cover_, snowPipeline_})
    if (p) device_->Destroy(p);
  if (coverField_) device_->Destroy(coverField_);
}

std::string Tarmacs::Key(const std::string& texture) {
  std::string k = texture;
  for (char& c : k) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (k.find("tarmac") == std::string::npos) return {};
  for (const char* folder : {"/env/common/decals/", "/env/common/splats/"})
    if (k.compare(0, std::strlen(folder), folder) == 0) k = k.substr(std::strlen(folder));
  if (!k.empty() && k[0] == '/') k = k.substr(1);
  if (k.size() > 4 && k.compare(k.size() - 4, 4, ".dds") == 0) k.resize(k.size() - 4);
  return k;
}

// The table (remap.json's top level "tarmacs"): each game picture's key, and its material from "materials" (the files'
// folder, real_size_m, extent, its tiers).
bool Tarmacs::ReadTable() {
  if (tableRead_) return !table_.empty();
  tableRead_ = true;
  const std::string folder = kRemasteredTerrain;
  auto file = textures_->ReadFile(folder + "/remap.json");
  if (!file) return false;
  JsonValue remap;
  std::string why;
  if (!ReadJson(*file, &remap, &why)) {
    fprintf(stderr, "render: remastered tarmacs: the remastered ground's remap.json cannot be read: %s\n", why.c_str());
    return false;
  }
  std::unordered_map<std::string, int> byName;
  for (const auto& [picture, value] : remap["tarmacs"].members) {
    const std::string name = value.StringOr(std::string()), key = Key(picture);
    const JsonValue& m = remap["materials"][name];
    const std::string dir = m["dir"].StringOr(std::string());
    if (key.empty() || name.empty() || dir.empty()) {
      fprintf(stderr, "render: remastered tarmacs: %s -> %s: no such material in remap.json\n", picture.c_str(), name.c_str());
      continue;
    }
    auto known = byName.find(name);
    if (known == byName.end()) {
      Material mat;
      mat.name = name;
      mat.base = folder + dir + "/" + name + "_";
      mat.realSize = static_cast<float>(m["real_size_m"].NumberOr(0));
      mat.extent = std::clamp(static_cast<float>(m["extent"].NumberOr(1)), 1.0f, 2.0f);
      for (const JsonValue& t : m["tiers"].items) mat.has8k = mat.has8k || t.StringOr(std::string()) == "8k";
      known = byName.emplace(name, static_cast<int>(materials_.size())).first;
      materials_.push_back(mat);
    }
    table_[key] = known->second;
  }
  printf("render: remastered tarmacs: %zu of the game's pictures in %zu materials\n", table_.size(), materials_.size());
  return !table_.empty();
}

int Tarmacs::MaterialOf(const std::string& key) {
  if (key.empty() || !ReadTable()) return -1;
  // (the "_normals" twin by its "_albedo" picture's name)
  std::string k = key;
  if (const size_t n = k.rfind("_normals"); n != std::string::npos) k = k.substr(0, n) + "_albedo" + k.substr(n + 8);
  auto it = table_.find(k);
  // (a Seraphim pad's picture has the kind of ground it stands on after its name, tarmacs.lua: "..._albedo_evergreen";
  // where the table names only the plain picture, its material serves them all)
  if (const size_t a = k.rfind("_albedo_"); it == table_.end() && a != std::string::npos) it = table_.find(k.substr(0, a + 7));
  if (it == table_.end() || materials_[static_cast<size_t>(it->second)].failed) return -1;
  return it->second;
}

float Tarmacs::Extent(int material) const {
  return material >= 0 && material < static_cast<int>(materials_.size()) ? materials_[static_cast<size_t>(material)].extent : 1.0f;
}

// Its relief's depth: `relief` world units at the size its picture was made for (real_size_m, the size it is drawn at
// most often), larger as it is drawn larger (the same picture over a wider pad: its slabs and joints all larger), times
// `depth`; at most a quarter of a world unit, as the ground's (the marks on it lie on the relief's middle).
float Tarmacs::Depth(int material, float width) const {
  if (material < 0 || material >= static_cast<int>(materials_.size())) return 0;
  const float real = materials_[static_cast<size_t>(material)].realSize;
  const float scale = real > 0 ? std::fabs(width) / real : 1.0f;
  return std::min(relief * scale, 0.25f) * depth;
}

// A material's three pictures at the tier wanted (read again when that changes); false where they cannot be read (its
// pads are then drawn as the game's).
bool Tarmacs::Load(Material& m) {
  if (m.failed) return false;
  const std::string tier = ultra && m.has8k ? "8k" : "4k";
  if (m.tier == tier) return true;
  HitchLog::Scope made("remastered tarmac material", m.name + " " + tier);  // [hitches]
  const auto start = std::chrono::steady_clock::now();
  textures_->Fetch({m.base + "albedo_" + tier + ".dds", m.base + "normal_" + tier + ".dds", m.base + "arh_" + tier + ".dds"});  // (the three read side by side)
  m.albedo = textures_->Get(m.base + "albedo_" + tier + ".dds");
  m.normal = textures_->Get(m.base + "normal_" + tier + ".dds", textures_->FlatNormal());
  m.arh = textures_->Get(m.base + "arh_" + tier + ".dds");
  if (!m.albedo || !m.arh) {
    fprintf(stderr, "render: remastered tarmacs: %s's %s pictures cannot be read: its pads drawn as the game's\n", m.name.c_str(), tier.c_str());
    m.failed = true;
    return false;
  }
  m.tier = tier;
  printf("render: remastered tarmacs: %s read (%s) in %.0f ms\n", m.name.c_str(), tier.c_str(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return true;
}

// [hitches] (Want, Ahead: tarmacs.h)
void Tarmacs::Want(int material) {
  if (!readAhead || material < 0 || material >= static_cast<int>(materials_.size())) return;
  Material& m = materials_[static_cast<size_t>(material)];
  const std::string tier = TierOf(m);
  if (m.failed || m.tier == tier || m.aheadTier == tier) return;
  m.aheadTier = tier;
  // [spikes] (made ahead off the frames' thread, TextureCache::MakeAhead: was read ahead and made here one a frame, 5-8 ms each)
  textures_->MakeAhead({m.base + "albedo_" + tier + ".dds", m.base + "normal_" + tier + ".dds", m.base + "arh_" + tier + ".dds"});
}

void Tarmacs::Ahead() {
  if (!readAhead) return;
  if (!made_ && ReadTable()) Pipelines();  // (the first frame the pads are on: their pipelines)
  for (Material& m : materials_) {
    if (m.failed || m.aheadTier.empty() || m.tier == m.aheadTier || m.aheadTier != TierOf(m)) continue;
    const std::string files[3] = {m.base + "albedo_" + m.aheadTier + ".dds", m.base + "normal_" + m.aheadTier + ".dds", m.base + "arh_" + m.aheadTier + ".dds"};
    bool made = true;  // [spikes] (its pictures made ahead, off the frames' thread: Want)
    for (const std::string& f : files) made = made && !textures_->Ahead(f);
    if (!made) continue;
    Load(m);  // (its pictures made already: Load takes them as they are)
    return;
  }
}

// The pads' shader source ([padsnow] `snow`: with the snow ground's, Tarmacs::snowShelter).
// (the ground's tracks and puddles as lighting.h has them for the ground: SURFACE_TRACKS, SURFACE_DEEP_TRACKS with the
// shader's own deepTrack, deepSlope, deepWater; no puddles of the weather's own patches, the pad's gather by its relief;
// the lying snow on it as on a decal's mark)
static std::string PadSource(bool snow) {
  return std::string(snow ? "#define DECAL_SNOW_GROUND 1\n" : "") + (gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "static float markSnow = 1;\n#define SURFACE_SNOW markSnow\n" +
         "#define SURFACE_TRACKS 1\n#define SURFACE_DEEP_TRACKS 1\nstatic float4 deepTrack = 0;\nstatic float4 deepSlope = 0;\n" +
         "static float deepWater = 0;\nstatic float2 deepGloss = 0;\n" + kViewShaderBlock + RayShaders() + kLightingShaderBlock + kShaderHead +
         (snow ? kMarkSnowLying : "") + kShaderTail;
}

bool Tarmacs::SnowPipeline() {
  if (snowMade_) return snowMade_ > 0;
  snowMade_ = -1;
  if (!Pipelines()) return false;
  std::string error;
  const std::string source = PadSource(true);  // (kept while the pipeline is made: the description points into it)
  gfx::PipelineDesc pd;
  pd.name = "remastered tarmacs, snow ground";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  snowPipeline_ = device_->CreatePipeline(pd, &error);
  if (!snowPipeline_) {
    fprintf(stderr, "render: remastered tarmacs cannot lay the snow ground's snow: %s\n", error.c_str());
    return false;
  }
  snowMade_ = 1;
  return true;
}

bool Tarmacs::Pipelines() {
  if (made_) return made_ > 0;
  made_ = -1;
  std::string error;
  const std::string source = PadSource(false);  // (PadSource says what it holds)
  gfx::PipelineDesc pd;
  pd.name = "remastered tarmacs";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;  // the far faces (as the decals)
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, &error);
  if (pipeline_) {
    const std::string cover = std::string(kViewShaderBlock) + kCoverShader;
    gfx::PipelineDesc cd;
    cd.name = "remastered tarmacs: the ground cover cleared";
    cd.vertexSource = cd.pixelSource = cover;
    cd.cull = gfx::Cull::None;
    cd.depthTest = cd.depthWrite = false;
    cd.depthTarget = false;
    cd.blend = gfx::Blend::Additive;
    cd.colorFormat = gfx::Format::R8;
    cd.samples = 1;
    cover_ = device_->CreatePipeline(cd, &error);
  }
  if (!pipeline_ || !cover_) {
    fprintf(stderr, "render: remastered tarmacs cannot be drawn: %s\n", error.c_str());
    return false;
  }
  made_ = 1;
  return true;
}

int Tarmacs::Draw(const View& view, const float* camera16, gfx::Texture sceneDepth, gfx::Texture waterMap, gfx::Texture waterRamp, gfx::Buffer cube,
                  const std::vector<Pad>& pads) {
  drawn = 0;
  if (pads.empty() || !Pipelines()) return 0;
  PadLook look{};
  look.parallax[0] = parallax ? 1.0f : 0.0f;
  look.parallax[1] = parallaxReach;
  look.parallax[2] = 1 / std::max(0.5f * parallaxReach, 1.0f);  // (as the ground's: Terrain::Draw)
  look.parallax[3] = parallaxShadows;
  std::copy(&field.level[0][0], &field.level[0][0] + 12, &look.tracksLevel[0][0]);
  std::copy(&field.atlas[0][0], &field.atlas[0][0] + 12, &look.tracksAtlas[0][0]);
  std::copy(field.info, field.info + 4, look.tracksInfo);
  if (!tracksMap) look.tracksInfo[2] = 0;
  // [padsnow] with the snow ground: its pipeline, the decals' snow constants after the camera's, the shelter at t6
  const bool snowNow = snowShelter && SnowPipeline();
  device_->SetPipeline(snowNow ? snowPipeline_ : pipeline_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  if (snowNow) {
    float camera28[28];
    std::copy(camera16, camera16 + 16, camera28);
    std::copy(snow, snow + 12, camera28 + 16);
    device_->SetConstants(1, camera28, sizeof camera28);
    device_->SetTexture(6, snowShelter);
  } else {
    device_->SetConstants(1, camera16, sizeof(float) * 16);
  }
  device_->SetConstants(3, &look, sizeof look);
  device_->SetTexture(1, sceneDepth);
  device_->SetTexture(3, waterMap ? waterMap : textures_->Black());
  device_->SetTexture(4, waterRamp ? waterRamp : textures_->Black());
  device_->SetTexture(26, tracksMap ? tracksMap : textures_->Black());
  device_->SetVertexBuffer(cube);
  // by material (the pads keep their order within one)
  for (size_t m = 0; m < materials_.size(); ++m) {
    batch_.clear();
    for (const Pad& p : pads) {
      if (p.material != static_cast<int>(m)) continue;
      batch_.insert(batch_.end(), {p.centre.x, p.centre.y, p.centre.z, std::max(std::fabs(p.halfX), std::fabs(p.halfZ)) * 1.05f + 1.5f, p.halfX, p.halfZ,
                                   p.cosHeading, p.sinHeading, p.alpha, p.snow, p.depth, 0});
    }
    if (batch_.empty() || !Load(materials_[m])) continue;
    const Material& mat = materials_[m];
    device_->SetTexture(0, mat.albedo);
    device_->SetTexture(2, mat.normal);
    device_->SetTexture(5, mat.arh);
    const size_t count = batch_.size() / 12;
    for (size_t at = 0; at < count; at += kMaxPads) {
      const size_t n = std::min(kMaxPads, count - at);
      device_->SetConstants(2, batch_.data() + at * 12, static_cast<uint32_t>(n * 12 * sizeof(float)));
      device_->Draw(36, 0, static_cast<uint32_t>(n));
      drawn += static_cast<int>(n);
    }
  }
  if (snowNow) device_->SetTexture(6, {});  // [padsnow]
  return drawn;  // (t26 left as the ground bound it)
}

gfx::Texture Tarmacs::ClearCover(const View& view, const float* area, const std::vector<Pad>& pads, gfx::Texture into, int intoSize) {
  if (pads.empty() || area[2] <= 0 || area[3] <= 0 || !Pipelines()) return {};
  const int size = into ? intoSize : kCoverSize;  // [plantlod] (the picture's texels a side)
  const float wide = area[2] * area[3];
  // the pads over the square, by material
  bool any = false;
  for (const Pad& p : pads) {
    const float reach = std::max(std::fabs(p.halfX), std::fabs(p.halfZ)) * 1.42f;
    if (p.centre.x + reach < area[0] || p.centre.z + reach < area[1] || p.centre.x - reach > area[0] + wide || p.centre.z - reach > area[1] + wide) continue;
    if (p.material >= 0 && p.material < static_cast<int>(materials_.size()) && Load(materials_[static_cast<size_t>(p.material)])) any = true;
  }
  if (!any) return {};
  if (!coverField_ && !into) {
    gfx::TargetDesc td;
    td.width = td.height = kCoverSize;
    td.format = gfx::Format::R8;
    coverField_ = device_->CreateTarget(td);
    if (!coverField_) return {};
  }
  gfx::PassDesc pass;
  pass.color = into ? into : coverField_;  // [plantlod]
  pass.clearColor = true;
  device_->BeginPass(pass);
  device_->SetPipeline(cover_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  std::vector<float> k;
  for (size_t m = 0; m < materials_.size(); ++m) {
    const Material& mat = materials_[m];
    if (mat.tier.empty()) continue;
    k.assign(area, area + 4);
    for (const Pad& p : pads) {
      if (p.material != static_cast<int>(m) || p.alpha <= 0.003f) continue;
      const float reach = std::max(std::fabs(p.halfX), std::fabs(p.halfZ)) * 1.42f;
      if (p.centre.x + reach < area[0] || p.centre.z + reach < area[1] || p.centre.x - reach > area[0] + wide || p.centre.z - reach > area[1] + wide) continue;
      // (read at the level whose texels are about the picture's: its texels across the pad over the square's across it)
      const float texels = mat.tier == "8k" ? 8192.0f : 4096.0f;
      const float level = std::max(0.0f, std::log2(texels * (wide / static_cast<float>(size)) / std::max(2 * std::fabs(p.halfX), 0.1f)));
      k.insert(k.end(), {p.centre.x, p.centre.z, p.halfX, p.halfZ, p.cosHeading, p.sinHeading, p.alpha, level});
      if ((k.size() - 4) / 8 >= kMaxCoverPads) break;
    }
    const size_t n = (k.size() - 4) / 8;
    if (!n) continue;
    device_->SetTexture(0, mat.albedo);
    device_->SetConstants(1, k.data(), static_cast<uint32_t>(k.size() * sizeof(float)));
    device_->Draw(static_cast<uint32_t>(n * 6));
  }
  device_->SetTexture(0, {});
  return into ? into : coverField_;
}

}  // namespace render
