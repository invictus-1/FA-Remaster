#include "ranges.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

constexpr size_t kBatch = 256;

// What the three shaders share: the camera, and the place under a pixel from the scene's depth
// (as decals.cpp finds it).
const char kCommon[] = R"hlsl(
cbuffer RangeCamera : register(b1) {
  float4 cameraRight;  // w: tan(half the field of view)
  float4 cameraUp;     // w: aspect
  float4 cameraBack;   // w: near distance
  float4 lineInfo;     // x: the outline's thickness; y: that of a hole's outline; z: the water's level; w: 1 = the pass over the water
  float4 boxInfo;      // x: the middle of the heights the ground lies between; y: half of them; z: 1 = there are
                       // outlines of earlier groups to lay this one over
  float4 lineColor;    // as the scene holds colour
};
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(p, 0); }
#else
Texture2D<float> sceneDepth : register(t1);
float DepthAt(int2 p) { return sceneDepth.Load(int3(p, 0)); }
#endif

float3 WorldAt(float2 pixel, float depth) {
  float away = cameraBack.w / depth;
  float2 ndc = float2(pixel.x * viewport.z * 2 - 1, 1 - pixel.y * viewport.w * 2);
  return cameraPosition + (cameraRight.xyz * (ndc.x * cameraRight.w * cameraUp.w) + cameraUp.xyz * (ndc.y * cameraRight.w) - cameraBack.xyz) * away;
}
)hlsl";

// A ring: a box round it (its far faces, so that it also works with the camera inside). Red: how
// much of the pixel's place lies in the ring; green: in the ring made smaller by the line's
// thickness. Added up over a group's rings.
const char kMaskShader[] = R"hlsl(
cbuffer Rings : register(b2) {
  float4 rings[256];  // x, z of the middle; the radius; the inner radius (0: none)
};

struct VOut {
  float4 position : SV_Position;
  nointerpolation float4 ring : TEXCOORD0;
};

VOut VS(float3 corner : POSITION, uint instance : SV_InstanceID) {
  float4 r = rings[instance];
  float reach = r.z + 1;
  VOut o;
  o.position = mul(viewProj, float4(r.x + corner.x * reach, boxInfo.x + corner.y * boxInfo.y, r.y + corner.z * reach, 1));
  o.position.z = 0.5 * o.position.w;  // (no depth is tested: nothing of it may be cut off as too far or too near)
  o.ring = r;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float depth = DepthAt(int2(i.position.xy));
  if (depth <= 0) discard;  // nothing drawn here
  float3 world = WorldAt(i.position.xy, depth);
  if (world.y < lineInfo.z) {  // under water: the place on the water's surface the eye looks through
    float3 ray = world - cameraPosition;
    world = cameraPosition + ray * ((lineInfo.z - cameraPosition.y) / min(ray.y, -1e-4));
  }
  float d = length(world.xz - i.ring.xy);
  // what one pixel spans there, for soft edges; and no line thinner than a pixel and a half
  float pixel = max(dot(world - cameraPosition, -cameraBack.xyz) * 2 * cameraRight.w * viewport.w, 1e-4);
  float outer = max(lineInfo.x, pixel * 1.5), inner = max(lineInfo.y, pixel * 1.5);
  float whole = saturate((i.ring.z - d) / pixel + 0.5);
  float core = saturate((i.ring.z - outer - d) / pixel + 0.5);
  if (i.ring.w > 0) {
    whole *= saturate((d - i.ring.w) / pixel + 0.5);
    core *= saturate((d - i.ring.w - inner) / pixel + 0.5);
  }
  return float4(whole, core, 0, 1);
}
)hlsl";

const char kFullScreen[] = R"hlsl(
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";

