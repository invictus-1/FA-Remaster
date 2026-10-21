#include "decals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/vfs.h"
#include "effect_files.h"
#include "lighting.h"
#include "ray_scene.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 1024;  // per draw call (the size of the shader's array)

// Each decal is drawn as a box (its far faces, so that it also works with the camera inside it).
// The pixel shader finds the ground's place under the pixel from the scene's depth, turns it into
// the decal's own frame, and gives up where that is outside the footprint.
const char kShaderHead[] = R"hlsl(
struct Instance {
  float4 centre;   // xyz; w: half the box's height
  float4 shape;    // x, y: half the footprint along its own x and z; z, w: cos and sin of its heading
  float4 options;  // x: how much of it is there (fading), y: a light's age (0..1 of its time; a plain mark: how much of the lying snow covers it),
                   // z: 1 = a light (texture x ramp, added), 2 a normal decal, 3 a mark, 4 a "Water Albedo" decal
                   // where the ground is above the water, 5 the same on the water where the ground is under it;
                   // w: the modern look's brightness of a light; a picture's 1 when it has its own relief (t5)
};
cbuffer DecalCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // x, y: 1 / the map's size; z: 1 = the map has water; w: the water's level
#ifdef DECAL_SNOW_GROUND
  float4 snowWind;     // [decalsnow] xy the way the wind blows (unit length); zw 1 / the map's size
  float4 snowTune;     // [decalsnow] Terrain::snowTune
  float4 snowMore;     // [decalsnow] x 1 = the snow's materials are drawn (the ground's wider, mottled margin)
#endif
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
Texture2D decalTexture : register(t0);
Texture2D rampTexture : register(t2);
Texture2D waterMap : register(t3);
Texture2D waterRamp : register(t4);
Texture2D reliefTexture : register(t5);  // a remastered picture's relief: x in alpha, y in green (BC5 read as the game's DXT5), y up the picture
#ifdef DECAL_SNOW_GROUND
Texture2D snowShelter : register(t6);  // [decalsnow] the snow ground's shelter (Terrain::SnowShelter): g sky, b crowns, a how it lies
#endif
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

struct VOut {
  float4 position : SV_Position;
  nointerpolation float4 centre : TEXCOORD0;
  nointerpolation float4 shape : TEXCOORD1;
  nointerpolation float4 options : TEXCOORD2;
};

VOut VS(float3 corner : POSITION, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
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

)hlsl";

}  // namespace

// ([padsnow] the decals' shader is the head above, this, and the tail below, in that order: the pads' takes this too)
const char kMarkSnowLying[] = R"hlsl(#if defined(MODERN_RELIEF) || defined(DECAL_SNOW_GROUND)
// How much snow lies on a mark's place (0..1), `fallen` the snow over it (the weather's, times the share on the mark),
// `ground` the ground's own slope there: as ModernLight lays it on the ground, level ground first, in patches. [decalsnow]
// With the snow ground (DECAL_SNOW_GROUND) as the ground's snow pipeline lays it: deeper in the lee and hollows, thinner
// and patchier under trees, on steep, windward and scoured ground (terrain.cpp's SnowAround, its snowLie and snowLess: the
// same numbers), and with the snow's materials over the ground's wider, mottled margin (lighting.h; the break-up by the
// layers' heights close by is the ground's alone).
float MarkSnowLying(float3 world, float3 ground, float fallen) {
  float patch = CloudNoise(world * 0.9) * 0.55 + CloudNoise(float3(world.xz * 0.21, 5.3)) * 0.45;
  float takes = patch * 0.75 + saturate((ground.y - 0.35) * 1.6) * 0.25;
#ifdef DECAL_SNOW_GROUND
  float4 shelter = snowShelter.SampleLevel(samplerLinearClamp, saturate(world.xz * snowWind.zw), 0);
  float crowns = shelter.b, lay = shelter.a * 2 - 1;
  float slope = length(ground.xz);
  float lee = slope > 1e-3 ? dot(ground.xz / slope, snowWind.xy) * saturate(slope * 5) : 0;  // (1: facing away from the wind)
  fallen *= (1 + 0.4 * lay * snowTune.w + 0.15 * lee) * (1 - 0.3 * crowns * snowTune.y);
  takes -= 0.08 * crowns * snowTune.y + 0.25 * saturate((0.85 - ground.y) * 4) + 0.1 * saturate(-lee) + 0.3 * saturate(-lay) * snowTune.w;
  float bar = 1.02 - fallen * 1.15;
  [branch] if (snowMore.x > 0.5) {
    float lying = smoothstep(bar - 0.15, bar + 0.35, takes) * smoothstep(0.25, 0.5, ground.y);
    float mottle = CloudNoise(float3(world.xz * 0.45, 1.3)) * 0.6 + CloudNoise(float3(world.xz * 1.3, 4.7)) * 0.4;
    return saturate(lying + (mottle - 0.5) * 3.2 * lying * (1 - lying));
  }
#else
  float bar = 1.02 - fallen * 1.15;
#endif
  return smoothstep(bar, bar + 0.2, takes) * smoothstep(0.25, 0.5, ground.y);
}
#endif
)hlsl";

