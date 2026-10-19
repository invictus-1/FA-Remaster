#include "placed.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

#include "bp_reader.h"
#include "core/vfs.h"
#include "dds.h"

namespace render {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char kShader[] = R"hlsl(
cbuffer Placed : register(b1) {
  float4 row0, row1, row2;  // the mesh's place: model to world
  float4 params;            // x: how much of it shows; y: the colour's strength; z: 1 = its texture's alpha does not count,
                            // 2 = it counts as it is, nothing cut ([pings] CommandFeedback3, 4); w: how much nearer the
                            // camera its depth is taken (reversed depth: added to it, PlacedMeshes::DepthPull)
};
Texture2D albedo : register(t0);

struct VOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VOut VS(float3 position : POSITION, float2 uv : TEXCOORD0) {
  float4 p = float4(position, 1);
  VOut o;
  o.position = mul(viewProj, float4(dot(row0, p), dot(row1, p), dot(row2, p), 1));
  o.position.z += params.w * o.position.w;  // (the same step everywhere once divided by w)
  o.uv = uv;
  return o;
}

float4 PS(VOut i) : SV_Target {
  float4 c = albedo.Sample(samplerLinear, i.uv);
  // ("FakeRings": the original gives the whole shape the one alpha; CommandFeedback3 and 4 blend by the texture's as
  // it is; the others cut by the texture's)
  if (params.z > 1.5) {
  } else if (params.z > 0.5) {
    c.a = 1;
  } else if (c.a <= 35.0 / 255) {
    discard;
  }
  // the modern look's picture is linear light, mapped back at the end (as a mark on the ground
  // is given, decals.cpp)
  if (look > 0.5) c.rgb = pow(max(c.rgb, 0), 2.2) * params.y;
  return float4(c.rgb, saturate(c.a * params.x));
}
)hlsl";

}  // namespace

bool PlacedMeshes::Init(std::string* error) {
  const std::string source = std::string(kViewShaderBlock) + kShader;
  gfx::PipelineDesc pd;
  pd.name = "placed meshes";
  pd.vertexSource = pd.pixelSource = source;
  pd.attrs = {{"POSITION", 0, gfx::AttrType::Float3, 0}, {"TEXCOORD", 0, gfx::AttrType::Float2, 48}};
  pd.stride = sizeof(ScmVertex);
  pd.cull = gfx::Cull::None;
  pd.depthTest = pd.depthWrite = false;
  pd.blend = gfx::Blend::Alpha;
  pd.colorFormat = kSceneFormat;
  pd.samples = gSceneSamples;
  pd.alphaWrite = false;
  over_ = device_->CreatePipeline(pd, error);
  if (!over_) return false;
  pd.name = "placed meshes, hidden by what is in front";
  pd.depthTest = true;
  tested_ = device_->CreatePipeline(pd, error);
  if (!tested_) return false;
  // [pings] CommandFeedback3 and 4: their faces turned away left out, as the original's Rasterizer_Cull_CW does (the
  // models' own culling, mesh_renderer.cpp); 4 tested against what is in front, writing no depth
  pd.cull = gfx::Cull::Back;
  pd.name = "placed meshes, CommandFeedback4";
  feedback4_ = device_->CreatePipeline(pd, error);
  if (!feedback4_) return false;
  pd.name = "placed meshes, CommandFeedback3";
  pd.depthTest = false;
  feedback3_ = device_->CreatePipeline(pd, error);
  return static_cast<bool>(feedback3_);
}

bool PlacedMeshes::Flat(const std::string& shader) {
  const std::string kind = Lower(shader);
  return kind == "commandfeedback" || kind == "commandfeedback2" || kind == "commandfeedback3" || kind == "commandfeedback4" || kind == "rallypoint" ||
         kind == "fakerings";
}

int PlacedMeshes::Feedback(const std::string& shader) {
  static const char kName[] = "commandfeedback";  // (and the number: every model is asked, so no copy is made)
  if (shader.size() != sizeof kName) return 0;
  for (size_t i = 0; i + 1 < sizeof kName; ++i)
    if (std::tolower(static_cast<unsigned char>(shader[i])) != kName[i]) return 0;
  return shader.back() == '3' ? 3 : shader.back() == '4' ? 4 : 0;
}

