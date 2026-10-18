#include "hidden_outlines.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "mesh_renderer.h"
#include "models.h"

namespace render {
namespace {

// The masks: a marked unit drawn again where its bones and place put it (as the scene's pass computes them), into
// a layer's picture with a depth of its own, so that each pixel keeps the nearest marked unit there: its mark (the
// instance's colour holds its four bytes, MeshRenderer::DrawMarked) and whether its surface there lies behind the
// scene's depth.
const char kMaskShader[] = R"hlsl(
struct Instance {
  float4 row0, row1, row2;
  float4 color;  // the mark's bytes, 0..255: palette index, which unit, strength, layer
  float4 bones;
};
cbuffer Instances : register(b2) {
  Instance instances[768];
};
cbuffer Masks : register(b1) {
  float4 scene;   // x, y: the scene's pixels for one of the screen's; z, w: the scene's size in pixels
  float4 behind;  // x: the camera's near distance; y: the share of a distance, z: the world units more, that count as behind;
                  // w: 1 = the scene's pixels do not lie on the screen's (drawn smaller, or shifted by the temporal upscaler)
  float4 ground;  // x, y: the map's width and height (0: no ground); z: world units of the heights' largest value; w: how
                  // far over the ground a unit's surface still counts as in it
};
StructuredBuffer<float4> boneRows : register(t0, space1);
#ifdef MSAA
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float> heightMap : register(t2);  // the ground's heights, one per grid point (Terrain::HeightTexture)

struct VIn {
  float3 position : POSITION;
  uint4 bones : BLENDINDICES;
};
struct VOut {
  float4 position : SV_Position;
  nointerpolation float4 mark : TEXCOORD0;
  float3 world : TEXCOORD1;
};

// The ground's height at a place of the map (between its grid points, as the ground is drawn).
float GroundAt(float2 xz) {
  float2 p = clamp(xz, 0, ground.xy);
  float2 f = floor(p);
  float2 t = p - f;
  int2 c = int2(f), most = int2(ground.xy);
  float a = heightMap.Load(int3(min(c, most), 0)), b = heightMap.Load(int3(min(c + int2(1, 0), most), 0));
  float d = heightMap.Load(int3(min(c + int2(0, 1), most), 0)), e = heightMap.Load(int3(min(c + int2(1, 1), most), 0));
  return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y) * ground.z;
}

VOut VS(VIn i, uint instance : SV_InstanceID) {
  Instance n = instances[instance];
  float4 p = float4(i.position, 1);
  [branch] if (n.bones.x >= 0) {
    uint at = (uint)n.bones.x + i.bones.x * 3;
    float4 b0 = boneRows[at], b1 = boneRows[at + 1], b2 = boneRows[at + 2];
    p = float4(dot(b0, p), dot(b1, p), dot(b2, p), 1);
  }
  float3 world = float3(dot(n.row0, p), dot(n.row1, p), dot(n.row2, p));
  VOut o;
  o.position = mul(viewProj, float4(world, 1));
  o.mark = n.color;
  o.world = world;
  return o;
}

// The farthest of what the scene holds at a pixel of its own (its depth is reversed: the farthest is the least).
float SceneFarthest(int2 at) {
#ifdef MSAA
  float d = sceneDepth.Load(at, 0);
  [unroll] for (int s = 1; s < SAMPLES; ++s) d = min(d, sceneDepth.Load(at, s));
  return d;
#else
  return sceneDepth.Load(int3(at, 0));
#endif
}

float4 PS(VOut i) : SV_Target {
  // what of it lies in the ground (a building's foot on a slope, a tank's tracks in a dip) is not hidden: it is
  // buried, and its line would run along every foot; it counts as no unit (its depth kept: what is behind stays so)
  if (ground.x > 0 && i.world.y < GroundAt(i.world.xz) + ground.w) return float4(0, 0, 0, 0.5);
  // (the scene drawn smaller or shifted: the farthest of the four of its pixels round the place, else a pixel of it that
  // also holds a nearer part of the same unit - an arm in front of the body - would count as hiding the body)
  float d;
  [branch] if (behind.w > 0.5) {
    int2 at = int2(floor(i.position.xy * scene.xy - 0.5)), most = int2(scene.zw) - 1;
    d = min(min(SceneFarthest(clamp(at, int2(0, 0), most)), SceneFarthest(clamp(at + int2(1, 0), int2(0, 0), most))),
            min(SceneFarthest(clamp(at + int2(0, 1), int2(0, 0), most)), SceneFarthest(clamp(at + int2(1, 1), int2(0, 0), most))));
  } else {
    d = SceneFarthest(min(int2(i.position.xy * scene.xy), int2(scene.zw) - 1));
  }
  // (distances along the view: the near distance over the depth, the far plane endless; the sky is nothing)
  float here = behind.x / max(i.position.z, 1e-30), there = d > 0 ? behind.x / d : 1e30;
  bool hidden = there * (1 + behind.y) + behind.z < here;
  return float4(i.mark.xyz / 255, hidden ? 1 : 0.5);
}
)hlsl";