namespace {

const char kShaderTail[] = R"hlsl(
#ifdef MODERN_RELIEF
// [scars] A normal decal with Decals::modernRelief (the modern look): the ground's own slope (from how its place changes
// from pixel to pixel) tilted by the decal's, and the ground multiplied by how much more or less of the modern light the
// tilted ground takes than the flat: the sun through its and the clouds' shadows, the sky's light from all sides and the
// map's fill (ModernLight's diffuse part). With the sun down the sky's share keeps the change near 1; flat parts change
// nothing. Under lying snow (options.y: -1 - the share of it that lies on the mark, as a picture's) the relief softens
// as the snow fills it. options.w: 0 the game's (how much applies in red), 1 the visuals track's effects' own (two
// channels, y up the picture), 2 a plain normal map (x in alpha, y in green; no share in red): both at full strength.
float4 NormalDecalModern(float4 c, float4 options, float4 shape, float3 world) {
  float3 ground = normalize(cross(ddy(world), ddx(world)));
  if (ground.y < 0) ground = -ground;
  float2 tilt = c.ag * 2 - 1;
  float3 inDecal = float3(tilt.x, sqrt(saturate(1 - dot(tilt, tilt))), tilt.y);
  float strength = c.r;
  if (options.w > 0.5) {
    if (options.w < 1.5) inDecal.z = -tilt.y;
    strength = 1;
  }
  float3 turned = float3(inDecal.x * shape.z + inDecal.z * shape.w, inDecal.y, -inDecal.x * shape.w + inDecal.z * shape.z);
  float3 tilted = normalize(ground + turned - float3(0, 1, 0));
  // (where the snow lies on it - thick, it fills the relief but for a hint of it)
  float snow = -1 - options.y;
  [branch] if (weather.y > 0 && snow > 0) {
    float fallen = weather.y * snow;
    tilted = normalize(lerp(tilted, ground, MarkSnowLying(world, ground, fallen) * saturate(fallen * 1.5 - 0.2) * 0.75));
  }
  float3 sun = ToLinear(sunColor) * (lightMultiplier * modern.x * SunShadow(world, ground) * CloudShadow(world));
  float3 fill = ToLinear(shadowFill) * 0.5;
  float3 skyFlat, skyTilted;
  [branch] if (skyZenith.w > 0.5) {  // the computed sky (sky.h)
    skyFlat = SkyAmbient(ground);
    skyTilted = SkyAmbient(tilted);
  } else {
    skyFlat = ToLinear(environmentSky.SampleLevel(samplerLinear, ground, 8).rgb);
    skyTilted = ToLinear(environmentSky.SampleLevel(samplerLinear, tilted, 8).rgb);
  }
  float3 before = sun * saturate(dot(sunDirection, ground)) + (skyFlat + fill) * modern.y;
  float3 after = sun * saturate(dot(sunDirection, tilted)) + (skyTilted + fill) * modern.y;
  float3 change = clamp(after / max(before, 1e-5), 0.35, 2.5);
  return float4(lerp(float3(1, 1, 1), change, saturate(strength * options.x)), 1);
}
#endif

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  if (depth <= 0) discard;  // nothing drawn here
  float3 world = WorldAt(i.position.xy, depth);
  [branch] if (i.options.z > 5.5) {
    // hot metal (hot_metal.h): a fresh scorch's or crater's middle glowing on the ground, its light added, through the
    // air as the ground's own (options: x its age, y how long it glows, w its own number)
    float3 d = world - i.centre.xyz;
    if (abs(d.y) > i.centre.w) discard;
    float2 local = float2(d.x * i.shape.z - d.z * i.shape.w, d.x * i.shape.w + d.z * i.shape.z) / i.shape.xy;
    if (any(abs(local) > 1)) discard;
    float4 c = decalTexture.Sample(samplerAniso, local * 0.5 + 0.5);
    float3 glow = HotGroundGlow(local, c.a, i.options.x, i.options.y, i.options.w) * (kHotMetalStrength * pow(falling.w > 0 ? falling.w : 1, 0.6));
    if (max(glow.r, glow.g) <= 0) discard;  // (nothing glows there: nothing written)
    return float4(ThroughAirTo(glow, world) - ThroughAirTo(0, world), 1);
  }
  // a "Water Albedo" decal: the original raises the ground under it to the water's surface
  // (TDecalsWaterAlbedo). Before the water: only where the ground is above it; after the water: only
  // where the ground is under it, on the surface the line of sight meets.
  if (i.options.z > 3.5) {
    bool under = mapInfo.z > 0.5 && world.y < mapInfo.w;
    if (under != (i.options.z > 4.5)) discard;
    if (under) {
      float3 ray = world - cameraPosition;
      world = cameraPosition + ray * ((mapInfo.w - cameraPosition.y) / min(ray.y, -1e-4));
    }
  }
  float3 d = world - i.centre.xyz;
  if (abs(d.y) > i.centre.w) discard;
  // into the decal's own frame (the heading turned back)
  float2 local = float2(d.x * i.shape.z - d.z * i.shape.w, d.x * i.shape.w + d.z * i.shape.z) / i.shape.xy;
  if (any(abs(local) > 1)) discard;
  float4 c = decalTexture.Sample(samplerAniso, local * 0.5 + 0.5);
  if (i.options.z > 3.5) {  // a "Water Albedo" decal: unlit, in its own colours (the original's times the light multiplier)
    c.a *= i.options.x;
    if (c.a <= 0.003) discard;
    c.rgb = look > 0.5 ? ToLinear(c.rgb) * (1.6 * (falling.w > 0 ? falling.w : 1)) : c.rgb * lightMultiplier;
    return c;
  }
  if (i.options.z > 2.5) {  // a mark: as it is (the modern look's picture is linear light, mapped back at the end)
    if (look > 0.5) c.rgb = ToLinear(c.rgb) * (1.6 * (falling.w > 0 ? falling.w : 1));  // (less to an eye opened for the night)
    return c;
  }
  if (i.options.z > 1.5) {
#ifdef MODERN_RELIEF
    return NormalDecalModern(c, i.options, i.shape, world);  // [scars] (Decals::modernRelief)
#else
    // a normal decal: the ground's own slope (from how its place changes from pixel to pixel)
    // tilted by the decal's, and the picture multiplied by how much more or less light that takes
    float3 ground = normalize(cross(ddy(world), ddx(world)));
    if (ground.y < 0) ground = -ground;
    float2 tilt = c.ag * 2 - 1;
    float3 inDecal = float3(tilt.x, sqrt(saturate(1 - dot(tilt, tilt))), tilt.y);
    float strength = c.r;
    if (i.options.w > 0.5) {  // the visuals track's own (two channels, x and y, y up the picture): at full strength
      inDecal.z = -tilt.y;
      strength = 1;
    }
    float3 turned = float3(inDecal.x * i.shape.z + inDecal.z * i.shape.w, inDecal.y, -inDecal.x * i.shape.w + inDecal.z * i.shape.z);
    float3 tilted = normalize(ground + turned - float3(0, 1, 0));
    float shade = SunShadow(world, ground);
    float3 before = OriginalLight(dot(sunDirection, ground), shade), after = OriginalLight(dot(sunDirection, tilted), shade);
    float3 change = clamp(after / max(before, 0.03), 0.35, 2.5);
    return float4(lerp(float3(1, 1, 1), change, saturate(strength * i.options.x)), 1);
#endif
  }
  if (i.options.z > 0.5) {  // a light: its glow times its ramp along its time, added
    if (look > 0.5) discard;  // the modern look has real lights for this (lights.h)
    c *= rampTexture.Sample(samplerLinearClamp, float2(i.options.y, 0.5));
    return c;
  }
  c.a *= i.options.x;
  if (c.a <= 0.003) discard;
  // lit as the ground it lies on: the ground's slope from how its place changes from pixel to pixel
  float3 N = normalize(cross(ddy(world), ddx(world)));
  if (N.y < 0) N = -N;
#ifdef DECAL_SNOW_GROUND
  const float3 groundSlope = N;  // [decalsnow]
#endif
  if (i.options.w > 0.5) {  // a remastered picture's own relief, tilting the ground's slope as a normal decal does
    float2 t = reliefTexture.Sample(samplerAniso, local * 0.5 + 0.5).ag * 2 - 1;
    float3 inDecal = float3(t.x, sqrt(saturate(1 - dot(t, t))), -t.y);  // (its y up the picture: toward the decal's -z)
    float3 turned = float3(inDecal.x * i.shape.z + inDecal.z * i.shape.w, inDecal.y, -inDecal.x * i.shape.w + inDecal.z * i.shape.z);
    N = normalize(N + turned - float3(0, 1, 0));
  }
  float shadow = SunShadow(world, N);
  // under water: tinted toward the water ramp's colour by depth, as the ground is (terrain.cpp)
  float4 tint = 0;
  if (mapInfo.z > 0.5) tint = waterRamp.Sample(samplerLinearClamp, float2(waterMap.Sample(samplerLinearClamp, world.xz * mapInfo.xy).g, 0.5));
  if (look > 0.5) {
#ifdef DECAL_SNOW_GROUND
    // [decalsnow] (Decals::snowGround) the snow that lies over the mark is the ground's own: the mark thins out by as much as
    // lies there, and is lit bare
    [branch] if (weather.y > 0 && i.options.y > 0) {
      c.a *= 1 - MarkSnowLying(world, groundSlope, weather.y * i.options.y);
      if (c.a <= 0.003) discard;
    }
    markSnow = 0;
#else
    markSnow = i.options.y;  // (a mark laid on the snow shows on it until new snow covers it)
#endif
    float3 lit = ModernLight(ToLinear(c.rgb), 0, 0.9, 1, 0, N, world, shadow);
#ifdef MSAA
    return float4(lit, c.a);  // (the modern water colours what is under it itself)
#endif
    float full = max(lightMultiplier * modern.x * (skyZenith.w > 0.5 ? skyGlow.w * lerp(1, 0.45, skyHorizon.w) : 1), 0.001);
    float3 screen = lerp(pow(max(lit / full, 0), 1 / 2.2), tint.rgb, tint.a);
    return float4(ToLinear(screen) * full, c.a);
  }
  return float4(lerp(c.rgb * OriginalLight(dot(sunDirection, N), shadow), tint.rgb, tint.a), c.a);
}
)hlsl";