bool PlacedMeshes::TakeModel(const Model* model, const Mat4& world) {
  const int kind = Feedback(model);
  if (!kind) return false;
  taken_.push_back({model, world, kind});
  return true;
}

float PlacedMeshes::DepthPull(float zNear, float cameraDistance) {
  return zNear * 0.0001f / std::max(0.01f * cameraDistance, 0.01f);
}

void PlacedMeshes::DrawOne(const gfx::Pipeline& pipeline, const View& view, const ModelLod& lod, const float rows[12], const float params[4],
                           gfx::Pipeline* bound) {
  float constants[16];
  std::copy(rows, rows + 12, constants);
  std::copy(params, params + 4, constants + 12);
  if (pipeline.id != bound->id) {
    device_->SetPipeline(pipeline);
    device_->SetConstants(0, &view.constants, sizeof view.constants);
    *bound = pipeline;
  }
  device_->SetConstants(1, constants, sizeof constants);
  device_->SetTexture(0, lod.albedo);
  device_->SetVertexBuffer(lod.vertices);
  device_->SetIndexBuffer(lod.indices);
  device_->DrawIndexed(lod.indexCount);
  ++drawn;
}

void PlacedMeshes::Draw(const View& view, const std::vector<PlacedMesh>& meshes, float night, float depthPull) {
  drawn = 0;
  gfx::Pipeline bound;
  // [pings] the models of script entities and units drawn by CommandFeedback3 or 4, at their own place and size
  for (const Taken& t : taken_) {
    const Mat4& w = t.world;
    const float rows[12] = {w.m[0][0], w.m[0][1], w.m[0][2], w.m[0][3], w.m[1][0], w.m[1][1], w.m[1][2], w.m[1][3], w.m[2][0], w.m[2][1], w.m[2][2], w.m[2][3]};
    const float params[4] = {1, 1.6f * night, 2, t.kind == 4 ? depthPull : 0.0f};
    DrawOne(t.kind == 4 ? feedback4_ : feedback3_, view, t.model->lods[0], rows, params, &bound);
  }
  for (const PlacedMesh& p : meshes) {
    if (p.hidden || !Flat(p.shader)) continue;
    const Model* model = models_->Mesh(p.mesh, p.texture, p.shader, p.uniformScale);
    if (!model->ok || model->lods.empty()) continue;
    const ModelLod& lod = model->lods[0];
    const std::string kind = Lower(p.shader);
    const bool rings = kind == "fakerings";
    const int feedback = Feedback(p.shader);  // [pings]
    float size = model->scale, shows = 1;
    if (rings) {
      shows = std::clamp(p.fractionComplete, 0.0f, 1.0f);
    } else if (feedback) {
      // [pings] (CommandFeedback3, 4: its own size, all of it, whatever its lifetime)
    } else {
      // larger the farther the camera is (the original: 1 at 10.5 away, 15 at 1010.5, evenly, by
      // a measure of distance I take to be the distance to the camera)
      const float away = Length(p.position - view.eye);
      size *= std::max(1 + 14 * (away - 10.5f) * 0.001f, 0.1f);
      const float t = p.lifetime > 0 ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 0;
      size *= 1 + ((kind == "commandfeedback2" ? 1.1f : 0.7f) - 1) * t;
      if (kind != "rallypoint") shows = 1 - t;
    }
    if (shows <= 0) continue;
    const Mat4 world = Mat4::FromTRS(p.position, p.orientation, 1);
    const Vec3 s = p.scale * size;
    const float rows[12] = {world.m[0][0] * s.x, world.m[0][1] * s.y, world.m[0][2] * s.z, world.m[0][3],
                            world.m[1][0] * s.x, world.m[1][1] * s.y, world.m[1][2] * s.z, world.m[1][3],
                            world.m[2][0] * s.x, world.m[2][1] * s.y, world.m[2][2] * s.z, world.m[2][3]};
    const float params[4] = {shows, 1.6f * night, rings ? 1.0f : feedback ? 2.0f : 0.0f, feedback == 4 ? depthPull : 0.0f};
    DrawOne(rings ? tested_ : feedback == 4 ? feedback4_ : feedback == 3 ? feedback3_ : over_, view, lod, rows, params, &bound);
  }
}