// A group's outline, from its rings' sums: where the whole rings are and the smaller ones are not;
// laid over the outlines of the groups before it. Where two groups' lines lie on one another (a
// commander's direct, indirect and anti-air rings at one radius) the later one takes the earlier
// one's place, soft edge and all: blended over it, the earlier colours showed through the later
// line's half-covered edge pixels as a pale second line beside it.
const char kGatherShader[] = R"hlsl(
Texture2D mask : register(t0);
Texture2D before : register(t2);
float4 PS(float4 position : SV_Position) : SV_Target {
  float4 m = mask.Load(int3(position.xy, 0));
  float edge = saturate(m.r) * (1 - saturate(m.g));
  float4 mine = float4(lineColor.rgb * edge, edge * lineColor.a);
  if (boxInfo.z < 0.5) return mine;
  float4 under = before.Load(int3(position.xy, 0));
  // (what of the earlier lines this one does not cover stays; together they cover as much of the
  // pixel as the one that covers more)
  float left = max(under.a - mine.a, 0) / max(under.a, 1e-4);
  return float4(mine.rgb + under.rgb * left, max(under.a, mine.a));
}
)hlsl";

// The outlines over the scene: over the land or over the water, told by the scene's depth.
const char kLayShader[] = R"hlsl(
Texture2D lines : register(t0);
float4 PS(float4 position : SV_Position) : SV_Target {
  float4 c = lines.Load(int3(position.xy, 0));
  if (c.a <= 0) discard;
  float depth = DepthAt(int2(position.xy));
  if (depth <= 0) discard;
  bool under = WorldAt(position.xy, depth).y < lineInfo.z;
  if (under != (lineInfo.w > 0.5)) discard;
  return c;
}
)hlsl";

}  // namespace

bool Ranges::Init(std::string* error) {
  const std::string head = std::string(gSceneSamples > 1 ? "#define MSAA 1\n" : "") + kViewShaderBlock + kCommon;
  const std::string maskSource = head + kMaskShader, gatherSource = head + kFullScreen + kGatherShader, laySource = head + kFullScreen + kLayShader;
  gfx::PipelineDesc pd;
  pd.name = "range rings";
  pd.vertexSource = pd.pixelSource = maskSource;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}};
  pd.stride = 12;
  pd.cull = gfx::Cull::Front;  // the far faces
  pd.depthTest = pd.depthWrite = false;
  pd.depthTarget = false;
  pd.blend = gfx::Blend::Additive;
  pd.colorFormat = gfx::Format::RGBA8;
  mask_ = device_->CreatePipeline(pd, error);
  if (!mask_) return false;
  pd.name = "range outlines";
  pd.vertexSource = pd.pixelSource = gatherSource;
  pd.attrs.clear();
  pd.stride = 0;
  pd.cull = gfx::Cull::None;
  pd.blend = gfx::Blend::Opaque;  // (it lays itself over the picture so far, read from the other of two targets)
  pd.colorFormat = kSceneFormat;
  gather_ = device_->CreatePipeline(pd, error);
  if (!gather_) return false;
  pd.blend = gfx::Blend::Premultiplied;
  pd.name = "range outlines on the scene";
  pd.vertexSource = pd.pixelSource = laySource;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  lay_ = device_->CreatePipeline(pd, error);
  if (!lay_) return false;
  // a cube from -1 to 1, its faces wound counter-clockwise seen from outside (as decals.cpp has it)
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
  return static_cast<bool>(cube_);
}

bool Ranges::Any() const {
  for (const RangeGroup& g : groups)
    if (!g.rings.empty()) return true;
  return false;
}

void Ranges::CameraConstants(const View& view, const Camera& camera, float* out) const {
  Vec3 right, up, back;
  camera.Axes(&right, &up, &back);
  const float c[12] = {right.x, right.y, right.z, std::tan(camera.fovY * 0.5f),
                       up.x, up.y, up.z, static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)),
                       back.x, back.y, back.z, camera.zNear};
  std::copy(c, c + 12, out);
}

