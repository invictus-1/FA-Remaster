#include "ray_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "linalg.h"
#include "mesh_renderer.h"
#include "models.h"
#include "scmap.h"

namespace render {
namespace {

// A level's colour (RayScene): its albedo's average over a small level of the texture (about 4 x 4; the holes
// of leaves, by the albedo's alpha, left out), and its team colour's share, the specular's alpha averaged, or for
// leaves their coverage, the albedo's alpha averaged.
const char kAverage[] = R"hlsl(
Texture2D albedo : register(t0);
Texture2D specular : register(t1);
RWTexture2D<float4> colours : register(u0);
cbuffer Entry : register(b1) {
  uint4 entry;  // x: the entry; y: 1 = the albedo's alpha says where its holes are
};
[numthreads(1, 1, 1)] void CS() {
  uint w, h, levels;
  albedo.GetDimensions(0, w, h, levels);
  uint level = levels > 3 ? levels - 3 : levels - 1;
  uint lw = max(w >> level, 1u), lh = max(h >> level, 1u);
  float3 sum = 0;
  float weight = 0, texels = 0;
  for (uint y = 0; y < min(lh, 8u); ++y)
    for (uint x = 0; x < min(lw, 8u); ++x) {
      float4 c = albedo.Load(int3(x, y, level));
      float a = entry.y ? c.a : 1;
      sum += c.rgb * a;
      weight += a;
      texels += 1;
    }
  specular.GetDimensions(0, w, h, levels);
  level = levels > 3 ? levels - 3 : levels - 1;
  lw = max(w >> level, 1u);
  lh = max(h >> level, 1u);
  float team = 0, n = 0;
  for (uint j = 0; j < min(lh, 8u); ++j)
    for (uint i = 0; i < min(lw, 8u); ++i) {
      team += specular.Load(int3(i, j, level)).a;
      n += 1;
    }
  float share = entry.y ? (texels > 0 ? weight / texels : 1) : (n > 0 ? team / n : 0);
  colours[uint2(entry.x % 1024, entry.x / 1024)] = float4(weight > 0 ? sum / weight : float3(0.5, 0.5, 0.5), share);
}
)hlsl";

// What each instance is, for every shader that traces (kRayShaderBlock's t37), from its data and its level's colour:
// rgb its albedo (linear; a unit's team colour in by its share), a 0..1 for leaves (their coverage), 2 something
// solid, 3 the ground, 4 the water. A thread for each instance.
const char kResolve[] = R"hlsl(
Texture2D colours : register(t0);
StructuredBuffer<uint4> instanceData : register(t0, space1);
RWTexture2D<float4> picture : register(u0);
cbuffer Count : register(b1) {
  uint4 count;  // x: how many instances
};
float3 Lin(float3 c) { return pow(max(c, 0), 2.2); }
[numthreads(64, 1, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  if (i >= count.x) return;
  uint4 info = instanceData[i];
  float4 o;
  if (info.y & 1) o = float4(0, 0, 0, 3);
  else if (info.y & 2) o = float4(0.02, 0.05, 0.08, 4);
  else {
    float4 e = colours.Load(int3(info.x % 1024, info.x / 1024, 0));
    float3 team = float3(info.z & 255, (info.z >> 8) & 255, (info.z >> 16) & 255) / 255.0;
    o.rgb = lerp(Lin(e.rgb), Lin(team), info.y & 12 ? 0 : e.a);
    o.a = info.y & 8 ? saturate(e.a) : 2;
  }
  picture[uint2(i % 1024, i / 1024)] = o;
}
)hlsl";

// The check view (RayScene::Check): a ray from the camera through each pixel; what it meets lit by the sun,
// whose shadow another ray finds, and by the sky. The ground by its own normals (the terrain's normal map) and a
// colour of its own; a model by its colour, facing the ray (no normals are kept for models).
const char kCheck[] = R"hlsl(
RWTexture2D<float4> picture : register(u0);
cbuffer RayCheck : register(b1) {
  row_major float4x4 back;  // the picture's -1..1 and depth to the world (the inverse of the view's matrix)
  float4 size;              // x, y the picture's width and height; z, w the map's
};
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
  float2 ndc = (id.xy + 0.5) / size.xy * float2(2, -2) + float2(-1, 1);
  float4 a = mul(back, float4(ndc, 1, 1)), b = mul(back, float4(ndc, 0.5, 1));
  float3 from = a.xyz / a.w, d = normalize(b.xyz / b.w - from);
  const uint seed = RayHash(id.x * 1973 + id.y * 9277 + 1);
  float t;
  uint instance;
  float3 c;
  if (RayTrace(from, d, 0, 1e6, 0xff, seed, t, instance)) {
    float3 p = from + d * t, albedo, n;
    RaySurfaceAt(instance, p, d, size.zw, albedo, n);
    c = RayLitAt(p, n, albedo, seed, 48);
  } else {
    c = skyZenith.w > 0.5 ? ComputedSky(d) : float3(0.4, 0.5, 0.65);
  }
  picture[id.xy] = float4(c, 1);
}
)hlsl";