// [pings] MinimapStamps (placed.h)

bool MinimapStamps::Cartographic(const std::string& shader) {
  const std::string kind = Lower(shader);
  return kind == "commandfeedback" || kind == "commandfeedback2" || kind == "commandfeedback3" || kind == "commandfeedback4" || kind == "rallypoint" ||
         kind == "fakerings" || kind == "fakeringsnodepth";
}

const MinimapStamps::Look& MinimapStamps::LookOf(TextureCache& files, const std::string& mesh, const std::string& albedo) {
  const std::string key = moho::Vfs::Normalize(mesh) + "|" + moho::Vfs::Normalize(albedo);
  auto it = looks_.find(key);
  if (it != looks_.end()) return it->second;
  Look& l = looks_[key];
  // the mesh file (.scm, version 5; 6 with indices of 32 bits): positions and the first texture coordinates
  const auto file = files.ReadFile(mesh);
  if (!file || file->size() < 48 || file->compare(0, 4, "MODL") != 0) return l;
  int32_t h[11];
  std::memcpy(h, file->data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  const int64_t indexBytes = h[0] == 6 ? 4 : 2;
  if ((h[0] != 5 && h[0] != 6) || vertexCount <= 0 || indexCount < 3 || vertexOffset < 0 || indexOffset < 0 ||
      vertexOffset + vertexCount * static_cast<int64_t>(sizeof(ScmVertex)) > static_cast<int64_t>(file->size()) ||
      indexOffset + indexCount * indexBytes > static_cast<int64_t>(file->size()))
    return l;
  l.vertices.resize(static_cast<size_t>(vertexCount) * 5);
  for (int64_t i = 0; i < vertexCount; ++i) {
    ScmVertex v;
    std::memcpy(&v, file->data() + vertexOffset + i * static_cast<int64_t>(sizeof v), sizeof v);
    float* o = &l.vertices[static_cast<size_t>(i) * 5];
    o[0] = v.position[0], o[1] = v.position[1], o[2] = v.position[2], o[3] = v.uv0[0], o[4] = v.uv0[1];
  }
  l.indices.resize(static_cast<size_t>(indexCount / 3 * 3));
  for (size_t i = 0; i < l.indices.size(); ++i) {
    uint32_t index = 0;
    std::memcpy(&index, file->data() + indexOffset + static_cast<int64_t>(i) * indexBytes, static_cast<size_t>(indexBytes));
    l.indices[i] = index < static_cast<uint64_t>(vertexCount) ? index : 0;
  }
  // the albedo's colour, and each half size of it (what the card samples a small stamp from); white when it cannot
  // be read here (formats only the card reads)
  std::vector<uint8_t> rgba;
  int w = 1, hgt = 1;
  const auto texture = files.ReadFile(albedo);
  DdsImage image;
  std::string error;
  if (texture && ParseDds(*texture, &image, &error) && DecodeRgba(image, 0, &rgba) && image.width > 0 && image.height > 0) {
    w = image.width;
    hgt = image.height;
  } else {
    rgba.assign(4, 255);
  }
  std::vector<uint8_t> level(static_cast<size_t>(w) * static_cast<size_t>(hgt) * 3);
  for (size_t i = 0; i < level.size() / 3; ++i)
    for (int k = 0; k < 3; ++k) level[i * 3 + k] = rgba[i * 4 + k];
  for (;;) {
    l.levels.push_back(level);
    l.levelWidth.push_back(w);
    l.levelHeight.push_back(hgt);
    if (w == 1 && hgt == 1) break;
    const int w2 = std::max(w / 2, 1), h2 = std::max(hgt / 2, 1);
    std::vector<uint8_t> half(static_cast<size_t>(w2) * static_cast<size_t>(h2) * 3);
    for (int y = 0; y < h2; ++y)
      for (int x = 0; x < w2; ++x)
        for (int k = 0; k < 3; ++k) {
          int sum = 0, n = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
              const int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, hgt - 1);
              sum += level[(static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 3 + static_cast<size_t>(k)];
              ++n;
            }
          half[(static_cast<size_t>(y) * static_cast<size_t>(w2) + static_cast<size_t>(x)) * 3 + static_cast<size_t>(k)] = static_cast<uint8_t>(sum / n);
        }
    level.swap(half);
    w = w2;
    hgt = h2;
  }
  l.ok = true;
  return l;
}