// The decals' shader source ([scars] `relief`: with the normal decals lit as Decals::modernRelief says; [decalsnow] `snow`:
// with the snow ground's, Decals::snowGround).
std::string DecalSource(bool relief, bool snow = false) {
  // (how much of the lying snow a mark takes is the mark's own: see `snowLying`)
  return std::string(snow ? "#define DECAL_SNOW_GROUND 1\n" : "") + (relief ? "#define MODERN_RELIEF 1\n" : "") + (gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "static float markSnow = 1;\n#define SURFACE_SNOW markSnow\n" +
         kViewShaderBlock + RayShaders() + kLightingShaderBlock + HotMetal::ShaderBlock() + kShaderHead + kMarkSnowLying + kShaderTail;  // (RayShaders: the small lights' shadows by rays; hot metal: hot_metal.h)
}

// [scars] Whether a DDS picture's blue channel holds anything: the game's normal decals keep it empty (x in alpha, y in
// green, how much applies in red); a plain normal map does not. Read from its first level's colour blocks (DXT1, DXT3,
// DXT5: the two end colours of each, 5:6:5) or its pixels (32 bits, with masks); anything else: no.
bool BlueInDds(const std::string& file) {
  if (file.size() < 128 || file.compare(0, 4, "DDS ") != 0) return false;
  auto u32 = [&](size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(file[at])) | static_cast<uint32_t>(static_cast<unsigned char>(file[at + 1])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(file[at + 2])) << 16 | static_cast<uint32_t>(static_cast<unsigned char>(file[at + 3])) << 24;
  };
  const uint32_t height = u32(12), width = u32(16), flags = u32(80), fourCC = u32(84), bits = u32(88), blueMask = u32(100);
  const size_t blocks = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4);
  int stride = 0;
  if ((flags & 4) && fourCC == 0x31545844u) stride = 8;                                               // DXT1
  else if ((flags & 4) && (fourCC == 0x33545844u || fourCC == 0x35545844u)) stride = 16;            // DXT3, DXT5
  if (stride) {
    for (size_t b = 0; b < blocks && 128 + (b + 1) * stride <= file.size(); ++b) {
      const size_t at = 128 + b * stride + (stride - 8);
      const uint32_t ends = u32(at);
      if ((ends & 0x1f) > 1 || ((ends >> 16) & 0x1f) > 1) return true;  // (blue of either end colour, past rounding)
    }
    return false;
  }
  if ((flags & 0x40) && bits == 32 && blueMask) {
    int shift = 0;
    while (!((blueMask >> shift) & 1)) ++shift;
    for (size_t p = 0; p < static_cast<size_t>(width) * height && 128 + (p + 1) * 4 <= file.size(); ++p)
      if (((u32(128 + p * 4) & blueMask) >> shift) > 2) return true;
  }
  return false;
}

}  // namespace