// The outlines over the finished picture: a pixel of a unit's hidden part is on its line where another unit, or
// none, lies within the line's width of it (in rings of a pixel each, eight ways round, the last ring taken by the
// share of it the width reaches); else it is inside, and gets the fill.
const char kLayShader[] = R"hlsl(
Texture2D moving : register(t0);    // the moving units' layer
Texture2D standing : register(t1);  // the buildings'
cbuffer Outlines : register(b1) {
  float4 look;  // x: the line's width in pixels; y: the line's share of colour; z: the fill's; w: rings (1..4)
  float4 size;  // x, y: the pictures' last pixel across and down
  float4 palette[255];
};
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// (a pixel's mark: r its palette index from 1, 0 none; g which unit; b its strength; a 255 hidden, 128 seen)
uint4 MarkAt(Texture2D layer, int2 p) { return uint4(layer.Load(int3(clamp(p, int2(0, 0), int2(size.xy)), 0)) * 255 + 0.5); }

float4 PS(float4 at : SV_Position) : SV_Target {
  int2 p = int2(at.xy);
  uint4 m = MarkAt(moving, p);
  bool isMoving = m.r > 0 && m.a > 191;
  uint4 t = m;
  if (!isMoving) {
    // (a moving unit seen here stands in front of whatever building lies behind it)
    if (m.r > 0) discard;
    t = MarkAt(standing, p);
    if (t.r == 0 || t.a < 192) discard;
  }
  static const float2 ways[8] = {float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
                                 float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071)};
  float edge = 0;
  int rings = int(look.w), hiddenNear = 0;
  for (int k = 1; k <= rings; ++k) {
    float reach = saturate(look.x - (k - 1));
    if (reach <= edge) break;
    for (int w = 0; w < 8; ++w) {
      int2 q = p + int2(round(ways[w] * k));
      uint4 n = isMoving ? MarkAt(moving, q) : MarkAt(standing, q);
      if (n.r != t.r || n.g != t.g) edge = max(edge, reach);
      else if (k == 1 && n.a > 191) ++hiddenNear;
    }
  }
  // (a speck - a pixel or two hidden with none of the same unit hidden round them - is not shown: a leaf's tip over a
  // unit, or a gap between its own parts that the scene, drawn smaller, did not see)
  if (hiddenNear < 2) discard;
  float strength = t.b / 255.0;
  return float4(palette[t.r - 1].rgb, strength * lerp(look.z, look.y, edge));
}
)hlsl";

}  // namespace

HiddenOutlines::~HiddenOutlines() {
  for (gfx::Texture t : {masks_[0], masks_[1], depth_})
    if (t) device_->Destroy(t);
}

bool HiddenOutlines::Init(std::string* error) {
  const std::string head = gSceneSamples > 1 ? "#define MSAA 1\n#define SAMPLES " + std::to_string(gSceneSamples) + "\n" : std::string();
  const std::string maskSource = head + kViewShaderBlock + kMaskShader;
  gfx::PipelineDesc pd;
  pd.name = "hidden outlines, masks";
  pd.vertexSource = pd.pixelSource = maskSource;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, offsetof(ScmVertex, position)}, {"BLENDINDICES", 0, gfx::AttrType::UByte4, offsetof(ScmVertex, bones)}};
  pd.stride = sizeof(ScmVertex);
  pd.colorFormat = gfx::Format::RGBA8;
  mask_ = device_->CreatePipeline(pd, error);
  if (!mask_) return false;
  gfx::PipelineDesc ld;
  ld.name = "hidden outlines";
  ld.vertexSource = ld.pixelSource = kLayShader;
  ld.cull = gfx::Cull::None;
  ld.depthTest = ld.depthWrite = false;
  ld.blend = gfx::Blend::Alpha;
  ld.colorFormat = gfx::Format::RGBA8;  // (the window's)
  lay_ = device_->CreatePipeline(ld, error);
  return static_cast<bool>(lay_);
}

void HiddenOutlines::Begin(int screenWidth, int screenHeight) {
  screenWidth_ = std::max(screenWidth, 1);
  screenHeight_ = std::max(screenHeight, 1);
  palette_.clear();
  layers_[0] = layers_[1] = false;
  marked = 0;
  drawn = false;
}

float HiddenOutlines::Strength(float radius, float distance, float fovY, float iconShown) const {
  // (its bounding sphere's width on the screen, in pixels of a screen 1080 lines high)
  const float across = radius * 1080.0f / (std::tan(fovY * 0.5f) * std::max(distance, 1e-3f));
  const float t = Clamp((across - smallest) / std::max(largest - smallest, 1e-3f), 0, 1);
  return t * t * (3 - 2 * t) * (1 - Clamp(iconShown, 0, 1));
}