// What the rays see of a level: solid surfaces only (not glass, shields, effects' shapes and the like).
bool Solid(const ModelLod& l) {
  return !l.glass && !l.shield && !l.hidden && !l.blend && l.unlit == 0 && l.nuke == 0 && !l.frame && !l.shell && l.impact == 0 &&
         !l.fades && !l.remasteredEffect && !l.chrome;
}

uint32_t Pack(const float* c) {
  auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  return b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16) | 0xff000000u;
}

constexpr int kColoursWide = 1024, kColoursHigh = 64;  // (65,536 levels' colours)
constexpr uint32_t kMostInstances = 1024 * 128;          // (what each instance is: 131,072 at most)

}  // namespace

RayScene::~RayScene() {
  for (auto& [serial, level] : levels_) device_->Destroy(level.structure);
  for (gfx::Structure s : {scene_, ground_, water_}) device_->Destroy(s);
  for (gfx::Buffer b : {groundVertices_, groundIndices_, waterVertices_, instanceData_}) device_->Destroy(b);
  for (gfx::Texture t : {colours_, picture_, instancesPicture_}) device_->Destroy(t);
  for (gfx::Pipeline p : {average_, check_, resolve_}) device_->Destroy(p);
}

bool RayScene::Init(std::string* error) {
  available_ = false;
  if (!device_->RayTracing()) {
    if (error) *error = "no ray tracing on this device or backend";
    return false;
  }
  const std::string average = std::string(kViewShaderBlock) + kAverage, check = std::string(kViewShaderBlock) + kRayShaderBlock + kCheck;
  gfx::ComputeDesc cd;
  cd.name = "rays: a level's colour";
  cd.source = average;
  average_ = device_->CreateCompute(cd, error);
  cd.name = "rays: the check view";
  cd.source = check;
  if (average_) check_ = device_->CreateCompute(cd, error);
  const std::string resolve = std::string(kViewShaderBlock) + kResolve;
  cd.name = "rays: what each instance is";
  cd.source = resolve;
  if (check_) resolve_ = device_->CreateCompute(cd, error);
  gfx::TargetDesc td;
  td.storage = true;
  td.width = kColoursWide;
  td.height = kColoursHigh;
  td.format = gfx::Format::RGBA8;
  if (resolve_) colours_ = device_->CreateTarget(td);
  td.width = 1024;
  td.height = kMostInstances / 1024;
  td.format = gfx::Format::RGBA32F;
  if (colours_) instancesPicture_ = device_->CreateTarget(td);
  gfx::BufferDesc bd;
  bd.kind = gfx::BufferKind::Shader;
  bd.dynamic = true;
  bd.stride = 16;
  if (instancesPicture_) instanceData_ = device_->CreateBuffer(bd);
  available_ = static_cast<bool>(instanceData_);
  return available_;
}