const MinimapStamps::Blueprint& MinimapStamps::BlueprintOf(TextureCache& files, const std::string& given) {
  const std::string path = moho::Vfs::Normalize(given);
  auto it = blueprints_.find(path);
  if (it != blueprints_.end()) return it->second;
  Blueprint& b = blueprints_[path];
  auto text = files.ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) return b;
  // a mesh blueprint itself, a blueprint with its Display.Mesh, or one that names its mesh blueprint (at its own scale)
  const BpValue* lods = nullptr;
  std::string from = path;  // (the file the levels' names are beside)
  BpValue named;
  if (!bp["Display"].IsTable() && bp["LODs"].IsTable()) {
    lods = &bp["LODs"];
    b.scale = static_cast<float>(bp["UniformScale"].NumberOr(1));
  } else if (bp["Display"]["Mesh"].IsTable()) {
    lods = &bp["Display"]["Mesh"]["LODs"];
    b.scale = static_cast<float>(bp["Display"]["UniformScale"].NumberOr(1));
  } else if (!bp["Display"]["MeshBlueprint"].StringOr(std::string()).empty()) {
    from = moho::Vfs::Normalize(bp["Display"]["MeshBlueprint"].string);
    if (from.size() < 3 || from.compare(from.size() - 3, 3, ".bp") != 0) from += ".bp";
    auto other = files.ReadFile(from);
    if (!other || !ReadBlueprintFile(*other, &named) || !named["LODs"].IsTable()) return b;
    lods = &named["LODs"];
    b.scale = static_cast<float>(bp["Display"]["UniformScale"].NumberOr(1));
  } else {
    return b;
  }
  // (names as ModelCache::ReadSources takes them: beside the blueprint unless they start at the root; the standard
  // ones "<name before the file's last _>_lod<n>.scm" and "..._albedo.dds")
  const size_t slash = from.rfind('/');
  const std::string dir = from.substr(0, slash + 1);
  std::string base = from.substr(slash + 1);
  const size_t under = base.rfind('_');
  base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);
  for (size_t i = 0; i < lods->items.size(); ++i) {
    const BpValue& entry = lods->Item(i);
    auto file = [&](const char* key, const std::string& standard) {
      const std::string given = entry[key].StringOr(std::string());
      if (given.empty()) return dir + base + standard;
      return given[0] == '/' ? given : dir + given;
    };
    Level level;
    level.cutoff = static_cast<float>(entry["LODCutoff"].NumberOr(0));
    level.cartographic = Cartographic(entry["ShaderName"].StringOr(std::string()));
    level.mesh = file("MeshName", "_lod" + std::to_string(i) + ".scm");
    level.albedo = file("AlbedoName", "_albedo.dds");
    b.levels.push_back(std::move(level));
  }
  b.ok = !b.levels.empty();
  return b;
}

bool MinimapStamps::StampBlueprint(TextureCache& files, const Blueprint& bp, const Mat4& place, Vec3 at, const View& view, uint32_t* pixels,
                                   int width, int height) {
  if (!bp.ok) return false;
  // the minimap camera's level of detail at its place, and the level it picks (Mesh::ComputeLOD: the first whose cutoff
  // it is within; the last one's reaches 20 further, as it fades; none: not drawn), left out once faded below 0.2
  const float lod = view.lodScale * view.widthPerDepth * Dot(at - view.eye, view.forward);
  const Level* chosen = nullptr;
  for (size_t i = 0; i < bp.levels.size() && !chosen; ++i) {
    const Level& l = bp.levels[i];
    const bool last = i + 1 == bp.levels.size();
    if (l.cutoff <= 0 || lod <= l.cutoff || (last && lod <= l.cutoff + 20)) chosen = &l;
  }
  if (!chosen || !chosen->cartographic) return false;
  if (chosen == &bp.levels.back() && chosen->cutoff > 0 && lod > chosen->cutoff && 1 - (lod - chosen->cutoff) / 20 < 0.2f) return false;
  const Look& look = LookOf(files, chosen->mesh, chosen->albedo);
  if (!look.ok) return false;
  Fill(look, place, view, pixels, width, height);
  return true;
}