uint32_t HiddenOutlines::Mark(Vec3 color, bool building, uint32_t id, float strength) {
  const uint32_t strong = static_cast<uint32_t>(std::lround(Clamp(strength, 0, 1) * 255.0f));
  if (strong == 0) return 0;
  // (its colour's place in this frame's palette: a few armies' colours, so a look along it is short)
  size_t index = 0;
  while (index < palette_.size() && (palette_[index].x != color.x || palette_[index].y != color.y || palette_[index].z != color.z)) ++index;
  if (index == palette_.size()) {
    if (palette_.size() >= 255) return 0;  // (more colours than a frame can hold: unheard of, never more than the armies')
    palette_.push_back(color);
  }
  // (which unit: enough of its id that two units next to each other are told apart)
  const uint32_t unit = (id ^ (id >> 8) ^ (id >> 16) ^ (id >> 24)) & 0xffu;
  ++marked;
  layers_[building ? 1 : 0] = true;
  return static_cast<uint32_t>(index + 1) | unit << 8 | strong << 16 | (building ? 1u : 0u) << 24;
}

void HiddenOutlines::Masks(MeshRenderer* meshes, const View& screen, gfx::Texture sceneDepth, int sceneWidth, int sceneHeight, float zNear,
                           bool shifted) {
  drawn = false;
  if (!marked || !mask_ || !lay_ || !sceneDepth || !meshes->MarkedDrawn()) return;
  const int w = screen.width, h = screen.height;
  if (w != width_ || h != height_ || !masks_[0]) {
    for (gfx::Texture& t : masks_) {
      if (t) device_->Destroy(t);
      t = {};
    }
    if (depth_) device_->Destroy(depth_);
    gfx::TargetDesc td;
    td.width = w;
    td.height = h;
    td.format = gfx::Format::RGBA8;
    for (gfx::Texture& t : masks_) t = device_->CreateTarget(td);
    td.format = gfx::Format::D32;
    depth_ = device_->CreateTarget(td);
    width_ = w;
    height_ = h;
    if (!masks_[0] || !masks_[1] || !depth_) return;
  }
  const float constants[12] = {static_cast<float>(sceneWidth) / static_cast<float>(w), static_cast<float>(sceneHeight) / static_cast<float>(h),
                               static_cast<float>(sceneWidth), static_cast<float>(sceneHeight), zNear, behindShare, behindUnits,
                               shifted || sceneWidth != w || sceneHeight != h ? 1.0f : 0.0f,
                               ground_[0], ground_[1], ground_[2], buried};
  for (int layer = 0; layer < 2; ++layer) {
    gfx::PassDesc pass;  // (cleared: no mark, and the depth at the far end)
    pass.color = masks_[layer];
    pass.depth = depth_;
    device_->BeginPass(pass);
    if (!layers_[layer]) continue;
    device_->SetPipeline(mask_);
    device_->SetConstants(1, constants, sizeof constants);
    device_->SetTexture(1, sceneDepth);
    device_->SetTexture(2, heights_);
    meshes->DrawMarked(screen, 0xff000000u, static_cast<uint32_t>(layer) << 24);
  }
  device_->SetTexture(1, {});
  device_->SetTexture(2, {});
  drawn = true;
}

void HiddenOutlines::Draw() {
  if (!drawn) return;
  // (the line's width by the screen's height; at least a pixel)
  const float lineWidth = std::max(width * static_cast<float>(screenHeight_) / 1080.0f, 1.0f);
  std::vector<float> constants(8 + 255 * 4, 0.0f);
  constants[0] = lineWidth;
  constants[1] = line;
  constants[2] = fill;
  constants[3] = static_cast<float>(std::min(static_cast<int>(std::ceil(lineWidth)), 4));
  constants[4] = static_cast<float>(width_ - 1);
  constants[5] = static_cast<float>(height_ - 1);
  for (size_t i = 0; i < palette_.size(); ++i) {
    constants[8 + i * 4] = palette_[i].x;
    constants[8 + i * 4 + 1] = palette_[i].y;
    constants[8 + i * 4 + 2] = palette_[i].z;
  }
  device_->SetPipeline(lay_);
  device_->SetConstants(1, constants.data(), static_cast<uint32_t>((8 + std::max(palette_.size(), size_t{1}) * 4) * sizeof(float)));
  device_->SetTexture(0, masks_[0]);
  device_->SetTexture(1, masks_[1]);
  device_->Draw(3);
  device_->SetTexture(0, {});
  device_->SetTexture(1, {});
}

}  // namespace render