void Decals::SetModernRelief(bool on) {
  modernRelief = on;
  if (!on || reliefTried_) return;
  reliefTried_ = true;
  std::string error;
  const std::string source = DecalSource(true);  // (kept while the pipeline is made: the description points into it)
  gfx::PipelineDesc pd;
  pd.name = "normal decals, modern relief";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Multiply;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  normalsModern_ = device_->CreatePipeline(pd, &error);
  if (!normalsModern_) fprintf(stderr, "render: the marks' relief cannot be lit as the ground (%s)\n", error.c_str());
}

void Decals::PrepareSnowGround() {
  // (the pictures' pipeline and the relief's, each with the snow ground's: DECAL_SNOW_GROUND)
  auto make = [&](bool relief, gfx::Pipeline* out, const char* name) {
    std::string error;
    const std::string source = DecalSource(relief, true);  // (kept while the pipeline is made: the description points into it)
    gfx::PipelineDesc pd;
    pd.name = name;
    pd.vertexSource = pd.pixelSource = source;
    pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
    pd.stride = 12;
    pd.cull = gfx::Cull::Front;
    pd.depthTest = pd.depthWrite = false;
    pd.depthTarget = false;
    pd.blend = relief ? gfx::Blend::Multiply : gfx::Blend::Alpha;
    pd.colorFormat = kSceneFormat;
    pd.samples = gSceneSamples;
    pd.alphaWrite = false;
    *out = device_->CreatePipeline(pd, &error);
    if (!*out) fprintf(stderr, "render: the marks cannot lay the snow ground's snow (%s)\n", error.c_str());
  };
  if (!snowTried_) {
    snowTried_ = true;
    make(false, &picturesSnow_, "decals, snow ground");
  }
  if (modernRelief && !reliefSnowTried_) {
    reliefSnowTried_ = true;
    make(true, &normalsModernSnow_, "normal decals, modern relief, snow ground");
  }
}

bool Decals::Plain(uint32_t texture) {
  auto known = plainOf_.find(texture);
  if (known != plainOf_.end()) return known->second;
  bool plain = false;
  auto file = normalFiles_.find(texture);
  if (file != normalFiles_.end())
    if (auto bytes = vfs_->ReadFile(file->second)) plain = BlueInDds(*bytes);
  plainOf_[texture] = plain;
  return plain;
}

bool Decals::Init(std::string* error) {
  const std::string source = DecalSource(false);
  gfx::PipelineDesc pd;
  pd.name = "decals";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;  // the far faces
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "lights on the ground";
  pd.blend = gfx::Blend::Additive;
  pd.alphaWrite = true;
  lights_ = device_->CreatePipeline(pd, error);
  if (!lights_) return false;
  pd.name = "normal decals";
  pd.blend = gfx::Blend::Multiply;
  pd.alphaWrite = false;
  normals_ = device_->CreatePipeline(pd, error);
  if (!normals_) return false;
  pd.name = "hot marks on the ground";  // (hot metal: a fresh scorch's glow, added; the picture's alpha left alone)
  pd.blend = gfx::Blend::Additive;
  hot_ = device_->CreatePipeline(pd, error);
  if (!hot_) return false;
  // a cube from -1 to 1, its faces wound counter-clockwise seen from outside
  std::vector<float> v;
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int a = 0; a < 3; ++a)
    for (int sign = -1; sign <= 1; sign += 2) {
      int iu = (a + 1) % 3, iw = (a + 2) % 3;
      if (sign < 0) std::swap(iu, iw);
      const Vec3 n = axes[a] * static_cast<float>(sign), u = axes[iu], w = axes[iw];
      const Vec3 p0 = n - u - w, p1 = n + u - w, p2 = n + u + w, p3 = n - u + w;
      for (Vec3 p : {p0, p1, p2, p0, p2, p3}) {
        v.push_back(p.x);
        v.push_back(p.y);
        v.push_back(p.z);
      }
    }
  gfx::BufferDesc bd;
  bd.stride = 12;
  bd.size = static_cast<uint32_t>(v.size() * sizeof(float));
  bd.data = v.data();
  cube_ = device_->CreateBuffer(bd);
  batch_.resize(kMaxInstances);
  return static_cast<bool>(cube_);
}