void RayScene::SetGround(const MapData& map, int step) {
  if (!available_ || map.width <= 0 || map.height <= 0 || map.heights.empty()) return;
  step = std::max(step, 1);
  const int across = map.width / step + 1, down = map.height / step + 1;
  std::vector<float> vertices;
  vertices.reserve(static_cast<size_t>(across) * static_cast<size_t>(down) * 3);
  for (int j = 0; j < down; ++j)
    for (int i = 0; i < across; ++i) {
      const int x = std::min(i * step, map.width), z = std::min(j * step, map.height);
      const float h = static_cast<float>(map.heights[static_cast<size_t>(z) * static_cast<size_t>(map.width + 1) + static_cast<size_t>(x)]) * map.heightScale;
      vertices.insert(vertices.end(), {static_cast<float>(x), h, static_cast<float>(z)});
    }
  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(across - 1) * static_cast<size_t>(down - 1) * 6);
  for (int j = 0; j + 1 < down; ++j)
    for (int i = 0; i + 1 < across; ++i) {
      const uint32_t a = static_cast<uint32_t>(j * across + i), b = a + 1, c = a + static_cast<uint32_t>(across), d = c + 1;
      indices.insert(indices.end(), {a, c, b, b, c, d});
    }
  for (gfx::Buffer bf : {groundVertices_, groundIndices_}) device_->Destroy(bf);
  device_->Destroy(ground_);
  gfx::BufferDesc bd;
  bd.stride = 12;
  bd.size = static_cast<uint32_t>(vertices.size() * sizeof(float));
  bd.data = vertices.data();
  groundVertices_ = device_->CreateBuffer(bd);
  bd.kind = gfx::BufferKind::Index;
  bd.stride = 4;
  bd.size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
  bd.data = indices.data();
  groundIndices_ = device_->CreateBuffer(bd);
  gfx::GeometryPart part;
  part.vertices = groundVertices_;
  part.vertexCount = static_cast<uint32_t>(vertices.size() / 3);
  part.indices = groundIndices_;
  part.indexCount = static_cast<uint32_t>(indices.size());
  ground_ = groundVertices_ && groundIndices_ ? device_->CreateGeometry(&part, 1) : gfx::Structure{};
  mapWidth_ = static_cast<float>(map.width);
  mapHeight_ = static_cast<float>(map.height);
  // the water's surface: two triangles over the map at its elevation
  device_->Destroy(water_);
  device_->Destroy(waterVertices_);
  water_ = {};
  waterVertices_ = {};
  if (map.water.present) {
    const float w = mapWidth_, h = mapHeight_, y = map.water.elevation;
    const float quad[18] = {0, y, 0, 0, y, h, w, y, 0, w, y, 0, 0, y, h, w, y, h};
    gfx::BufferDesc wd;
    wd.stride = 12;
    wd.size = sizeof quad;
    wd.data = quad;
    waterVertices_ = device_->CreateBuffer(wd);
    gfx::GeometryPart wp;
    wp.vertices = waterVertices_;
    wp.vertexCount = 6;
    if (waterVertices_) water_ = device_->CreateGeometry(&wp, 1);
  }
}