void MinimapStamps::Fill(const Look& look, const Mat4& place, const View& view, uint32_t* pixels, int width, int height) {
  const size_t picture = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (topCall_.size() != picture) {
    top_.assign(picture, 0);
    topCall_.assign(picture, 0);
  }
  // its corners in the world and on the minimap
  const size_t count = look.vertices.size() / 5;
  std::vector<float>& at = corners_;  // world x, y, z; pixel x, y
  std::vector<uint8_t>& on = cornersOn_;
  at.resize(count * 5);
  on.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const float* v = &look.vertices[i * 5];
    const Vec3 w{place.m[0][0] * v[0] + place.m[0][1] * v[1] + place.m[0][2] * v[2] + place.m[0][3],
                 place.m[1][0] * v[0] + place.m[1][1] * v[1] + place.m[1][2] * v[2] + place.m[1][3],
                 place.m[2][0] * v[0] + place.m[2][1] * v[1] + place.m[2][2] * v[2] + place.m[2][3]};
    float* o = &at[i * 5];
    o[0] = w.x, o[1] = w.y, o[2] = w.z;
    // (on the minimap: as WorldPicture::MinimapProject)
    const Mat4& m = view.matrix;
    const float wq = m.m[3][0] * w.x + m.m[3][1] * w.y + m.m[3][2] * w.z + m.m[3][3];
    on[i] = wq > 1e-4f ? 1 : 0;
    if (!on[i]) continue;
    const float half = 0.5f / wq;
    o[3] = ((m.m[0][0] * w.x + m.m[0][1] * w.y + m.m[0][2] * w.z + m.m[0][3]) * half + 0.5f) * static_cast<float>(width);
    o[4] = (0.5f - (m.m[1][0] * w.x + m.m[1][1] * w.y + m.m[1][2] * w.z + m.m[1][3]) * half) * static_cast<float>(height);
  }
  const int levels = static_cast<int>(look.levels.size());
  for (size_t t = 0; t + 2 < look.indices.size(); t += 3) {
    const uint32_t ia = look.indices[t], ib = look.indices[t + 1], ic = look.indices[t + 2];
    if (!on[ia] || !on[ib] || !on[ic]) continue;
    const float* a = &at[ia * 5];
    const float* b = &at[ib * 5];
    const float* c = &at[ic * 5];
    // (the pixels whose middles its box holds: most of a small stamp's triangles hold none and are passed by first)
    const int x0 = std::max(static_cast<int>(std::ceil(std::min({a[3], b[3], c[3]}) - 0.5f)), 0);
    const int x1 = std::min(static_cast<int>(std::floor(std::max({a[3], b[3], c[3]}) - 0.5f)), width - 1);
    if (x0 > x1) continue;
    const int y0 = std::max(static_cast<int>(std::ceil(std::min({a[4], b[4], c[4]}) - 0.5f)), 0);
    const int y1 = std::min(static_cast<int>(std::floor(std::max({a[4], b[4], c[4]}) - 0.5f)), height - 1);
    if (y0 > y1) continue;
    const float area = (b[3] - a[3]) * (c[4] - a[4]) - (c[3] - a[3]) * (b[4] - a[4]);
    if (std::fabs(area) < 1e-9f) continue;
    // the albedo's level for it: as many of its texels to a pixel as its texture's area to its own
    const float* ua = &look.vertices[ia * 5 + 3];
    const float* ub = &look.vertices[ib * 5 + 3];
    const float* uc = &look.vertices[ic * 5 + 3];
    const float uvArea = std::fabs((ub[0] - ua[0]) * (uc[1] - ua[1]) - (uc[0] - ua[0]) * (ub[1] - ua[1]));
    const float texels = std::sqrt(uvArea * static_cast<float>(look.levelWidth[0]) * static_cast<float>(look.levelHeight[0]) / std::fabs(area));
    const int li = std::clamp(static_cast<int>(std::floor(std::log2(std::max(texels, 1.0f)))), 0, levels - 1);
    const std::vector<uint8_t>& level = look.levels[static_cast<size_t>(li)];
    const int lw = look.levelWidth[static_cast<size_t>(li)], lh = look.levelHeight[static_cast<size_t>(li)];
    for (int py = y0; py <= y1; ++py)
      for (int px = x0; px <= x1; ++px) {
        const float sx = static_cast<float>(px) + 0.5f, sy = static_cast<float>(py) + 0.5f;
        const float wa = ((b[3] - sx) * (c[4] - sy) - (c[3] - sx) * (b[4] - sy)) / area;
        const float wb = ((c[3] - sx) * (a[4] - sy) - (a[3] - sx) * (c[4] - sy)) / area;
        const float wc = 1 - wa - wb;
        if (wa < 0 || wb < 0 || wc < 0) continue;
        const float y = wa * a[1] + wb * b[1] + wc * c[1];
        if (y < view.water) continue;  // (cut below the water's surface)
        const size_t pixel = static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px);
        if (topCall_[pixel] == call_ && y <= top_[pixel]) continue;  // (what lies nearer the camera, from straight above: higher)
        const float x = wa * a[0] + wb * b[0] + wc * c[0], z = wa * a[2] + wb * b[2] + wc * c[2];
        if (y < view.ground(x, z) - 0.01f) continue;  // (hidden by the ground)
        top_[pixel] = y;
        topCall_[pixel] = call_;
        float u = wa * ua[0] + wb * ub[0] + wc * uc[0], v = wa * ua[1] + wb * ub[1] + wc * uc[1];
        u -= std::floor(u);
        v -= std::floor(v);
        const int tx = std::min(static_cast<int>(u * static_cast<float>(lw)), lw - 1), ty = std::min(static_cast<int>(v * static_cast<float>(lh)), lh - 1);
        const uint8_t* rgb = &level[(static_cast<size_t>(ty) * static_cast<size_t>(lw) + static_cast<size_t>(tx)) * 3];
        pixels[pixel] = 0xff000000u | static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
      }
  }
}