gfx::Texture Decals::Resolve(const std::string& texture, gfx::Texture* relief, bool* ours, std::string* found) {
  if (relief) *relief = {};
  if (ours) *ours = false;
  if (texture.empty()) return {};
  // (a path begins with a slash; a script's short name may have a folder in it: "Tarmacs/Tar12x_01_albedo")
  std::string path;
  if (texture[0] == '/') {
    if (vfs_->Exists(texture)) path = texture;
  } else {
    for (const char* folder : {"/env/common/decals/", "/env/common/splats/"})
      if (path.empty() && vfs_->Exists(std::string(folder) + texture + ".dds")) path = std::string(folder) + texture + ".dds";
  }
  if (path.empty()) return {};
  if (found) *found = path;  // [scars] (the game's file, before the visuals track's takes its place)
  // the visuals track's picture of it, and the relief beside it ("x_albedo.dds", "x_normals.dds") where the game has
  // no normal decal of that name (where it has, its scripts lay that one as a decal of its own: the crater's
  // "crater01_normals" with "crater01_albedo"; it then reads the visuals track's, and the relief is not given twice)
  if (remastered) {
    const std::string mine = EffectFile(*vfs_, path, true);
    if (mine != path) {
      if (ours) *ours = true;
      std::string low = mine;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const std::string tail = "_albedo.dds";
      if (relief && low.size() > tail.size() && low.compare(low.size() - tail.size(), tail.size(), tail) == 0) {
        const std::string normals = mine.substr(0, mine.size() - tail.size()) + "_normals.dds";
        const std::string game = path.substr(0, path.size() - tail.size()) + "_normals.dds";
        if (vfs_->Exists(normals) && !vfs_->Exists(game)) *relief = textures_->Get(normals);
      }
      path = mine;
    }
  }
  return textures_->Get(path);
}

bool Decals::IsDepositMark(const std::string& texture, bool* hydrocarbon) {
  std::string name = texture;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const bool mass = name.find("mass_marker") != std::string::npos, hydro = name.find("hydrocarbon_marker") != std::string::npos;
  if (hydrocarbon) *hydrocarbon = hydro;
  return mass || hydro;
}

bool Decals::IsScar(const std::string& texture) {
  std::string name = texture;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name.find("scorch") != std::string::npos || name.find("crater") != std::string::npos;
}

bool Decals::Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  {  // a resource deposit's mark: remembered, for its icon from far up
    bool hydrocarbon = false;
    if (IsDepositMark(texture, &hydrocarbon) && seconds <= 0 && deposits_.size() < 4096) deposits_.push_back({centre, hydrocarbon, cutoff});
  }
  gfx::Texture relief;
  bool ours = false;
  const gfx::Texture t = Resolve(texture, &relief, &ours, &lastFile_);
  if (!t || sizeX == 0 || sizeZ == 0) return false;  // (a negative size mirrors it)
  if (groundHeight) {  // (given far from the ground, beyond what its box reaches: on the ground)
    const float ground = groundHeight(centre.x, centre.z);
    if (std::fabs(centre.y - ground) > std::max(std::fabs(sizeX), std::fabs(sizeZ)) * 0.5f) centre.y = ground;
  }
  decals_.push_back({t, {}, centre, sizeX * 0.5f, sizeZ * 0.5f, heading, cutoff, born, seconds});
  decals_.back().relief = relief;
  decals_.back().ours = ours;
  if (battleScars && seconds > 0 && IsScar(texture)) {  // a battle scar: kept; past the most, the oldest fades out
    decals_.back().scar = true;
    if (++scars_ > mostScars) {
      Decal* oldest = nullptr;
      for (Decal& d : decals_)
        if (d.scar && d.scarGone <= 0 && (!oldest || d.born < oldest->born)) oldest = &d;
      if (oldest) oldest->scarGone = born + 10;
    }
  }
  if (hotMetal && IsScar(texture)) decals_.back().burn = true;  // (hot metal: its middle glows while fresh)
  decals_.back().snowAtBirth = settled_ ? snowFallen_ : snowFallen_ - 1;
  decals_.back().id = lastId_ = nextId_++;
  decals_.back().tarmac = TarmacOf(texture);  // [tarmac] (a pad's: its material looked up when remastered pads are drawn)
  decals_.back().original = addingOriginal;
  decals_.back().mapOwn = !settled_;
  return true;
}

bool Decals::AddWater(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  if (!Add(texture, centre, sizeX, sizeZ, heading, cutoff, born, seconds)) return false;
  Decal& d = decals_.back();
  d.water = true;
  d.centre.y = std::max(d.centre.y, waterLevel);  // (its box reaches the surface)
  ++waterDecals_;
  return true;
}

void Decals::Remove(uint32_t id) {
  for (auto it = decals_.begin(); it != decals_.end(); ++it)
    if (it->id == id) {
      if (it->water) --waterDecals_;
      decals_.erase(it);
      return;
    }
}

// [decalfade]
void Decals::SetNearCutoff(uint32_t id, float nearCutoff) {
  for (auto it = decals_.rbegin(); it != decals_.rend(); ++it)
    if (it->id == id) {
      it->nearCutoff = std::max(nearCutoff, 0.0f);
      return;
    }
}

// [decalfade] (decals.h: fadeFraction, Lod)
float Decals::Measure(Vec3 p, Vec3 eye) const {
  if (fadeFraction < 0) return Length(p - eye) * lod.scale;  // (as before: the distance from the eye)
  // as the original's: the world the view spans across at that depth, times the camera's LOD scale
  return lod.scale * (lod.widthPerDepth * Dot(p - lod.eye, lod.forward));
}

