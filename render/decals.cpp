#include "decals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/vfs.h"
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
  float4 options;  // x: how much of it is there (fading), y: a light's age (0..1 of its time),
                   // z: 1 = a light (texture x ramp, added), w: the modern look's brightness of a light
};
cbuffer DecalCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 mapInfo;      // x, y: 1 / the map's size; z: 1 = the map has water
};
cbuffer Instances : register(b2) {
  Instance instances[1024];
};
Texture2D decalTexture : register(t0);
Texture2D rampTexture : register(t2);
Texture2D waterMap : register(t3);
Texture2D waterRamp : register(t4);
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
  float3 d = world - i.centre.xyz;
  if (abs(d.y) > i.centre.w) discard;
  // into the decal's own frame (the heading turned back)
  float2 local = float2(d.x * i.shape.z - d.z * i.shape.w, d.x * i.shape.w + d.z * i.shape.z) / i.shape.xy;
  if (any(abs(local) > 1)) discard;
  float4 c = decalTexture.Sample(samplerAniso, local * 0.5 + 0.5);
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
    float3 turned = float3(inDecal.x * i.shape.z + inDecal.z * i.shape.w, inDecal.y, -inDecal.x * i.shape.w + inDecal.z * i.shape.z);
    float3 tilted = normalize(ground + turned - float3(0, 1, 0));
    float shade = SunShadow(world, ground);
    float3 before = OriginalLight(dot(sunDirection, ground), shade), after = OriginalLight(dot(sunDirection, tilted), shade);
    float3 change = clamp(after / max(before, 0.03), 0.35, 2.5);
    return float4(lerp(float3(1, 1, 1), change, saturate(c.r * i.options.x)), 1);
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
  float shadow = SunShadow(world, N);
  // under water: tinted toward the water ramp's colour by depth, as the ground is (terrain.cpp)
  float4 tint = 0;
  if (mapInfo.z > 0.5) tint = waterRamp.Sample(samplerLinearClamp, float2(waterMap.Sample(samplerLinearClamp, world.xz * mapInfo.xy).g, 0.5));
  if (look > 0.5) {
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
  std::string source = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kLightingShaderBlock + kShader;
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
  pipeline_ = device_->CreatePipeline(pd, error);
  if (!pipeline_) return false;
  pd.name = "lights on the ground";
  pd.blend = gfx::Blend::Additive;
  lights_ = device_->CreatePipeline(pd, error);
  if (!lights_) return false;
  pd.name = "normal decals";
  pd.blend = gfx::Blend::Multiply;
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

gfx::Texture Decals::Resolve(const std::string& texture) {
  if (texture.empty()) return {};
  // (a path begins with a slash; a script's short name may have a folder in it: "Tarmacs/Tar12x_01_albedo")
  if (texture[0] == '/') return vfs_->Exists(texture) ? textures_->Get(texture) : gfx::Texture{};
  for (const char* folder : {"/env/common/decals/", "/env/common/splats/"}) {
    const std::string path = std::string(folder) + texture + ".dds";
    if (vfs_->Exists(path)) return textures_->Get(path);
  }
  return {};
}

bool Decals::Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born, float seconds) {
  const gfx::Texture t = Resolve(texture);
  if (!t || sizeX == 0 || sizeZ == 0) return false;  // (a negative size mirrors it)
  decals_.push_back({t, {}, centre, sizeX * 0.5f, sizeZ * 0.5f, heading, cutoff, born, seconds});
  return true;
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

void Decals::Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now) {
  drawn = 0;
  // those whose time is over go
  decals_.erase(std::remove_if(decals_.begin(), decals_.end(), [now](const Decal& d) { return d.seconds > 0 && now - d.born >= static_cast<double>(d.seconds); }),
                decals_.end());
  visible_.clear();
  for (size_t which = 0; which < decals_.size() + marks_.size(); ++which) {
    const Decal& d = which < decals_.size() ? decals_[which] : marks_[which - decals_.size()];
    const float reach = std::max(std::fabs(d.halfX), std::fabs(d.halfZ)) * 1.5f;
    // the ground under a decal may rise and fall across it: the box is tall for its width
    const float halfHeight = reach * 0.7f + 1.5f;
    const Vec3 r{reach, halfHeight, reach};
    const float distance = Length(d.centre - view.eye);
    if (d.cutoff > 0 && distance > d.cutoff) continue;
    if (!view.BoxVisible(d.centre - r, d.centre + r)) continue;
    if (now < d.born) continue;  // (a light whose beat the picture has not reached)
    float alpha = 1;
    if (d.seconds > 0) alpha = Clamp(static_cast<float>(d.born + static_cast<double>(d.seconds) - now) / 5.0f, 0, 1);
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
    n.light = d.mark ? 3.0f : d.ramp ? 1.0f : d.normal ? 2.0f : 0.0f;
    n.glow = glow;
    visible_.push_back({d.texture.id, d.ramp.id, d.mark ? 3 : d.ramp ? 2 : d.normal ? 1 : 0, n});
  }
  marks_.clear();
  if (visible_.empty()) return;
  // pictures first, then lights; by texture; those of one texture keep the order they were made in
  std::stable_sort(visible_.begin(), visible_.end(), [](const Visible& a, const Visible& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.texture != b.texture ? a.texture < b.texture : a.ramp < b.ramp;
  });

  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const bool water = waterMap_ && waterRamp_ && mapWidth_ > 0 && mapHeight_ > 0;
  const float cameraConstants[16] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                                     up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(view.height),
                                     back.x, back.y, back.z, camera.zNear,
                                     water ? 1 / mapWidth_ : 0, water ? 1 / mapHeight_ : 0, water ? 1.0f : 0.0f, 0};
  int bound = -1;
  for (size_t at = 0; at < visible_.size();) {
    const uint32_t texture = visible_[at].texture, ramp = visible_[at].ramp;
    const int kind = visible_[at].kind;
    size_t n = 0;
    while (at + n < visible_.size() && visible_[at + n].texture == texture && visible_[at + n].ramp == ramp && visible_[at + n].kind == kind &&
           n < kMaxInstances) {
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
    device_->SetConstants(2, batch_.data(), static_cast<uint32_t>(n * sizeof(Instance)));
    device_->Draw(36, 0, static_cast<uint32_t>(n));
    drawn += static_cast<int>(n);
    at += n;
  }
}

}  // namespace render
