#include "decals.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/vfs.h"
#include "effect_files.h"
#include "lighting.h"

namespace render {
namespace {

constexpr size_t kMaxInstances = 1024;  // per draw call (the size of the shader's array)

// Each decal is drawn as a box (its far faces, so that it also works with the camera inside it).
// The pixel shader finds the ground's place under the pixel from the scene's depth, turns it into
// the decal's own frame, and gives up where that is outside the footprint.
const char kShader[] = R"hlsl(
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
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
Texture2D decalTexture : register(t0);
Texture2D rampTexture : register(t2);
Texture2D waterMap : register(t3);
Texture2D waterRamp : register(t4);
Texture2D reliefTexture : register(t5);  // a remastered picture's relief: x in alpha, y in green (BC5 read as the game's DXT5), y up the picture
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

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  if (depth <= 0) discard;  // nothing drawn here
  float3 world = WorldAt(i.position.xy, depth);
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
    markSnow = i.options.y;  // (a mark laid on the snow shows on it until new snow covers it)
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

}  // namespace

bool Decals::Init(std::string* error) {
  // (how much of the lying snow a mark takes is the mark's own: see `snowLying`)
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + "static float markSnow = 1;\n#define SURFACE_SNOW markSnow\n" + kViewShaderBlock +
                       kLightingShaderBlock + kShader;
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

gfx::Texture Decals::Resolve(const std::string& texture, gfx::Texture* relief, bool* ours) {
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
  const gfx::Texture t = Resolve(texture, &relief, &ours);
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
  decals_.back().snowAtBirth = settled_ ? snowFallen_ : snowFallen_ - 1;
  decals_.back().id = lastId_ = nextId_++;
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

bool Decals::AddNormal(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  if (!Add(texture, centre, sizeX, sizeZ, heading, cutoff, born, seconds)) return false;
  decals_.back().normal = true;
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

void Decals::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now, bool overWater) {
  if (!overWater) {
    drawn = 0;
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
  for (size_t which = 0; which < decals_.size() + marks_.size(); ++which) {
    const Decal& d = which < decals_.size() ? decals_[which] : marks_[which - decals_.size()];
    if (overWater && !d.water) continue;
    const float reach = std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) * 1.5f;
    // the ground under a decal may rise and fall across it: the box is tall for its width
    const float halfHeight = reach * 0.7f + 1.5f;
    const Vec3 r{reach, halfHeight, reach};
    const float distance = Length(d.centre - view.eye);
    if (d.cutoff > 0 && distance > d.cutoff) continue;
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
    // it comes in over the last fifth of its distance (the original's DecalAlpha; how wide its fade is, is a GUESS)
    if (d.cutoff > 0) alpha *= Clamp((d.cutoff - distance) / (d.cutoff * 0.2f), 0, 1);
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
    visible_.push_back({d.texture.id, d.ramp.id, d.relief.id, d.water ? 4 : d.mark ? 3 : d.ramp ? 2 : d.normal ? 1 : 0, n});
  }
  if (!overWater) marks_.clear();
  if (visible_.empty()) return;
  // pictures first, then lights; by texture; those of one texture keep the order they were made in
  std::stable_sort(visible_.begin(), visible_.end(), [](const Visible& a, const Visible& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
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
  int bound = -1;
  for (size_t at = 0; at < visible_.size();) {
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
      device_->SetPipeline(kind == 2 ? lights_ : kind == 1 ? normals_ : pipeline_);  // (marks blend as pictures do)
      device_->SetConstants(0, &view.constants, sizeof view.constants);
      device_->SetConstants(1, cameraConstants, sizeof cameraConstants);
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
}

}  // namespace render