float Decals::LodAlpha(const Decal& d, Vec3 eye) const {
  if (fadeFraction < 0) {  // as before: by its distance from the eye, over the last fifth of its cutoff (a GUESS)
    if (d.cutoff <= 0) return 1;
    const float distance = Measure(d.centre, eye);
    if (distance > d.cutoff) return -1;
    return Clamp((d.cutoff - distance) / (d.cutoff * 0.2f), 0, 1);
  }
  // as the original's GetLODAlpha (0x89d400), whose caller (Render_FlatDecals 0x8003e0) measures at the decal's middle
  // on the ground
  const float f = Clamp(fadeFraction, 0, 1);
  const float m = Measure(d.centre, eye);
  if (d.nearCutoff > 0) {
    // comes in from nothing at f x its near cutoff to whole at it; and is left out beyond its cutoff (the original's
    // spatial query does that by the cutoff, CWldTerrainDecal::Update 0x89df2c and SpatialDB::CollectInView 0x5041e0,
    // testing a whole cell's nearest corner: here the decal's middle, INFERRED)
    if (d.cutoff > 0 && m > d.cutoff) return -1;
    const float start = d.nearCutoff * f;
    if (!(d.nearCutoff > start)) return m >= d.nearCutoff ? 1.0f : 0.0f;  // (f 1: no fade at all)
    return (Clamp(m, start, d.nearCutoff) - start) / (d.nearCutoff - start);
  }
  if (d.cutoff <= 0) return 1;  // (no cutoff: always, as before; the original's formula gives no number there)
  if (m >= d.cutoff) return -1;
  const float start = d.cutoff * f;
  if (!(d.cutoff > start)) return 1;  // (f 1: whole up to its cutoff)
  return 1 - (Clamp(m, start, d.cutoff) - start) / (d.cutoff - start);
}

bool Decals::AddNormal(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  if (!Add(texture, centre, sizeX, sizeZ, heading, cutoff, born, seconds)) return false;
  decals_.back().normal = true;
  // [scars] (its file, for modernRelief to read once whether it is a plain normal map; not the visuals track's own)
  if (!decals_.back().ours) normalFiles_.emplace(decals_.back().texture.id, lastFile_);
  return true;
}

void Decals::AddMark(gfx::Texture texture, Vec3 centre, float sizeX, float sizeZ, float heading) {
  Decal d{texture, {}, centre, sizeX * 0.5f, sizeZ * 0.5f, heading, 0, 0, 0};
  d.mark = true;
  marks_.push_back(d);
}

bool Decals::AddLight(const std::string& texture, const std::string& ramp, Vec3 centre, float size, double born, float seconds) {
  if (size <= 0 || seconds <= 0 || !vfs_->Exists(texture)) return false;
  const gfx::Texture t = textures_->Get(texture);
  const gfx::Texture r = vfs_->Exists(ramp) ? textures_->Get(ramp, textures_->White()) : textures_->White();
  if (!t) return false;
  decals_.push_back({t, r, centre, size, size, 0, 0, born, seconds});
  return true;
}

// [tarmac] A pad's picture's name among those seen (Tarmacs::Key), or -1: not a pad's.
int Decals::TarmacOf(const std::string& texture) {
  const std::string key = Tarmacs::Key(texture);
  if (key.empty()) return -1;
  for (size_t i = 0; i < tarmacKeys_.size(); ++i)
    if (tarmacKeys_[i] == key) return static_cast<int>(i);
  tarmacKeys_.push_back(key);
  tarmacMaterial_.push_back(-2);
  return static_cast<int>(tarmacKeys_.size() - 1);
}

// [tarmac] The material a decal of a pad is drawn in with remastered tarmacs on (-1: the game's pictures): none for one the
// original effects laid, nor for a mark, a light or a "Water Albedo" decal.
int Decals::TarmacMaterial(const Decal& d) {
  if (d.tarmac < 0 || d.original || d.mark || d.water || d.ramp) return -1;
  int& m = tarmacMaterial_[static_cast<size_t>(d.tarmac)];
  if (m == -2) m = tarmacs.MaterialOf(tarmacKeys_[static_cast<size_t>(d.tarmac)]);
  return m >= 0 && tarmacs.Usable(m) ? m : -1;
}

// [tarmac] A pad as Tarmacs draws it: over its material's extent, faded as the game's picture would be (its last seconds;
// with `view`: also as its distance fades it, LodAlpha, and not where the view does not see it).
bool Decals::TarmacPad(const Decal& d, double now, const View* view, Tarmacs::Pad* pad) {
  const int material = TarmacMaterial(d);
  if (material < 0 || d.normal || now < d.born) return false;
  const float extent = tarmacs.Extent(material);
  const float halfX = d.halfX * extent, halfZ = d.halfZ * extent;
  float alpha = d.seconds > 0 ? Clamp(static_cast<float>(d.born + static_cast<double>(d.seconds) - now) / std::min(5.0f, d.seconds * 0.5f), 0, 1) : 1.0f;
  if (view) {
    const float reach = std::max(std::fabs(halfX), std::fabs(halfZ)) * 1.5f;
    const Vec3 r{reach, reach * 0.7f + 1.5f, reach};
    const float lodAlpha = LodAlpha(d, view->eye);  // [decalfade]
    if (lodAlpha < 0 || !view->BoxVisible(d.centre - r, d.centre + r)) return false;
    alpha *= lodAlpha;
  }
  if (alpha <= 0 || (fadeFraction >= 0 && alpha < 1.0f / 255)) return false;  // ([decalfade] as the original leaves it out)
  pad->centre = d.centre;
  pad->halfX = halfX;
  pad->halfZ = halfZ;
  pad->cosHeading = std::cos(d.heading);
  pad->sinHeading = std::sin(d.heading);
  pad->alpha = alpha;
  pad->snow = snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1;  // (as a picture's mark: Draw)
  pad->depth = tarmacs.Depth(material, 2 * d.halfX);
  pad->material = material;
  return true;
}

void Decals::TarmacPads(double now, std::vector<Tarmacs::Pad>* out) {
  out->clear();
  if (!remasteredTarmacs) return;
  Tarmacs::Pad pad;
  for (const Decal& d : decals_)
    if (d.tarmac >= 0 && TarmacPad(d, now, nullptr, &pad)) out->push_back(pad);
}