int MinimapStamps::Stamp(ModelCache& models, TextureCache& files, const Frame& frame, const std::vector<PlacedMesh>& placed, const View& view,
                         uint32_t* pixels, int width, int height) {
  if (!pixels || width <= 0 || height <= 0 || !view.ground) return 0;
  if (++call_ == 0) {  // (once in four billion calls: the marks of the old ones cleared)
    std::fill(topCall_.begin(), topCall_.end(), 0u);
    call_ = 1;
  }
  int stamped = 0;
  // the simulation's: by the frame's blueprint names (each read once; a unit's id is its blueprint file's, a script
  // entity's name its mesh blueprint's without ".bp", a projectile's its blueprint file)
  if (frame.blueprints) {
    if (names_ != frame.blueprints) {
      names_ = frame.blueprints;
      byName_.clear();
      named_.clear();
    }
    byName_.resize(frame.blueprints->size(), nullptr);
    named_.resize(frame.blueprints->size(), 0);
    enum Kind { Unit, Entity, Shot };
    auto of = [&](uint32_t index, Kind kind) -> const Blueprint* {
      if (index >= byName_.size()) return nullptr;
      if (!named_[index]) {
        named_[index] = 1;
        const std::string& name = (*frame.blueprints)[index];
        if (!name.empty()) {
          const Blueprint& bp = BlueprintOf(files, kind == Unit ? models.UnitBlueprintPath(name) : kind == Entity ? name + ".bp" : name);
          bool any = false;
          for (const Level& l : bp.levels) any = any || l.cartographic;
          if (any) byName_[index] = &bp;  // (only those that can be stamped are kept: the others are passed by at once)
        }
      }
      return byName_[index];
    };
    for (const FrameEntity& f : frame.entities) {
      if (!f.seen || !f.mesh) continue;
      const Blueprint* bp = of(f.mesh, Entity);
      if (!bp) continue;
      const Vec3 at{f.position[0], f.position[1], f.position[2]};
      Mat4 place = Mat4::FromTRS(at, Quat{f.orientation[0], f.orientation[1], f.orientation[2], f.orientation[3]}, bp->scale);
      for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 3; ++k) place.m[row][k] *= f.scale[k];
      stamped += StampBlueprint(files, *bp, place, at, view, pixels, width, height) ? 1 : 0;
    }
    for (const FrameUnit& u : frame.units) {
      if (u.dead || !u.Seen()) continue;
      const Blueprint* bp = of(u.blueprint, Unit);
      if (!bp) continue;
      const Vec3 at{u.position[0], u.position[1], u.position[2]};
      const Mat4 place = Mat4::FromTRS(at, Quat{u.orientation[0], u.orientation[1], u.orientation[2], u.orientation[3]}, bp->scale);
      stamped += StampBlueprint(files, *bp, place, at, view, pixels, width, height) ? 1 : 0;
    }
    for (const FrameProjectile& p : frame.projectiles) {
      if (!p.seen) continue;
      const Blueprint* bp = of(p.blueprint, Shot);
      if (!bp) continue;
      const Vec3 at{p.position[0], p.position[1], p.position[2]};
      Mat4 place = Mat4::FromTRS(at, Quat{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]}, p.sized ? 1.0f : bp->scale);
      for (int row = 0; row < 3 && p.sized; ++row)
        for (int k = 0; k < 3; ++k) place.m[row][k] *= std::max(p.size[k], 0.0f);
      stamped += StampBlueprint(files, *bp, place, at, view, pixels, width, height) ? 1 : 0;
    }
  }
  // the interface's: its one level's cutoff the description's, with no fade
  for (const PlacedMesh& p : placed) {
    if (p.hidden || !Cartographic(p.shader)) continue;
    const float lod = view.lodScale * view.widthPerDepth * Dot(p.position - view.eye, view.forward);
    if (p.lodCutoff > 0 && lod > p.lodCutoff) continue;
    const Look& look = LookOf(files, p.mesh, p.texture);
    if (!look.ok) continue;
    Mat4 place = Mat4::FromTRS(p.position, p.orientation, p.uniformScale);
    for (int row = 0; row < 3; ++row) {
      place.m[row][0] *= p.scale.x;
      place.m[row][1] *= p.scale.y;
      place.m[row][2] *= p.scale.z;
    }
    Fill(look, place, view, pixels, width, height);
    ++stamped;
  }
  return stamped;
}