void RayScene::Build(const MeshRenderer& meshes) {
  if (!available_) return;
  instances_.clear();
  info_.clear();
  auto add = [&](gfx::Structure geometry, const float* r0, const float* r1, const float* r2, uint32_t colour, uint32_t flags, uint32_t team,
                 uint8_t mask = 0x01) {
    gfx::SceneInstance s;
    std::memcpy(s.transform, r0, 4 * sizeof(float));
    std::memcpy(s.transform + 4, r1, 4 * sizeof(float));
    std::memcpy(s.transform + 8, r2, 4 * sizeof(float));
    s.geometry = geometry;
    s.id = static_cast<uint32_t>(instances_.size());
    s.mask = mask;
    instances_.push_back(s);
    info_.insert(info_.end(), {colour, flags, team, 0});
  };
  if (ground_) {
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    add(ground_, identity, identity + 4, identity + 8, 0, 1, 0);
    if (water_) add(water_, identity, identity + 4, identity + 8, 0, 2, 0, 0x02);
  }
  bool averaged = false;
  meshes.ForEachAdded([&](const Model* model, const float* r0, const float* r1, const float* r2, const float* color, bool scenery) {
    if (!model) return;
    // its coarsest solid level (a tree's coarsest is a card that faces the camera: the one before it)
    const ModelLod* chosen = nullptr;
    for (auto l = model->lods.rbegin(); l != model->lods.rend() && !chosen; ++l)
      if (Solid(*l) && l->vertices && l->indices && l->vertexCount && l->indexCount) chosen = &*l;
    if (!chosen) return;
    const ModelLod& lod = *chosen;
    auto it = levels_.find(lod.serial);
    if (it == levels_.end()) {  // (a level seen the first time: its structure, and its colour worked out)
      Level level;
      gfx::GeometryPart part;
      part.vertices = lod.vertices;
      part.positionOffset = offsetof(ScmVertex, position);
      part.vertexCount = lod.vertexCount;
      part.indices = lod.indices;
      part.indexCount = lod.indexCount;
      part.opaque = !(lod.alphaCut > 0 || lod.leaves);  // (leaves: taken by chance, the shaders' TakesLeaf)
      level.structure = device_->CreateGeometry(&part, 1);
      level.leaves = !part.opaque;
      level.colour = std::min<uint32_t>(nextColour_++, kColoursWide * kColoursHigh - 1);
      if (lod.albedo) {
        const uint32_t entry[4] = {level.colour, lod.alphaCut > 0 || lod.leaves ? 1u : 0u, 0, 0};
        device_->SetPipeline(average_);
        device_->SetConstants(1, entry, sizeof entry);
        device_->SetTexture(0, lod.albedo);
        device_->SetTexture(1, lod.specular);
        device_->SetStorage(0, colours_);
        device_->Dispatch(1);
        averaged = true;
      }
      it = levels_.emplace(lod.serial, level).first;
    }
    if (it->second.structure)
      add(it->second.structure, r0, r1, r2, it->second.colour, (scenery ? 4u : 0u) | (it->second.leaves ? 8u : 0u), scenery ? 0u : Pack(color));
  });
  if (averaged) {
    device_->SetStorage(0, {});
    device_->SetTexture(0, {});
    device_->SetTexture(1, {});
  }
  if (instances_.size() > kMostInstances) {  // (what is past the picture's room is left out of the scene)
    instances_.resize(kMostInstances);
    info_.resize(static_cast<size_t>(kMostInstances) * 4);
  }
  if (!info_.empty()) {  // what each instance is, as the shaders that trace read it
    device_->UpdateBuffer(instanceData_, info_.data(), static_cast<uint32_t>(info_.size() * sizeof(uint32_t)));
    const uint32_t count[4] = {static_cast<uint32_t>(instances_.size()), 0, 0, 0};
    device_->SetPipeline(resolve_);
    device_->SetConstants(1, count, sizeof count);
    device_->SetTexture(0, colours_);
    device_->SetShaderBuffer(instanceData_, 0);
    device_->SetStorage(0, instancesPicture_);
    device_->Dispatch((count[0] + 63) / 64, 1, 1);
    device_->SetStorage(0, {});
    device_->SetTexture(0, {});
  }
  device_->BuildScene(&scene_, instances_.data(), static_cast<uint32_t>(instances_.size()));
}

gfx::Texture RayScene::Check(const View& view, gfx::Texture groundNormals, gfx::Texture groundColours) {
  if (!available_ || !scene_ || view.width <= 0 || view.height <= 0) return {};
  if (!picture_ || pictureWidth_ != view.width || pictureHeight_ != view.height) {
    device_->Destroy(picture_);
    gfx::TargetDesc td;
    td.storage = true;
    td.width = view.width;
    td.height = view.height;
    td.format = kSceneFormat;
    picture_ = device_->CreateTarget(td);
    pictureWidth_ = view.width;
    pictureHeight_ = view.height;
    if (!picture_) return {};
  }
  struct {
    Mat4 back;
    float size[4];
  } k;
  k.back = Inverse(view.constants.viewProj);
  k.size[0] = static_cast<float>(view.width);
  k.size[1] = static_cast<float>(view.height);
  k.size[2] = mapWidth_;
  k.size[3] = mapHeight_;
  device_->SetPipeline(check_);
  device_->SetConstants(0, &view.constants, sizeof view.constants);
  device_->SetConstants(1, &k, sizeof k);
  Bind(groundNormals, groundColours);
  device_->SetStorage(0, picture_);
  device_->Dispatch(static_cast<uint32_t>((view.width + 7) / 8), static_cast<uint32_t>((view.height + 7) / 8), 1);
  device_->SetStorage(0, {});
  Unbind();
  return picture_;
}

void RayScene::Bind(gfx::Texture groundNormals, gfx::Texture groundColours) {
  device_->SetScene(kRaySceneSlot, scene_);
  device_->SetTexture(kRayInstancesSlot, instancesPicture_);
  device_->SetTexture(kRayGroundSlot, groundNormals);
  device_->SetTexture(kRayGroundSlot + 1, groundColours);
}

void RayScene::Unbind() {
  for (int slot : {kRayInstancesSlot, kRayGroundSlot, kRayGroundSlot + 1}) device_->SetTexture(slot, {});
  device_->SetScene(kRaySceneSlot, {});
}

}  // namespace render