void Decals::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now, bool overWater) {
  if (!overWater) {
    drawn = 0;
    tarmacsDrawn = 0;  // [tarmac]
    // the snow that has fallen since the last frame (a mark is covered by one unit of it)
    if (snowClock_ >= 0 && now > snowClock_) snowFallen_ += snowing * static_cast<float>(std::min(now - snowClock_, 0.5)) / 90.0f;
    snowClock_ = now;
    settled_ = true;
    // those whose time is over go
    // (a battle scar only once it has faded out, as the most there may be asks; with battle scars off, at its time)
    auto over = [&](const Decal& d) {
      if (d.scar && battleScars) return d.scarGone > 0 && now >= d.scarGone;
      return d.seconds > 0 && now - d.born >= static_cast<double>(d.seconds);
    };
    decals_.erase(std::remove_if(decals_.begin(), decals_.end(), over), decals_.end());
    scars_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.scar; }));
    waterDecals_ = static_cast<int>(std::count_if(decals_.begin(), decals_.end(), [](const Decal& d) { return d.water; }));
  }
  visible_.clear();
  pads_.clear();  // [tarmac]
  const bool pads = remasteredTarmacs && !overWater;
  const bool modernNormals = modernRelief && normalsModern_;  // [scars]
  if (!overWater) plainNormals = 0;
  for (size_t which = 0; which < decals_.size() + marks_.size(); ++which) {
    const Decal& d = which < decals_.size() ? decals_[which] : marks_[which - decals_.size()];
    if (overWater && !d.water) continue;
    // [tarmac] a pad the visuals track has a material for: drawn in it (Tarmacs), its "_normals" twin not at all
    if (pads && d.tarmac >= 0 && TarmacMaterial(d) >= 0) {
      tarmacs.Want(TarmacMaterial(d));  // [hitches] (its material read ahead wherever the pad lies: Tarmacs::Want)
      Tarmacs::Pad pad;
      if (TarmacPad(d, now, &view, &pad)) pads_.push_back(pad);
      continue;
    }
    const float reach = std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) * 1.5f;
    // the ground under a decal may rise and fall across it: the box is tall for its width
    const float halfHeight = reach * 0.7f + 1.5f;
    const Vec3 r{reach, halfHeight, reach};
    const float lodAlpha = LodAlpha(d, view.eye);  // [decalfade] (-1: beyond its cutoff)
    if (lodAlpha < 0) continue;
    if (!view.BoxVisible(d.centre - r, d.centre + r)) continue;
    if (now < d.born) continue;  // (a light whose beat the picture has not reached)
    float alpha = 1;
    // (it fades out over its last five seconds, a short-lived one over the second half of its time: a
    // tank's tread marks last a second, a walker's footprints four; how the original fades them is a GUESS)
    if (d.scar && battleScars) {  // a battle scar: weathered after the game's time, faded out once the most is passed
      const float past = static_cast<float>(now - d.born) - d.seconds;
      alpha = 1 - (1 - scarFloor) * Clamp(past / std::max(scarWeathering, 1.0f), 0, 1);
      if (d.scarGone > 0) alpha *= Clamp(static_cast<float>(d.scarGone - now) / 10, 0, 1);
    } else if (d.seconds > 0) {
      alpha = Clamp(static_cast<float>(d.born + static_cast<double>(d.seconds) - now) / std::min(5.0f, d.seconds * 0.5f), 0, 1);
    }
    // it comes in as its distance says (LodAlpha: as before over the last fifth of it, a GUESS; or as the original's
    // GetLODAlpha, fadeFraction), and with the original's fade it is left out where the original leaves it out
    alpha *= lodAlpha;
    if (fadeFraction >= 0 && alpha < 1.0f / 255) continue;
    Instance n{};
    n.centre[0] = d.centre.x;
    n.centre[1] = d.centre.y;
    n.centre[2] = d.centre.z;
    n.halfHeight = halfHeight;
    n.halfX = d.halfX;
    n.halfZ = d.halfZ;
    n.cosHeading = std::cos(d.heading);
    n.sinHeading = std::sin(d.heading);
    n.alpha = alpha;
    n.age = d.seconds > 0 ? static_cast<float>((now - d.born) / static_cast<double>(d.seconds)) : 0;
    n.light = d.water ? (overWater ? 5.0f : 4.0f) : d.mark ? 3.0f : d.ramp ? 1.0f : d.normal ? 2.0f : 0.0f;
    // (a plain mark has no use for the age: there it says how much of the lying snow covers it)
    if (!d.mark && !d.ramp && !d.normal) n.age = snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1;
    // (a light's brightness; a picture's 1 with its own relief; a normal decal's 1 when it is the visuals track's)
    n.glow = d.ramp ? glow : d.normal ? (d.ours ? 1.0f : 0.0f) : d.relief && !d.mark && !d.water ? 1.0f : 0.0f;
    // [scars] (modernRelief: a normal decal's age says how much of the lying snow covers it, as a picture's, from -1 down;
    // a plain normal map's glow 2)
    if (modernNormals && d.normal && !d.water && !d.mark && !d.ramp) {
      n.age = -1 - (snowLying > 0 ? Clamp(snowFallen_ - d.snowAtBirth, 0, 1) : 1);
      if (!d.ours && Plain(d.texture.id)) {
        n.glow = 2;
        ++plainNormals;
      }
    }
    visible_.push_back({d.texture.id, d.ramp.id, d.relief.id, d.water ? 4 : d.mark ? 3 : d.ramp ? 2 : d.normal ? 1 : 0, n, d.mapOwn});
    // hot metal: a fresh scorch's or crater's middle glows over it (not under water, where it would only steam)
    if (d.burn && hotMetal && heat && !overWater && !d.normal && !d.water && !d.mark && !d.ramp && d.centre.y > waterLevel) {
      const float across = 2 * std::max(std::fabs(d.halfX), std::fabs(d.halfZ));
      const float age = static_cast<float>(now - d.born), seconds = heat->CraterSeconds(across);
      if (age < seconds) {
        Instance h = n;
        h.alpha = age;
        h.age = seconds;
        h.light = 6;
        h.glow = static_cast<float>(d.id % 97);
        visible_.push_back({d.texture.id, 0, 0, 5, h});
      }
    }
  }
  if (!overWater) marks_.clear();
  if (visible_.empty() && pads_.empty()) return;
  // pictures first, then lights; by texture; those of one texture keep the order they were made in
  // (hot metal's glows, kind 5, after the lights and before the marks)
  // ([tarmac] with remastered pads drawn: the map's own pictures, the pads, then the pictures laid since: rank 0.5)
  const bool padsNow = !pads_.empty();
  auto rank = [padsNow](const Visible& v) { return v.kind == 5 ? 2.5f : padsNow && v.kind == 0 && !v.mapOwn ? 0.5f : static_cast<float>(v.kind); };
  std::stable_sort(visible_.begin(), visible_.end(), [&rank](const Visible& a, const Visible& b) {
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (a.texture != b.texture) return a.texture < b.texture;
    return a.ramp != b.ramp ? a.ramp < b.ramp : a.relief < b.relief;
  });

  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const bool water = waterMap_ && waterRamp_ && mapWidth_ > 0 && mapHeight_ > 0;
  const float cameraConstants[16] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                                     up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                                     back.x, back.y, back.z, camera.zNear,
                                     water ? 1 / mapWidth_ : 0, water ? 1 / mapHeight_ : 0, water ? 1.0f : 0.0f, water ? waterLevel : 0.0f};
  // [decalsnow] while the ground lays its snow from its shelter (snowGround): the pictures', and with modernRelief the relief's,
  // pipelines with the snow ground's, their constants after the camera's (the wind's way and the map's size, the tuning, the
  // materials), the shelter at t6. (Not on the water's drawing: its decals take no snow.)
  const bool snowNow = snowGround.shelter && !overWater && snowGround.mapWidth > 0 && snowGround.mapHeight > 0;
  const bool picturesSnow = snowNow && picturesSnow_, normalsSnow = snowNow && modernNormals && normalsModernSnow_;
  float snowConstants[28] = {};
  if (snowNow) {
    std::copy(cameraConstants, cameraConstants + 16, snowConstants);
    const float wind = std::sqrt(snowGround.wind[0] * snowGround.wind[0] + snowGround.wind[1] * snowGround.wind[1]);
    snowConstants[16] = wind > 0 ? snowGround.wind[0] / wind : 0.0f;
    snowConstants[17] = wind > 0 ? snowGround.wind[1] / wind : 0.0f;
    snowConstants[18] = 1 / snowGround.mapWidth;
    snowConstants[19] = 1 / snowGround.mapHeight;
    std::copy(snowGround.tune, snowGround.tune + 4, snowConstants + 20);
    snowConstants[24] = snowGround.materials ? 1.0f : 0.0f;
  }
  // ([padsnow] the pads lay it as the marks do: Tarmacs::snowShelter)
  tarmacs.snowShelter = snowNow ? snowGround.shelter : gfx::Texture{};
  if (snowNow) std::copy(snowConstants + 16, snowConstants + 28, tarmacs.snow);
  int bound = -1;
  bool padsDone = !padsNow;  // [tarmac] (the pads drawn after the map's own pictures, before what was laid since)
  auto drawPads = [&]() {
    tarmacsDrawn = tarmacs.Draw(view, cameraConstants, sceneDepth, water ? waterMap_ : gfx::Texture{}, water ? waterRamp_ : gfx::Texture{}, cube_, pads_);
    padsDone = true;
    bound = -1;  // (the decals' own pipeline and textures bound again)
  };
  for (size_t at = 0; at < visible_.size();) {
    if (!padsDone && rank(visible_[at]) >= 0.5f) drawPads();
    const uint32_t texture = visible_[at].texture, ramp = visible_[at].ramp, relief = visible_[at].relief;
    const int kind = visible_[at].kind;
    size_t n = 0;
    while (at + n < visible_.size() && visible_[at + n].texture == texture && visible_[at + n].ramp == ramp && visible_[at + n].relief == relief &&
           visible_[at + n].kind == kind && n < kMaxInstances) {
      batch_[n] = visible_[at + n].instance;
      ++n;
    }
    if (bound != kind) {
      bound = kind;
      // (marks blend as pictures do; [scars] relief; [decalsnow] the snow ground's)
      const bool snowBound = kind == 1 ? normalsSnow : kind != 2 && kind != 5 && picturesSnow;
      device_->SetPipeline(kind == 5 ? hot_ : kind == 2 ? lights_ : kind == 1 ? (modernNormals ? (normalsSnow ? normalsModernSnow_ : normalsModern_) : normals_)
                                                                             : picturesSnow ? picturesSnow_ : pipeline_);
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      if (snowBound) device_->SetConstants(1, snowConstants, sizeof snowConstants);
      else device_->SetConstants(1, cameraConstants, sizeof cameraConstants);
      if (snowBound) device_->SetTexture(6, snowGround.shelter);
      device_->SetTexture(1, sceneDepth);
      device_->SetTexture(3, water ? waterMap_ : textures_->Black());
      device_->SetTexture(4, water ? waterRamp_ : textures_->Black());
      device_->SetVertexBuffer(cube_);
    }
    device_->SetTexture(0, gfx::Texture{texture});
    device_->SetTexture(2, ramp ? gfx::Texture{ramp} : textures_->White());
    device_->SetTexture(5, relief ? gfx::Texture{relief} : textures_->FlatNormal());
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->Draw(36, 0, static_cast<uint32_t>(n));
    drawn += static_cast<int>(n);
    at += n;
  }
  if (!padsDone) drawPads();
  if (snowNow) device_->SetTexture(6, {});  // [decalsnow]
}

}  // namespace render