void PingTestFrame(Vec3 around, double clock, const std::function<float(float, float)>& ground, Frame* frame,
                   std::vector<std::string>* names) {
  // (SimPing.lua's mesh names, "/meshes/game/ping_" .. data.Mesh; 0: none)
  static const char* const kMeshes[4] = {"/meshes/game/ping_alert_marker", "/meshes/game/ping_move", "/meshes/game/ping_attack_marker",
                                         "/meshes/game/ping_nuke_marker"};
  if (names->empty()) {
    names->push_back(std::string());
    names->insert(names->end(), std::begin(kMeshes), std::end(kMeshes));
    names->push_back("ueb5102");
  }
  frame->blueprints = names;
  frame->units.clear();
  frame->entities.clear();
  // (the script's step: every beat of a tenth of a second, each ping its own number of beats along)
  const double beat = std::floor(clock * 10);
  for (int k = 0; k < 4; ++k) {
    const float x = around.x + (static_cast<float>(k) - 1.5f) * 16, z = around.z;
    const float size = 0.4f + 0.2f * static_cast<float>(std::sin(0.3 * (beat + 5 * k)));
    FrameEntity e;
    e.id = 0x30000000u + static_cast<uint32_t>(k);
    e.mesh = static_cast<uint32_t>(1 + k);
    e.army = 0;
    e.position[0] = x, e.position[1] = ground(x, z) + 2, e.position[2] = z;
    e.scale[0] = e.scale[1] = e.scale[2] = size;
    frame->entities.push_back(e);
  }
  FrameUnit u;  // (the ferry beacon, in front of the row)
  u.id = 1;
  u.blueprint = 5;
  u.position[0] = around.x, u.position[2] = around.z + 14;
  u.position[1] = ground(u.position[0], u.position[2]);
  u.health = u.maxHealth = 10;
  u.layer = 1;
  frame->units.push_back(u);
}

}  // namespace render