void Ranges::Build(const View& view, const Camera& camera, gfx::Texture sceneDepth) {
  built_ = false;
  drawnRings = 0;
  if (!Any() || view.width <= 0 || view.height <= 0) return;
  if (view.width != width_ || view.height != height_) {
    if (maskTarget_) device_->Destroy(maskTarget_);
    if (lines_) device_->Destroy(lines_);
    if (linesBefore_) device_->Destroy(linesBefore_);
    gfx::TargetDesc td;
    td.width = view.width;
    td.height = view.height;
    td.format = gfx::Format::RGBA8;
    maskTarget_ = device_->CreateTarget(td);
    td.format = kSceneFormat;
    lines_ = device_->CreateTarget(td);
    linesBefore_ = device_->CreateTarget(td);
    width_ = view.width;
    height_ = view.height;
  }
  // how far the camera is between the nearest and the farthest zoom: the thickness goes with it
  const float zoom = std::clamp((camera.distance - zoomNear) / std::max(zoomFar - zoomNear, 1e-3f), 0.0f, 1.0f);
  const bool modern = view.constants.look > 0.5f;
  float constants[24];
  CameraConstants(view, camera, constants);
  constants[14] = waterLevel;
  constants[15] = 0;
  constants[16] = (low + high) * 0.5f;
  constants[17] = (high - low) * 0.5f + 2;
  constants[18] = constants[19] = 0;
  std::vector<float> batch(kBatch * 4);
  bool first = true;
  for (const RangeGroup& g : groups) {
    if (g.rings.empty()) continue;
    constants[12] = g.outerNear + (g.outerFar - g.outerNear) * zoom;
    constants[13] = g.innerNear + (g.innerFar - g.innerNear) * zoom;
    // the colour as the scene holds it: the modern look's picture is linear light, mapped back at
    // the end (as a mark on the ground is given, decals.cpp); a glow is light beyond what the
    // screen can show, which the picture's glow then spreads
    const float glow = 1 + 3 * static_cast<float>(std::clamp(g.glow, 0, 255)) / 255.0f;
    for (int k = 0; k < 3; ++k) {
      const float c = static_cast<float>((g.color >> (16 - 8 * k)) & 0xff) / 255.0f;
      constants[20 + k] = modern ? std::pow(c, 2.2f) * 1.6f * glow * night : c;
    }
    constants[23] = 1;
    gfx::PassDesc pass;
    pass.color = maskTarget_;
    device_->BeginPass(pass);
    device_->SetPipeline(mask_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(1, sceneDepth);
    device_->SetVertexBuffer(cube_);
    for (size_t at = 0; at < g.rings.size(); at += kBatch) {
      const size_t n = std::min(kBatch, g.rings.size() - at);
      for (size_t i = 0; i < n; ++i) {
        const RangeRing& r = g.rings[at + i];
        batch[i * 4] = r.centre.x;
        batch[i * 4 + 1] = r.centre.z;
        batch[i * 4 + 2] = r.radius;
        batch[i * 4 + 3] = r.innerRadius;
      }
      device_->SetConstants(2, batch.data(), static_cast<uint32_t>(n * 4 * sizeof(float)));
      device_->Draw(36, 0, static_cast<uint32_t>(n));
      drawnRings += static_cast<int>(n);
    }
    // (into the other of the two pictures, over the one made so far)
    std::swap(lines_, linesBefore_);
    constants[18] = first ? 0.0f : 1.0f;
    gfx::PassDesc gather;
    gather.color = lines_;
    gather.clearColor = true;
    device_->BeginPass(gather);
    device_->SetPipeline(gather_);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(0, maskTarget_);
    device_->SetTexture(2, linesBefore_);
    device_->Draw(3);
    constants[18] = 0;
    first = false;
  }
  built_ = !first;
}

void Ranges::Lay(const View& view, const Camera& camera, gfx::Texture sceneDepth, bool onWater) {
  if (!built_) return;
  float constants[24] = {};
  CameraConstants(view, camera, constants);
  constants[14] = waterLevel;
  constants[15] = onWater ? 1.0f : 0.0f;
  device_->SetPipeline(lay_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(0, lines_);
  device_->SetTexture(1, sceneDepth);
  device_->Draw(3);
}

}  // namespace render
