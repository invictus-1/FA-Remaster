#include "models.h"

#include <atomic>  // [bgload]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

#include "bp_reader.h"
#include "effect_files.h"
#include "core/vfs.h"
#include "hitch_log.h"  // [hitches]
#include "machinery.h"  // [machinery]
#include "unit_wear.h"

namespace render {
namespace {

// .scm: "MODL", then 11 ints (version 5 or 6, bone offset, weighted bone count, vertex offset, extra vertex
// offset, vertex count, index offset, index count, info offset, info count, total bone count);
// vertices of 68 bytes; indices of 16 bits, three per triangle. Each bone is 108 bytes: the rest
// pose's inverse (16 floats), position (3 floats) and rotation (w, x, y, z) relative to the parent,
// the offset of its name, the parent's index (-1: none), two unused ints.
//
// The bones of an .scm file, each composed with its parents as the simulation does it
// (src/sim/skeleton.cpp), so a pose the simulation hands over and this rest pose agree exactly.
void ReadScmBones(const std::string& file, std::vector<ModelBone>* bones) {
  bones->clear();
  if (file.size() < 48 || file.compare(0, 4, "MODL") != 0) return;
  int32_t h[11];
  std::memcpy(h, file.data() + 4, sizeof h);
  const int64_t offset = h[1], total = h[10];
  if (offset < 48 || total <= 0 || offset + total * 108 > static_cast<int64_t>(file.size())) return;
  bones->resize(static_cast<size_t>(total));
  for (int64_t i = 0; i < total; ++i) {
    const char* at = file.data() + offset + i * 108 + 64;
    float f[7];
    int32_t n[2];
    std::memcpy(f, at, sizeof f);
    std::memcpy(n, at + 28, sizeof n);
    ModelBone& b = (*bones)[static_cast<size_t>(i)];
    if (n[0] >= 0 && static_cast<size_t>(n[0]) < file.size()) b.name = file.c_str() + n[0];
    for (char& c : b.name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const Vec3 position{f[0], f[1], f[2]};
    const Quat rotation{f[4], f[5], f[6], f[3]};
    if (n[1] >= 0 && n[1] < i) {
      const ModelBone& parent = (*bones)[static_cast<size_t>(n[1])];
      b.parent = n[1];
      b.rotation = Mul(parent.rotation, rotation);
      b.position = parent.position + Rotate(parent.rotation, position);
    } else {
      b.rotation = rotation;
      b.position = position;
    }
  }
}

// [machinery] A remastered factory's parts (machinery.h): its finest level's own bones that the skeleton has not (by name)
// appended to the skeleton, their parents by name, so that every level's vertices on them keep them (LoadScm matches its
// file's bones to the skeleton's by name; those it does not find fall to the first). Bones already there are left.
void AppendOwnBones(const std::string& finest, std::vector<ModelBone>* bones) {
  std::vector<ModelBone> own;
  ReadScmBones(finest, &own);
  auto named = [&](const std::string& name) {
    for (size_t k = 0; k < bones->size(); ++k)
      if ((*bones)[k].name == name) return static_cast<int>(k);
    return -1;
  };
  for (const ModelBone& b : own) {
    if (named(b.name) >= 0 || bones->size() >= 256) continue;
    ModelBone extra = b;
    extra.parent = b.parent >= 0 ? named(own[static_cast<size_t>(b.parent)].name) : -1;
    bones->push_back(extra);
  }
}

// Trees of a group (ModelLod::treeStep), for a level of what sways: `bytes` its vertices as the file has them, `bones`
// the file's own bones. A tree is a bone that carries vertices, and it stands where that bone stands at rest (x, z;
// the same in every level of a group, whatever order or names a level lists its bones by). Only where two or more
// bones carry vertices and stand apart is the level a group; otherwise (a single tree, a group's far card) the bytes
// are cleared and the model's own beat holds.
void TagTrees(const std::vector<ModelBone>& bones, std::string* bytes, ModelLod* lod) {
  const size_t count = bytes->size() / sizeof(ScmVertex);
  auto tagOf = [&](size_t v) { return reinterpret_cast<uint8_t*>(&(*bytes)[v * sizeof(ScmVertex) + offsetof(ScmVertex, bones)]); };
  std::vector<char> carries(256, 0);
  for (size_t v = 0; v < count; ++v) carries[tagOf(v)[0]] = 1;
  float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f}, farthest = 0;
  int trees = 0;
  for (size_t b = 0; b < bones.size() && b < carries.size(); ++b) {
    if (!carries[b]) continue;
    ++trees;
    const float at[2] = {bones[b].position.x, bones[b].position.z};
    for (int k = 0; k < 2; ++k) {
      lo[k] = std::min(lo[k], at[k]);
      hi[k] = std::max(hi[k], at[k]);
      farthest = std::max(farthest, std::fabs(at[k]));
    }
  }
  const bool group = trees >= 2 && std::max(hi[0] - lo[0], hi[1] - lo[1]) > 1e-3f;
  lod->treeStep = group ? std::max(farthest, 1e-3f) / 127 : 0;
  for (size_t v = 0; v < count; ++v) {
    uint8_t* tag = tagOf(v);
    tag[1] = tag[2] = tag[3] = 0;
    if (!group || tag[0] >= bones.size()) continue;
    const float x = bones[tag[0]].position.x, z = bones[tag[0]].position.z;
    tag[1] = static_cast<uint8_t>(std::clamp(std::lround(x / lod->treeStep) + 128, 0L, 255L));
    tag[2] = static_cast<uint8_t>(std::clamp(std::lround(z / lod->treeStep) + 128, 0L, 255L));
    // its own beat, a share of a whole swing, from where it stands: smooth in the place (each level's bones stand
    // where the others' do, to a hair, and the beat must not jump when the level changes), yet unrelated between
    // trees a few model units apart
    const float s = std::sin(x * 0.13f + z * 0.29f) * 3.7f + x * 0.071f + z * 0.053f;
    tag[3] = static_cast<uint8_t>(1 + std::min(253, static_cast<int>((s - std::floor(s)) * 254)));
  }
}

// `skeleton`: the bones the model's vertices are to follow by index (Model::bones). Where the file
// lists its own bones in another order, its vertices are renumbered by the bones' names.
// `trees`: a level of what sways: its trees of a group are told apart first (TagTrees).
uint32_t NextLodSerial() {
  static std::atomic<uint32_t> made{0};  // [bgload] (worlds opened on threads of their own count on it too)
  return ++made;
}

// (debris, debris.h; Model::paintSpots) Where a mesh's paint lies on its texture: 24 of its triangles picked evenly by
// their area along the file's list (so spread over its parts), each its middle in the texture and how much texture a
// model unit of its surface takes there. Triangles that are flat in the texture or reach out of it are left out.
// [hull sit] (Model::deckHeight) The height of a model's top at the rim of its plan, in model units: over a grid of 32 x 32
// squares across its box seen from above, the highest of its corners, its triangles' middles and its edges' middles in
// each square, and of the squares it covers the lowest quarter's highest (a ship's deck at its sides, not its towers).
float DeckHeight(const char* vertices, int64_t vertexCount, const char* indices, int64_t indexCount, int indexBytes, Vec3 lo, Vec3 hi) {
  constexpr int kGrid = 32;
  std::vector<float> top(kGrid * kGrid, -1e30f);
  const float sx = kGrid / std::max(hi.x - lo.x, 1e-6f), sz = kGrid / std::max(hi.z - lo.z, 1e-6f);
  auto lay = [&](float x, float y, float z) {
    const int gx = std::clamp(static_cast<int>((x - lo.x) * sx), 0, kGrid - 1), gz = std::clamp(static_cast<int>((z - lo.z) * sz), 0, kGrid - 1);
    float& t = top[static_cast<size_t>(gz) * kGrid + gx];
    t = std::max(t, y);
  };
  auto at = [&](int64_t i, float p[3]) {
    int64_t v = 0;
    if (indexBytes == 4) {
      uint32_t k;
      std::memcpy(&k, indices + i * 4, 4);
      v = k;
    } else {
      uint16_t k;
      std::memcpy(&k, indices + i * 2, 2);
      v = k;
    }
    if (v >= vertexCount) return false;
    std::memcpy(p, vertices + v * static_cast<int64_t>(sizeof(ScmVertex)), 3 * sizeof(float));
    return true;
  };
  for (int64_t t = 0; t + 2 < indexCount; t += 3) {
    float a[3], b[3], c[3];
    if (!at(t, a) || !at(t + 1, b) || !at(t + 2, c)) continue;
    lay(a[0], a[1], a[2]);
    lay(b[0], b[1], b[2]);
    lay(c[0], c[1], c[2]);
    lay((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3);
    lay((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2);
    lay((b[0] + c[0]) / 2, (b[1] + c[1]) / 2, (b[2] + c[2]) / 2);
    lay((a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2);
  }
  std::vector<float> covered;
  for (float t : top)
    if (t > -1e29f) covered.push_back(t);
  if (covered.empty()) return 1e30f;
  const size_t quarter = covered.size() / 4;
  std::nth_element(covered.begin(), covered.begin() + static_cast<std::ptrdiff_t>(quarter), covered.end());
  return covered[quarter];
}

void MakePaintSpots(const char* vertices, int64_t vertexCount, const char* indices, int64_t indexCount, int indexBytes, std::vector<Vec3>* out) {
  out->clear();
  auto index = [&](int64_t i) -> int64_t {
    if (indexBytes == 4) {
      uint32_t v;
      std::memcpy(&v, indices + i * 4, 4);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, indices + i * 2, 2);
    return v;
  };
  struct Tri {
    float area, u, v, density;
  };
  std::vector<Tri> tris;
  double total = 0;
  for (int64_t t = 0; t + 2 < indexCount; t += 3) {
    ScmVertex c[3];
    bool ok = true;
    for (int k = 0; k < 3 && ok; ++k) {
      const int64_t i = index(t + k);
      ok = i >= 0 && i < vertexCount;
      if (ok) std::memcpy(&c[k], vertices + i * static_cast<int64_t>(sizeof(ScmVertex)), sizeof(ScmVertex));
    }
    if (!ok) continue;
    const Vec3 a{c[0].position[0], c[0].position[1], c[0].position[2]}, b{c[1].position[0], c[1].position[1], c[1].position[2]},
        d{c[2].position[0], c[2].position[1], c[2].position[2]};
    const float area = 0.5f * Length(Cross(b - a, d - a));
    const float du1 = c[1].uv0[0] - c[0].uv0[0], dv1 = c[1].uv0[1] - c[0].uv0[1], du2 = c[2].uv0[0] - c[0].uv0[0], dv2 = c[2].uv0[1] - c[0].uv0[1];
    const float texture = 0.5f * std::abs(du1 * dv2 - du2 * dv1);
    bool inside = true;
    for (const ScmVertex& v : c) inside = inside && v.uv0[0] >= -0.01f && v.uv0[0] <= 1.01f && v.uv0[1] >= -0.01f && v.uv0[1] <= 1.01f;
    if (area < 1e-8f || texture < 1e-9f || !inside) continue;
    tris.push_back({area, (c[0].uv0[0] + c[1].uv0[0] + c[2].uv0[0]) / 3, (c[0].uv0[1] + c[1].uv0[1] + c[2].uv0[1]) / 3, std::sqrt(texture / area)});
    total += area;
  }
  if (tris.empty() || total <= 0) return;
  constexpr int kSpots = 24;
  double reached = 0;
  size_t at = 0;
  for (int k = 0; k < kSpots; ++k) {
    const double want = total * (k + 0.5) / kSpots;
    while (at + 1 < tris.size() && reached + tris[at].area < want) reached += tris[at++].area;
    out->push_back({tris[at].u, tris[at].v, tris[at].density});
  }
}

}  // namespace

// [spikes] A mesh made ahead (ModelCache::MakeAhead, on the meshes' thread): what LoadScm made of its file with the inputs
// Build will give it, its two buffers prepared on the device (PrepareBuffer), finished when Build takes it (TakeScm).
struct ScmAhead {
  bool ok = false, missing = false;  // (missing: no such file)
  std::string error;
  ModelLod lod;  // (its counts and its trees' step; no buffers)
  Vec3 boundsMin, boundsMax;
  float deckHeight = 1e30f;  // [hull sit] (Model::deckHeight)
  std::vector<Model::BoneBox> boneBoxes;
  std::shared_ptr<const HitShape> hitShape;
  std::vector<Vec3> paintSpots;
  std::unique_ptr<gfx::Device::Prepared> vertices, indices;
  size_t bytes = 0;  // (what its prepared buffers hold)
};

namespace {

// `ahead` ([spikes]): the buffers prepared into it, not made (on any thread; Build finishes them, TakeScm)
bool LoadScm(gfx::Device* device, const std::string& file, const std::vector<ModelBone>& skeleton, ModelLod* lod, Vec3* boundsMin,
             Vec3* boundsMax, std::string* error, std::vector<Model::BoneBox>* boneBoxes = nullptr,
             std::shared_ptr<const HitShape>* hitShape = nullptr, int64_t hitShapeMost = 0, bool trees = false,
             std::vector<Vec3>* paintSpots = nullptr, ScmAhead* ahead = nullptr, float* deckHeight = nullptr) {
  auto fail = [&](const char* what) {
    *error = what;
    return false;
  };
  if (file.size() < 48 || file.compare(0, 4, "MODL") != 0) return fail("not an .scm file");
  int32_t h[11];
  std::memcpy(h, file.data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  // version 6 is this project's extension for models of more than 65,535 vertices: version 5 with
  // indices of 32 bits (render/REQUESTS.md, item 4)
  if (h[0] != 5 && h[0] != 6) return fail("unsupported .scm version");
  const int64_t indexBytes = h[0] == 6 ? 4 : 2;
  if (vertexCount <= 0 || indexCount <= 0 || vertexOffset < 0 || indexOffset < 0 ||
      vertexOffset + vertexCount * static_cast<int64_t>(sizeof(ScmVertex)) > static_cast<int64_t>(file.size()) ||
      indexOffset + indexCount * indexBytes > static_cast<int64_t>(file.size()))
    return fail("the .scm file is cut short");
  static_assert(sizeof(ScmVertex) == 68, "the file's vertex layout");

  const char* vertices = file.data() + vertexOffset;
  if (boundsMin) {
    *boundsMin = {1e30f, 1e30f, 1e30f};
    *boundsMax = {-1e30f, -1e30f, -1e30f};
    for (int64_t i = 0; i < vertexCount; ++i) {
      float p[3];
      std::memcpy(p, vertices + i * static_cast<int64_t>(sizeof(ScmVertex)), sizeof p);
      *boundsMin = {std::min(boundsMin->x, p[0]), std::min(boundsMin->y, p[1]), std::min(boundsMin->z, p[2])};
      *boundsMax = {std::max(boundsMax->x, p[0]), std::max(boundsMax->y, p[1]), std::max(boundsMax->z, p[2])};
    }
  }
  // which of the skeleton's bones each of the file's bones is; left alone when they are the same list
  std::vector<ModelBone> own;
  ReadScmBones(file, &own);
  std::string tagged;  // (trees of a group: by the file's own bones, before they are renumbered)
  if (trees) {
    tagged.assign(vertices, static_cast<size_t>(vertexCount) * sizeof(ScmVertex));
    TagTrees(own, &tagged, lod);
    vertices = tagged.data();
  }
  bool same = own.size() <= skeleton.size();
  for (size_t i = 0; same && i < own.size(); ++i) same = own[i].name == skeleton[i].name;
  std::string renumbered;
  if (!same && !skeleton.empty()) {
    std::vector<uint8_t> to(256, 0);
    for (size_t i = 0; i < own.size() && i < 256; ++i)
      for (size_t k = 0; k < skeleton.size() && k < 256; ++k)
        if (skeleton[k].name == own[i].name) {
          to[i] = static_cast<uint8_t>(k);
          break;
        }
    renumbered.assign(vertices, static_cast<size_t>(vertexCount) * sizeof(ScmVertex));
    for (int64_t i = 0; i < vertexCount; ++i) {
      char& bone = renumbered[static_cast<size_t>(i) * sizeof(ScmVertex) + offsetof(ScmVertex, bones)];
      bone = static_cast<char>(to[static_cast<uint8_t>(bone)]);
    }
    vertices = renumbered.data();
  }
  if (boneBoxes) {  // (battle scars, unit_wear.h: which bone's part of the model a hit lies on)
    boneBoxes->assign(std::max<size_t>(skeleton.size(), 1), Model::BoneBox{});
    for (int64_t i = 0; i < vertexCount; ++i) {
      ScmVertex v;
      std::memcpy(&v, vertices + i * static_cast<int64_t>(sizeof(ScmVertex)), sizeof v);
      Model::BoneBox& b = (*boneBoxes)[std::min<size_t>(v.bones[0], boneBoxes->size() - 1)];
      b.lo = {std::min(b.lo.x, v.position[0]), std::min(b.lo.y, v.position[1]), std::min(b.lo.z, v.position[2])};
      b.hi = {std::max(b.hi.x, v.position[0]), std::max(b.hi.y, v.position[1]), std::max(b.hi.z, v.position[2])};
    }
  }
  if (hitShape && indexCount / 3 <= hitShapeMost)  // (battle scars, unit_wear.h: where on the model a hit lies)
    *hitShape = MakeHitShape(vertices, vertexCount, file.data() + indexOffset, indexCount, static_cast<int>(indexBytes), std::max<size_t>(skeleton.size(), 1));
  if (deckHeight && boundsMin)  // [hull sit] (a ship's deck at its sides: how deep it may sit in the lifted sea, World::RideWaves)
    *deckHeight = DeckHeight(vertices, vertexCount, file.data() + indexOffset, indexCount, static_cast<int>(indexBytes), *boundsMin, *boundsMax);
  if (paintSpots)  // (debris, debris.h: where its paint lies on its texture)
    MakePaintSpots(vertices, vertexCount, file.data() + indexOffset, indexCount, static_cast<int>(indexBytes), paintSpots);
  gfx::BufferDesc vb;
  vb.stride = sizeof(ScmVertex);
  vb.size = static_cast<uint32_t>(vertexCount * static_cast<int64_t>(sizeof(ScmVertex)));
  vb.data = vertices;
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = static_cast<uint32_t>(indexBytes);
  ib.size = static_cast<uint32_t>(indexCount * indexBytes);
  ib.data = file.data() + indexOffset;
  lod->indexCount = static_cast<uint32_t>(indexCount);
  lod->vertexCount = static_cast<uint32_t>(vertexCount);
  lod->indexBytes = static_cast<uint32_t>(indexBytes);
  if (ahead) {  // [spikes] (made ahead: the buffers prepared, here, off the frames' thread)
    ahead->vertices = device->PrepareBuffer(vb);
    ahead->indices = device->PrepareBuffer(ib);
    ahead->bytes = static_cast<size_t>(vb.size) + ib.size;
    if (!ahead->vertices || !ahead->indices) return fail("the device refused the mesh");
    return true;
  }
  lod->vertices = device->CreateBuffer(vb);
  lod->indices = device->CreateBuffer(ib);
  if (!lod->vertices || !lod->indices) return fail("the device refused the mesh");
  return true;
}

// [spikes] Build's shader name of a level as the kind it is drawn as (Build, and the meshes made ahead): the first game's
// names, which many blueprints still carry, as today's techniques (the original program's own table, docs/engine-answers.md,
// render 17; no name at all is "Unit"), and a stream's water and white water as LabPBR and LabPBRAlpha (`stream` 1 or 2).
std::string DrawnKind(std::string kind, int* stream) {
  static const char* const kOld[][2] = {
      {"tmeshnolighting", "flat"}, {"tmeshnonormals", "vertexnormal"}, {"tmeshalpha", "normalmappedalpha"},
      {"tmeshglow", "normalmappedglow"}, {"tmeshterrain", "normalmappedterrain"}, {"simple", "unit"},
      {"team", "unit"}, {"tmeshalphaglowfade", "unitbuild"}, {"tmeshmetalbuild", "aeonbuild"},
      {"tmeshshield", "shield"}, {"tmeshzfill", "shieldfill"}, {"tmeshadd", "effect"},
      {"tmeshexplosion", "explosion"}, {"tmeshcloud", "cloud"}, {"tmeshoutercloud", "outercloud"},
      {"tmeshempnuke", "nukeemp"}, {"tmeshquantumnuke", "nukequantum"}, {"tmeshtemporalbubble", "temporalbubble"}};
  for (const auto& old : kOld)
    if (kind == old[0]) {
      kind = old[1];
      break;
    }
  if (kind.empty()) kind = "unit";
  *stream = 0;
  if (kind == "streamwater") *stream = 1, kind = "labpbr";
  else if (kind == "streamfoam") *stream = 2, kind = "labpbralpha";
  return kind;
}

// [spikes] (the inputs a level's LoadScm is given, as one key: what a mesh made ahead is taken by)
std::string ScmKey(const std::string& mesh, const std::vector<ModelBone>& skeleton, bool first, bool shape, int64_t most, bool trees, bool spots) {
  std::string key = mesh;
  key += '|';
  for (const ModelBone& b : skeleton) key += b.name + ',';
  key += '|' + std::to_string(first) + std::to_string(shape) + std::to_string(trees) + std::to_string(spots) + '|' + std::to_string(most);
  return key;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

int ModelCache::LoadDisplayTable(const std::string& hostPath) {
  std::ifstream in(hostPath);
  std::string line;
  int count = 0;
  while (std::getline(in, line)) {
    const size_t at = line.find("DISPLAY\t");
    if (at == std::string::npos) continue;
    std::vector<std::string> f;
    for (size_t p = at + 8;;) {
      const size_t e = line.find('\t', p);
      f.push_back(line.substr(p, e == std::string::npos ? std::string::npos : e - p));
      if (e == std::string::npos) break;
      p = e + 1;
    }
    if (f.size() < 11) continue;
    while (!f.back().empty() && (f.back().back() == '\r' || f.back().back() == ' ')) f.back().pop_back();
    auto name = [](const std::string& v) { return v == "-" ? std::string() : v; };
    DisplayEntry& entry = display_[Lower(f[0])];
    entry.scale = static_cast<float>(atof(f[10].c_str()));
    LodSource lod;
    lod.cutoff = f[2] == "-" ? 1e9f : static_cast<float>(atof(f[2].c_str()));
    lod.shader = f[3] == "-" ? "Unit" : f[3];
    lod.mesh = name(f[4]);
    lod.albedo = name(f[5]);
    lod.normals = name(f[6]);
    lod.specular = name(f[7]);
    lod.secondary = name(f[8]);
    entry.lods.push_back(std::move(lod));
    ++count;
  }
  return count;
}

void ModelCache::SetLook(const BlueprintLook& look) {
  if (look.levels.empty()) return;
  // a unit by its id; a projectile or a prop by its blueprint file's path
  DisplayEntry& entry = display_[look.kind == 0 ? Lower(look.id) : moho::Vfs::Normalize(look.id)];
  entry.scale = look.scale;
  entry.lods.clear();
  for (const BlueprintLook::Level& l : look.levels) {
    LodSource s;
    s.cutoff = l.cutoff > 0 ? l.cutoff : 1e9f;
    s.scrolling = l.scrolling;
    s.shader = l.shader.empty() ? "Unit" : l.shader;
    s.mesh = l.mesh;
    s.albedo = l.albedo;
    s.normals = l.normals;
    s.specular = l.specular;
    s.secondary = l.secondary;
    entry.lods.push_back(std::move(s));
  }
}

const Model* ModelCache::Unit(const std::string& blueprintId, bool original) {
  std::string id = Lower(blueprintId);
  const size_t before = models_.size();  // [hitches] (a model made now: what its kind upgrades to read ahead)
  const Model* m = nullptr;
  if (!original) {
    auto listed = display_.find(id);
    if (listed != display_.end()) m = FromDisplay(id, id, listed->second, std::string());
  }
  if (!m) m = Blueprint(UnitBlueprintPath(id), original);
  if (readAhead && !original && !makingAhead_ && models_.size() != before) ReadAheadUpgrade(id);  // [hitches]
  return m;
}

const std::string& ModelCache::UnitBlueprintPath(const std::string& blueprintId) {
  const std::string id = Lower(blueprintId);
  auto known = unitPaths_.find(id);
  if (known != unitPaths_.end()) return known->second;
  // the game's own units are in /units; a mod's are in its own folder under /mods
  std::string path = "/units/" + id + "/" + id + "_unit.bp";
  if (!vfs_->Exists(path)) {
    if (!modUnitsListed_) {
      modUnitsListed_ = true;
      for (std::string& found : vfs_->FindFiles("/mods", "*_unit.bp")) {
        const std::string name = Lower(found.substr(found.rfind('/') + 1));
        modUnits_.emplace(name, std::move(found));  // (sorted: the first of a name is kept)
      }
    }
    auto mod = modUnits_.find(id + "_unit.bp");
    if (mod != modUnits_.end()) path = mod->second;
  }
  return unitPaths_.emplace(id, path).first->second;
}

const Model* ModelCache::RemasteredWreck(const std::string& blueprintId) {
  const std::string id = Lower(blueprintId);
  const std::string name = "wreck:" + id;
  auto it = models_.find(name);
  if (it != models_.end()) return it->second.get();
  const std::string first = "/preview/" + id + "/" + id + "_wreck_lod0.scm";
  if (!vfs_->Exists(first)) {
    models_.emplace(name, nullptr);  // (none: asked once)
    return nullptr;
  }
  // the unit's levels as its blueprint lists them (their distances), the meshes and textures then the wreck's own
  std::vector<LodSource> sources;
  float scale = 1;
  std::string error;
  if (!ReadSources(UnitBlueprintPath(id), &sources, &scale, &error) || sources.empty()) sources.assign(1, LodSource{});
  sources[0].mesh = first;  // (its skeleton: the wreck's own, every vertex on its first bone)
  Model* m = models_.emplace(name, std::make_unique<Model>()).first->second.get();
  Build(m, name, id, sources, false, false, std::string(), false, id + "_wreck");
  m->scale = 1;  // (its prop gives its size: the unit's UniformScale)
  m->lights.clear();
  for (ModelLod& lod : m->lods) lod.sway = 0;
  return m;
}

bool ModelCache::Remastered(const std::string& blueprintId) const {
  const std::string id = Lower(blueprintId);
  return vfs_->Exists("/preview/" + id + "/" + id + "_lod0.scm") || vfs_->Exists("/mods/labvisuals/textures/" + id + "/" + id + "_lab_albedo.dds");
}

const Model* ModelCache::FromDisplay(const std::string& key, const std::string& base, const DisplayEntry& entry, const std::string& faction,
                                     bool effect) {
  const std::string name = "display:" + key + (faction.empty() ? std::string() : "#" + faction) + (effect ? "#effect" : "");
  auto it = models_.find(name);
  if (it != models_.end()) return it->second.get();
  Model* m = models_.emplace(name, std::make_unique<Model>()).first->second.get();
  m->scale = entry.scale > 0 ? entry.scale : 1;
  // [spikes] A prop's look as the simulation has it that names the very files its blueprint file does (a map's props once
  // the game has begun: every kind of them had its model made from its .bp file as the map opened, and was made again
  // from the simulation's looks in the game's first frames, a hundred models of the map in one frame): the model made
  // from the file taken as it is (what Build makes of the same files: the same buffers and textures), its levels numbered
  // anew as Build would number them, the rest as this way makes it (no simTree, as before).
  if (!effect && faction.empty() && !key.empty() && key[0] == '/') {
    auto from = builtFrom_.find(key);
    auto made = models_.find(key);
    if (from != builtFrom_.end() && made != models_.end() && made->second && made->second->ok && from->second.scale == m->scale &&
        made->second->lods.size() == entry.lods.size() && SameSources(from->second.sources, entry.lods)) {
      *m = *made->second;
      m->simTree = false;
      // (its levels numbered anew, as Build numbers them: the model renderer draws a level's instances together by the
      // level itself, sorted by these numbers, so two copies under one number would be drawn one instance at a time)
      for (size_t i = 0; i < m->lods.size(); ++i) {
        m->lods[i].serial = NextLodSerial();
        m->lods[i].cutoff = entry.lods[i].cutoff;  // (the simulation's distances, often its own: FAF sets the props'; not drawn by)
      }
      ++madeAlike;
      if (HitchLog::on) HitchLog::Add("same model as its file's: " + key, 0);
      return m;
    }
  }
  Build(m, key, base, entry.lods, false, true, faction, effect);
  return m;
}

// [spikes] (FromDisplay) the levels name the same files, shaders and treads (paths as the file system takes them; their
// distances may differ, which Build only keeps: ModelLod::cutoff)
bool ModelCache::SameSources(const std::vector<LodSource>& a, const std::vector<LodSource>& b) {
  if (a.size() != b.size()) return false;
  auto same = [](const std::string& x, const std::string& y) { return x == y || moho::Vfs::Normalize(x) == moho::Vfs::Normalize(y); };
  for (size_t i = 0; i < a.size(); ++i) {
    const LodSource &x = a[i], &y = b[i];
    if (x.scrolling != y.scrolling || x.shader != y.shader || !same(x.mesh, y.mesh) || !same(x.albedo, y.albedo) ||
        !same(x.normals, y.normals) || !same(x.specular, y.specular) || !same(x.secondary, y.secondary))
      return false;
  }
  return true;
}

const Model* ModelCache::Mesh(const std::string& meshPath, const std::string& albedoPath, const std::string& shader, float uniformScale) {
  const std::string mesh = moho::Vfs::Normalize(meshPath);
  const std::string key = "mesh:" + mesh + "|" + albedoPath + "|" + shader + "|" + std::to_string(uniformScale);
  auto it = models_.find("display:" + key);
  if (it != models_.end()) return it->second.get();
  DisplayEntry entry;
  entry.scale = uniformScale;
  LodSource lod;
  lod.shader = shader;
  lod.mesh = mesh;
  if (!albedoPath.empty()) {
    lod.albedo = moho::Vfs::Normalize(albedoPath);
    lod.albedoNamed = true;
  }
  entry.lods.push_back(std::move(lod));
  const size_t slash = mesh.rfind('/');
  std::string base = mesh.substr(slash == std::string::npos ? 0 : slash + 1);
  base = base.substr(0, base.rfind('.'));
  return FromDisplay(key, base, entry, std::string());
}

const Model* ModelCache::Wreck(const Model* of, bool ownScale) {
  if (!of || !of->ok) return of;
  auto& kept = wrecks_[ownScale ? 1 : 0];
  auto it = kept.find(of);
  if (it != kept.end()) return it->second.get();
  auto wreck = std::make_unique<Model>(*of);  // (the same buffers and textures)
  if (!ownScale) wreck->scale = 1;
  wreck->lights.clear();
  for (ModelLod& lod : wreck->lods) {
    lod.serial = NextLodSerial();
    lod.wreck = true;
    lod.sway = 0;  // (a burnt tree does not wave)
  }
  return kept.emplace(of, std::move(wreck)).first->second.get();
}

const PersonalShieldLook& ModelCache::PersonalShield(const std::string& blueprintId) {
  const std::string id = Lower(blueprintId);
  auto known = personalShields_.find(id);
  if (known != personalShields_.end()) return known->second;
  PersonalShieldLook& look = personalShields_[id];
  const std::string& path = UnitBlueprintPath(id);
  auto text = path.empty() ? std::optional<std::string>() : vfs_->ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) return look;
  const BpValue& own = bp["Display"]["PersonalShield"];
  if (!own.IsTable()) return look;
  look.any = true;
  look.hitOnly = own["HitOnly"].kind == BpValue::Bool && own["HitOnly"].boolean;
  look.capsule = Lower(own["Shell"].StringOr(std::string())) == "capsule";
  const std::string lattice = own["Lattice"].StringOr(std::string());
  if (!lattice.empty() && vfs_->Exists(lattice)) look.lattice = textures_->Get(lattice);
  const BpValue& size = own["ShellSize"];
  if (size.IsTable() && size.items.size() >= 3) {
    look.sized = true;
    for (int k = 0; k < 3; ++k) look.size[k] = static_cast<float>(size.Item(static_cast<size_t>(k)).NumberOr(0));
    look.sized = look.size[0] > 0 && look.size[1] > 0 && look.size[2] > 0;
  }
  const BpValue& centre = own["ShellCentre"];
  if (centre.IsTable() && centre.items.size() >= 3) {
    look.centred = true;
    for (int k = 0; k < 3; ++k) look.centre[k] = static_cast<float>(centre.Item(static_cast<size_t>(k)).NumberOr(0));
  }
  return look;
}

const std::string& ModelCache::Faction(const std::string& blueprintId) {  // [wakes]
  const std::string id = Lower(blueprintId);
  auto known = factions_.find(id);
  if (known != factions_.end()) return known->second;
  std::string& faction = factions_[id];
  faction = UnitFaction(id);  // (the game's own units: by the id, as always)
  if (!faction.empty()) return faction;
  const std::string& path = UnitBlueprintPath(id);
  auto text = path.empty() ? std::optional<std::string>() : vfs_->ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) return faction;
  auto named = [](const std::string& name) {
    const std::string n = Lower(name);
    return n == "uef" || n == "aeon" || n == "cybran" || n == "seraphim" ? n : std::string();
  };
  faction = named(bp["General"]["FactionName"].StringOr(std::string()));
  for (size_t k = 0; faction.empty() && k < bp["Categories"].items.size(); ++k) faction = named(bp["Categories"].Item(k).StringOr(std::string()));
  return faction;
}

Vec3 OnShell(const Model& shell, Vec3 rest) {
  const Model::ShellShape& s = shell.shellShape;
  const float scale = shell.scale > 0 ? shell.scale : 1;
  const float p[3] = {rest.x / scale, rest.y / scale, rest.z / scale};
  const float t = std::clamp(p[s.along] - s.centre[s.along], -s.half, s.half);
  const float into = std::fabs(t) - (s.half - s.cap);
  const float section = into <= 0 ? 1.0f : std::sqrt(std::max(0.0f, 1 - (into / s.cap) * (into / s.cap)));
  const float dx = p[s.acrossA] - s.centre[s.acrossA], dy = p[s.acrossB] - s.centre[s.acrossB];
  float out[3];
  out[s.along] = s.centre[s.along] + t;
  out[s.acrossA] = s.centre[s.acrossA];
  out[s.acrossB] = s.centre[s.acrossB];
  const float a = s.a * section, b = s.b * section;
  if (a > 1e-4f && b > 1e-4f && (std::fabs(dx) > 1e-4f || std::fabs(dy) > 1e-4f)) {
    // (the superellipse's |x/a|^3.2 + |y/b|^3.2 = 1, met along the point's way from the axis)
    const float k = std::pow(std::pow(std::fabs(dx) / a, 3.2f) + std::pow(std::fabs(dy) / b, 3.2f), -1 / 3.2f);
    out[s.acrossA] += dx * k;
    out[s.acrossB] += dy * k;
  }
  return Vec3{out[0] * scale, out[1] * scale, out[2] * scale};
}

const Model* ModelCache::ShieldShell(const Model* of, const PersonalShieldLook& look) {
  auto known = shells_.find(of);
  if (known != shells_.end()) return known->second.get();
  auto m = std::make_unique<Model>();
  const gfx::Texture lattice = look.lattice;
  const Vec3 lo = of->boundsMin, hi = of->boundsMax;
  float c[3] = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
  const float h[3] = {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
  if (look.centred)
    for (int k = 0; k < 3; ++k) c[k] = look.centre[k];
  // (its long axis the model's longest side, or z with the blueprint's size; the other two across it)
  const int along = look.sized ? 2 : h[2] >= h[0] && h[2] >= h[1] ? 2 : h[0] >= h[1] ? 0 : 1;
  const int acrossA = along == 0 ? 1 : 0, acrossB = along == 2 ? 1 : 2;
  const float margin = 0.22f * std::min(h[acrossA], h[acrossB]);
  const float a = look.sized ? look.size[0] : h[acrossA] + margin, b = look.sized ? look.size[1] : h[acrossB] + margin;
  const float half = look.sized ? look.size[2] * 0.5f : h[along] + margin;
  const float cap = std::min(std::max(a, b), half);  // (how long each rounded end is)
  constexpr int kRings = 64, kAround = 72;
  constexpr float kPower = 3.2f;
  std::vector<ScmVertex> vertices;
  vertices.reserve(static_cast<size_t>(kRings) * kAround);
  auto at = [&](int ring, int k) {
    // along: the ends' rings closer together, where the shape turns
    const float u = static_cast<float>(ring) / (kRings - 1) * 2 - 1;  // -1 .. 1
    const float t = std::copysign(std::pow(std::fabs(u), 0.85f), u) * half;
    const float into = std::fabs(t) - (half - cap);
    const float s = into <= 0 ? 1.0f : std::sqrt(std::max(0.0f, 1 - (into / cap) * (into / cap)));
    const float angle = static_cast<float>(k) / kAround * 6.2831853f;
    const float ca = std::cos(angle), sa = std::sin(angle);
    float p[3];
    p[along] = c[along] + t;
    p[acrossA] = c[acrossA] + s * a * std::copysign(std::pow(std::fabs(ca), 2 / kPower), ca);
    p[acrossB] = c[acrossB] + s * b * std::copysign(std::pow(std::fabs(sa), 2 / kPower), sa);
    return Vec3{p[0], p[1], p[2]};
  };
  for (int ring = 0; ring < kRings; ++ring)
    for (int k = 0; k < kAround; ++k) {
      const Vec3 p = at(ring, k);
      // its way out: from its neighbours along and around (at the very ends, along the axis)
      const Vec3 du = at(std::min(ring + 1, kRings - 1), k) - at(std::max(ring - 1, 0), k);
      const Vec3 dv = at(ring, (k + 1) % kAround) - at(ring, (k + kAround - 1) % kAround);
      Vec3 n = Cross(dv, du);
      float axis[3] = {0, 0, 0};
      axis[along] = ring < kRings / 2 ? -1.0f : 1.0f;
      if (Length(n) < 1e-6f) n = {axis[0], axis[1], axis[2]};
      n = Normalize(n);
      float centre[3] = {c[0], c[1], c[2]};
      centre[along] = along == 0 ? p.x : along == 1 ? p.y : p.z;  // (the axis's point level with it)
      if (Dot(n, p - Vec3{centre[0], centre[1], centre[2]}) < 0 && Length(p - Vec3{centre[0], centre[1], centre[2]}) > 1e-4f) n = n * -1.0f;
      const Vec3 tangent = Normalize(du.x == 0 && du.y == 0 && du.z == 0 ? Vec3{axis[0], axis[1], axis[2]} : du);
      const Vec3 binormal = Cross(n, tangent);
      ScmVertex v{};
      v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
      v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
      v.tangent[0] = tangent.x, v.tangent[1] = tangent.y, v.tangent[2] = tangent.z;
      v.binormal[0] = binormal.x, v.binormal[1] = binormal.y, v.binormal[2] = binormal.z;
      v.uv0[0] = static_cast<float>(k) / kAround;
      v.uv0[1] = static_cast<float>(ring) / (kRings - 1);
      vertices.push_back(v);
    }
  std::vector<uint32_t> indices;
  indices.reserve(static_cast<size_t>(kRings - 1) * kAround * 6);
  for (int ring = 0; ring + 1 < kRings; ++ring)
    for (int k = 0; k < kAround; ++k) {
      const uint32_t i0 = static_cast<uint32_t>(ring * kAround + k), i1 = static_cast<uint32_t>(ring * kAround + (k + 1) % kAround);
      const uint32_t i2 = i0 + kAround, i3 = i1 + kAround;
      for (uint32_t i : {i0, i2, i1, i1, i2, i3}) indices.push_back(i);
    }
  ModelLod lod;
  lod.serial = NextLodSerial();
  gfx::BufferDesc vb;
  vb.stride = sizeof(ScmVertex);
  vb.size = static_cast<uint32_t>(vertices.size() * sizeof(ScmVertex));
  vb.data = vertices.data();
  lod.vertices = device_->CreateBuffer(vb);
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = 4;
  ib.size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
  ib.data = indices.data();
  lod.indices = device_->CreateBuffer(ib);
  lod.indexCount = static_cast<uint32_t>(indices.size());
  lod.vertexCount = static_cast<uint32_t>(vertices.size());
  lod.indexBytes = 4;
  lod.albedo = lattice;
  lod.normals = textures_->White();
  lod.specular = textures_->White();
  lod.shaderName = "PersonalShieldShell";
  lod.shell = lod.glass = lod.blend = true;
  lod.teamMask = false;
  lod.noShadow = true;
  m->lods.push_back(lod);
  m->scale = of->scale;
  {  // (its box: the capsule's extent along each of the model's axes)
    float e[3];
    e[along] = half;
    e[acrossA] = a;
    e[acrossB] = b;
    m->boundsMin = {c[0] - e[0], c[1] - e[1], c[2] - e[2]};
    m->boundsMax = {c[0] + e[0], c[1] + e[1], c[2] + e[2]};
  }
  m->shellShape.along = along;
  m->shellShape.acrossA = acrossA;
  m->shellShape.acrossB = acrossB;
  for (int k = 0; k < 3; ++k) m->shellShape.centre[k] = c[k];
  m->shellShape.a = a;
  m->shellShape.b = b;
  m->shellShape.half = half;
  m->shellShape.cap = cap;
  m->ok = static_cast<bool>(lod.vertices) && static_cast<bool>(lod.indices);
  return shells_.emplace(of, std::move(m)).first->second.get();
}

const Model* ModelCache::Blueprint(const std::string& blueprintPath, bool original, const std::string& faction, bool effect) {
  std::string path = moho::Vfs::Normalize(blueprintPath);
  // "/units/uel0201/uel0201_unit.bp": the files beside it are named after the blueprint file
  const size_t slash = path.rfind('/');
  const std::string dir = path.substr(0, slash + 1);
  std::string base = path.substr(slash + 1);
  const size_t under = base.rfind('_');
  base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);
  if (!original) {
    auto listed = display_.find(path);
    if (listed != display_.end()) return FromDisplay(path, base, listed->second, faction, effect);
  }
  const std::string key = original ? path + "#original" : (faction.empty() ? path : path + "#" + faction) + (effect ? "#effect" : "");
  auto it = models_.find(key);
  if (it != models_.end()) return it->second.get();
  Model* m = models_.emplace(key, std::make_unique<Model>()).first->second.get();
  std::vector<LodSource> sources;
  std::string error;
  std::string named;
  if (!ReadSources(path, &sources, &m->scale, &error, &named)) {
    if (!error.empty()) fprintf(stderr, "render: model %s: %s\n", path.c_str(), error.c_str());
    // (a blueprint that names a mesh blueprint instead of holding its mesh: the debris a dying unit
    // throws, "/meshes/projectiles/debris_misc_04_mesh"; that one's levels, at this blueprint's scale)
    if (!named.empty()) {
      named = moho::Vfs::Normalize(named);
      if (named.size() < 3 || named.compare(named.size() - 3, 3, ".bp") != 0) named += ".bp";
      if (named != path && vfs_->Exists(named)) {
        const float scale = m->scale;
        const Model* of = Blueprint(named, original, faction, effect);
        if (of->ok) {
          *m = *of;  // (the same buffers and textures)
          m->scale = scale;
        }
      }
    }
    return m;
  }
  Build(m, path, base, sources, original, false, faction, effect);
  // [spikes] (what it was made from, for the simulation's look of it that names the same: FromDisplay. Not where the lab's
  // LabVisuals textures stand in for the finest level's, which a look from the simulation does not take)
  if (!original && faction.empty() && !effect && m->ok && !vfs_->Exists("/mods/labvisuals/textures/" + base + "/" + base + "_lab_albedo.dds"))
    builtFrom_[path] = {sources, m->scale};
  // [plants] (the game's trees: Model::simTree)
  if (auto text = vfs_->ReadFile(path)) {
    BpValue bp;
    if (ReadBlueprintFile(*text, &bp)) {
      const std::string kind = bp["ScriptClass"].StringOr(std::string());
      m->simTree = kind == "Tree" || kind == "TreeGroup";
    }
  }
  return m;
}

bool ModelCache::ReadSources(const std::string& path, std::vector<LodSource>* sources, float* scale, std::string* error,
                             std::string* meshBlueprint) {
  const size_t slash = path.rfind('/');
  const std::string dir = path.substr(0, slash + 1);
  std::string base = path.substr(slash + 1);
  const size_t under = base.rfind('_');
  base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);
  auto text = vfs_->ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) {
    *error = text ? "the blueprint cannot be read" : "no such blueprint";
    return false;
  }
  const BpValue& display = bp["Display"];
  const bool meshFile = !display.IsTable() && bp["LODs"].IsTable();  // a mesh blueprint itself
  if (!meshFile && !display["Mesh"].IsTable()) {
    // (nothing to draw here, as many projectiles are effects only; but it may name a mesh blueprint
    // instead of holding its mesh, at its own scale: Blueprint takes that one's)
    if (meshBlueprint) *meshBlueprint = display["MeshBlueprint"].StringOr(std::string());
    *scale = static_cast<float>(display["UniformScale"].NumberOr(1));
    return false;
  }
  const BpValue& lods = meshFile ? bp["LODs"] : display["Mesh"]["LODs"];
  *scale = static_cast<float>((meshFile ? bp : display)["UniformScale"].NumberOr(1));
  const size_t count = std::max<size_t>(1, lods.items.size());
  for (size_t i = 0; i < count; ++i) {
    const BpValue& entry = lods.Item(i);
    // a name in the blueprint is beside the blueprint unless it starts at the root; a texture the
    // blueprint does not name has a standard name and may well not be there
    auto file = [&](const char* k, const std::string& standard, bool* named) {
      const std::string given = entry[k].StringOr(std::string());
      if (named) *named = !given.empty();
      if (given.empty()) return standard.empty() ? std::string() : dir + base + standard;
      return given[0] == '/' ? given : dir + given;
    };
    LodSource s;
    s.cutoff = static_cast<float>(entry["LODCutoff"].NumberOr(1e9));
    s.scrolling = entry["Scrolling"].kind == BpValue::Bool && entry["Scrolling"].boolean;
    s.shader = entry["ShaderName"].StringOr("Unit");
    s.mesh = file("MeshName", "_lod" + std::to_string(i) + ".scm", nullptr);
    s.albedo = file("AlbedoName", "_albedo.dds", &s.albedoNamed);
    s.normals = file("NormalsName", "_normalsts.dds", &s.normalsNamed);
    s.specular = file("SpecularName", "_specteam.dds", &s.specularNamed);
    s.secondary = file("SecondaryName", "", &s.secondaryNamed);
    sources->push_back(std::move(s));
  }
  return true;
}

void ModelCache::Prefetch(const std::vector<std::string>& unitIds) {
  std::vector<std::string> files;
  for (const std::string& unitId : unitIds) {
    const std::vector<std::string> own = UnitFiles(unitId);  // [hitches] (the list, as before, in UnitFiles)
    files.insert(files.end(), own.begin(), own.end());
  }
  textures_->Fetch(files);
}

// (Prefetch's list of one unit's files; none when its model is made already)
std::vector<std::string> ModelCache::UnitFiles(const std::string& unitId) {
  std::vector<std::string> files;
  const std::string id = Lower(unitId);
  std::vector<LodSource> sources;
  auto listed = display_.find(id);
  if (listed != display_.end()) {
    if (models_.count("display:" + id)) return files;
    sources = listed->second.lods;
  } else {
    const std::string& path = UnitBlueprintPath(id);
    if (models_.count(path)) return files;
    float scale = 1;
    std::string error;
    if (!ReadSources(path, &sources, &scale, &error)) return files;
  }
  for (const LodSource& s : sources)
    for (const std::string* f : {&s.mesh, &s.albedo, &s.normals, &s.specular, &s.secondary})
      if (!f->empty()) files.push_back(*f);
  // (a remastered one's: its own meshes and textures, as Build looks for them; a shared mesh's
  // remaster is found by Build itself)
  const std::string preview = "/preview/" + id + "/";
  if (vfs_->Exists(preview + id + "_lod0.scm")) {
    for (int i = 0; i < 8 && vfs_->Exists(preview + id + "_lod" + std::to_string(i) + ".scm"); ++i)
      files.push_back(preview + id + "_lod" + std::to_string(i) + ".scm");
    for (const std::string& folder : {preview, preview + "textures/"})
      for (const char* kind : {"albedo", "normal", "spec", "ao"}) files.push_back(folder + id + "_lab_" + kind + ".dds");
    if (wearArt)  // [grime] (its wear art, where Build takes it)
      for (const char* kind : {"masks", "damage_albedo", "damage_normal"}) files.push_back(preview + "textures/" + id + "_lab_" + kind + ".dds");
  } else {
    for (const char* kind : {"albedo", "normal", "spec", "ao"}) files.push_back("/mods/labvisuals/textures/" + id + "/" + id + "_lab_" + kind + ".dds");
  }
  return files;
}

bool ModelCache::UnitMade(const std::string& unitId) {
  const std::string id = Lower(unitId);
  return display_.count(id) ? models_.count("display:" + id) > 0 : models_.count(UnitBlueprintPath(id)) > 0;
}

// [hitches] (ReadAheadUpgrade: the kind's blueprint read on the textures' thread; AheadStep goes on from there)
void ModelCache::ReadAheadUpgrade(const std::string& unitId) {
  const std::string id = Lower(unitId);
  if (id.empty() || std::find(aheadSeen_.begin(), aheadSeen_.end(), id) != aheadSeen_.end()) return;
  aheadSeen_.push_back(id);
  Ahead a;
  a.of = id;
  a.path = UnitBlueprintPath(id);
  textures_->FetchLater({a.path});
  ahead_.push_back(std::move(a));
}

// [spikes] (MakeAhead: each kind straight to its files, stage 1)
void ModelCache::MakeAhead(const std::vector<std::string>& unitIds) {
  for (const std::string& unitId : unitIds) {
    const std::string id = Lower(unitId);
    if (id.empty() || UnitMade(id)) continue;
    bool asked = false;
    for (const Ahead& a : ahead_) asked = asked || a.to == id;
    if (asked) continue;
    Ahead a;
    a.to = id;
    AskFiles(&a);
    if (a.files.empty()) continue;  // (nothing to draw: no model to make)
    ahead_.push_back(std::move(a));
  }
}

// [spikes] A kind's files as its model's making will want them (Build): of UnitFiles' list those that are there, less the
// remastered textures Build would not take (the lab's LabVisuals ones where the simulation's looks name the textures; a
// remastered model's textures beside its meshes where it has a textures folder), and a remastered kind's own wreck's
// (RemasteredWreck). Its textures are made ahead, and its meshes (MeshJob, the meshes' own thread).
void ModelCache::AskFiles(Ahead* a) {
  const std::string& id = a->to;
  std::vector<std::string> listed = UnitFiles(id);
  const std::string preview = "/preview/" + id + "/";
  const bool merged = display_.count(id) > 0;
  const bool texturesFolder = vfs_->Exists(preview + "textures/" + id + "_lab_albedo.dds");
  a->wreck = !listed.empty() && !models_.count("wreck:" + id) && vfs_->Exists(preview + id + "_wreck_lod0.scm");
  if (a->wreck) {
    for (int i = 0; i < 8 && vfs_->Exists(preview + id + "_wreck_lod" + std::to_string(i) + ".scm"); ++i)
      listed.push_back(preview + id + "_wreck_lod" + std::to_string(i) + ".scm");
    for (const char* kind : {"albedo", "normal", "spec", "ao"})
      listed.push_back(preview + (vfs_->Exists(preview + "textures/" + id + "_wreck_lab_albedo.dds") ? "textures/" : "") + id + "_wreck_lab_" + kind + ".dds");
  }
  std::vector<std::string> textures, others;
  for (std::string& f : listed) {
    const std::string n = moho::Vfs::Normalize(f);
    if (n.empty() || std::find(textures.begin(), textures.end(), n) != textures.end() || std::find(others.begin(), others.end(), n) != others.end()) continue;
    if (merged && n.compare(0, 20, "/mods/labvisuals/tex") == 0) continue;
    if (texturesFolder && n.compare(0, preview.size(), preview) == 0 && n.find('/', preview.size()) == std::string::npos &&
        n.find("/" + id + "_lab_") != std::string::npos)
      continue;
    if (!vfs_->Exists(n)) continue;
    (n.size() > 4 && n.compare(n.size() - 4, 4, ".dds") == 0 ? textures : others).push_back(n);
  }
  textures_->MakeAhead(textures);
  a->files = std::move(textures);
  a->files.insert(a->files.end(), others.begin(), others.end());  // (its meshes: made ahead below, not read here)
  a->stage = 1;
  if (a->files.empty()) return;
  // its meshes made ahead on the meshes' thread, as Build will make them: the kind's (from the simulation's looks where
  // they name it, else from its blueprint file), then its own wreck's (RemasteredWreck's sources)
  MeshJob job;
  job.what = id;
  job.base = id;
  job.hitShapes = hitShapes;
  auto look = display_.find(id);
  if (look != display_.end()) {
    job.sources = look->second.lods;
  } else {
    job.blueprint = UnitBlueprintPath(id);
    job.what = job.blueprint;  // (Build's `what` for a model from a blueprint file: its path)
    const size_t slash = job.what.rfind('/');
    job.base = job.what.substr(slash + 1);
    const size_t under = job.base.rfind('_');
    job.base = under == std::string::npos ? job.base.substr(0, job.base.rfind('.')) : job.base.substr(0, under);
  }
  a->meshesOf = job.what;
  AskMeshes(std::move(job));
  if (a->wreck) {
    MeshJob wreck;
    wreck.what = "wreck:" + id;
    wreck.base = id;
    wreck.stem = id + "_wreck";
    wreck.blueprint = UnitBlueprintPath(id);
    wreck.first = "/preview/" + id + "/" + id + "_wreck_lod0.scm";  // (as RemasteredWreck names it)
    AskMeshes(std::move(wreck));
  }
}

// [spikes] (AheadStep) an asked-for kind done with: what was made ahead for it and not taken let go of
void ModelCache::FinishAhead(size_t k) {
  DropMeshes(ahead_[k].meshesOf);
  if (ahead_[k].wreck) DropMeshes("wreck:" + ahead_[k].to);
  ahead_.erase(ahead_.begin() + static_cast<std::ptrdiff_t>(k));
}

// [spikes] The meshes made ahead (MeshJob): one thread of the cache's own works the jobs in the order asked; what it made is
// kept by each mesh's inputs (ScmKey) until Build takes it or the kind is done with (DropMeshes). It stays at most a quarter
// of a gigabyte of prepared buffers ahead of what is taken.
struct ModelCache::MeshesAhead {
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<MeshJob> queue;
  std::unordered_map<std::string, int> pending;  // (jobs queued or being worked on, by their `what`)
  std::unordered_set<std::string> dropped;       // (dropped while being worked on: let go of when done)
  struct Kept {
    std::string what;
    std::unique_ptr<ScmAhead> scm;
  };
  std::unordered_map<std::string, Kept> scms;  // (by ScmKey)
  std::unordered_map<std::string, std::pair<std::string, std::vector<ModelBone>>> bones;  // (by skeleton mesh: whose, its bones)
  std::unordered_map<std::string, std::vector<std::string>> textures;  // (a wreck's blueprint's textures, by `what`)
  size_t held = 0;  // (the bytes of the buffers prepared and not taken)
  static constexpr size_t kMostHeld = 256u << 20;
  bool stop = false;
  std::thread thread;
};

ModelCache::ModelCache(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}

ModelCache::~ModelCache() {
  if (!meshesAhead_) return;
  {
    std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
    meshesAhead_->stop = true;
  }
  meshesAhead_->wake.notify_all();
  if (meshesAhead_->thread.joinable()) meshesAhead_->thread.join();
  meshesAhead_.reset();  // (what was prepared and not taken let go of, while the device is there)
}

void ModelCache::AskMeshes(MeshJob job) {
  if (!meshesAhead_) {
    meshesAhead_ = std::make_unique<MeshesAhead>();
    meshesAhead_->thread = std::thread([this] { MeshesRun(); });
  }
  {
    std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
    ++meshesAhead_->pending[job.what];
    meshesAhead_->dropped.erase(job.what);
    meshesAhead_->queue.push_back(std::move(job));
  }
  meshesAhead_->wake.notify_all();
}

bool ModelCache::MeshesPending(const std::string& what) const {
  if (!meshesAhead_) return false;
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  return meshesAhead_->pending.count(what) > 0;
}

std::unique_ptr<ScmAhead> ModelCache::TakeScm(const std::string& key) {
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  auto it = meshesAhead_->scms.find(key);
  if (it == meshesAhead_->scms.end()) return nullptr;
  std::unique_ptr<ScmAhead> scm = std::move(it->second.scm);
  meshesAhead_->scms.erase(it);
  meshesAhead_->held -= std::min(meshesAhead_->held, scm->bytes);
  meshesAhead_->wake.notify_all();
  return scm;
}

bool ModelCache::TakeBones(const std::string& skeletonMesh, std::vector<ModelBone>* out) {
  if (skeletonMesh.empty()) return false;
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  auto it = meshesAhead_->bones.find(skeletonMesh);
  if (it == meshesAhead_->bones.end()) return false;
  *out = std::move(it->second.second);
  meshesAhead_->bones.erase(it);
  return true;
}

std::vector<std::string> ModelCache::MeshesTextures(const std::string& what) {
  std::vector<std::string> out;
  if (!meshesAhead_) return out;
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  auto it = meshesAhead_->textures.find(what);
  if (it == meshesAhead_->textures.end()) return out;
  out = std::move(it->second);
  meshesAhead_->textures.erase(it);
  return out;
}

void ModelCache::DropMeshes(const std::string& what) {
  if (!meshesAhead_ || what.empty()) return;
  std::vector<std::unique_ptr<ScmAhead>> gone;  // (let go of outside the lock: declared first, destroyed last)
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  MeshesAhead& h = *meshesAhead_;
  for (auto it = h.queue.begin(); it != h.queue.end();) {
    if (it->what != what) {
      ++it;
      continue;
    }
    if (--h.pending[what] <= 0) h.pending.erase(what);
    it = h.queue.erase(it);
  }
  if (h.pending.count(what)) h.dropped.insert(what);  // (being worked on)
  for (auto it = h.scms.begin(); it != h.scms.end();) {
    if (it->second.what != what) {
      ++it;
      continue;
    }
    h.held -= std::min(h.held, it->second.scm->bytes);
    gone.push_back(std::move(it->second.scm));
    it = h.scms.erase(it);
  }
  for (auto it = h.bones.begin(); it != h.bones.end();) it = it->second.first == what ? h.bones.erase(it) : std::next(it);
  h.textures.erase(what);
  h.wake.notify_all();
}

void ModelCache::MeshesRun() {
  MeshesAhead& h = *meshesAhead_;
  std::unique_lock<std::mutex> lock(h.mutex);
  for (;;) {
    h.wake.wait(lock, [&] { return h.stop || (!h.queue.empty() && h.held < MeshesAhead::kMostHeld); });
    if (h.stop) return;
    MeshJob job = std::move(h.queue.front());
    h.queue.pop_front();
    lock.unlock();
    MeshesWork(job);
    lock.lock();
    if (--h.pending[job.what] <= 0) h.pending.erase(job.what);
  }
}

// (on the meshes' thread: Build's reading and LoadScm calls for one model, with the inputs Build will give them)
void ModelCache::MeshesWork(const MeshJob& job) {
  HitchLog::Scope note("meshes made ahead,", job.what);  // [hitches] (on the meshes' thread: no frame waits for it)
  auto read = [&](const std::string& path) -> std::optional<std::string> {
    const std::string key = moho::Vfs::Normalize(path);
    if (key.empty()) return std::nullopt;
    return vfs_->ReadFile(key);
  };
  std::vector<LodSource> sources = job.sources;
  std::vector<std::string> wreckTextures;
  if (!job.blueprint.empty()) {
    float scale = 1;
    std::string error;
    const bool listed = ReadSources(job.blueprint, &sources, &scale, &error);
    if (!job.first.empty()) {  // (a remastered wreck: RemasteredWreck's sources)
      if (!listed || sources.empty()) sources.assign(1, LodSource{});
      sources[0].mesh = job.first;
      // (the textures its blueprint names, which Build gets before the wreck's own replace them: made ahead too)
      for (const LodSource& s : sources)
        for (const std::string* f : {&s.albedo, &s.normals, &s.specular, &s.secondary})
          if (!f->empty() && vfs_->Exists(*f)) wreckTextures.push_back(*f);
    } else if (!listed) {
      sources.clear();
    }
  }
  std::vector<std::pair<std::string, std::unique_ptr<ScmAhead>>> made;
  std::vector<ModelBone> bones;
  BuildPlan plan = PlanBuild(job.base, std::move(sources), false, job.stem);
  std::optional<std::string> skeletonFile = plan.skeletonMesh.empty() ? std::nullopt : read(plan.skeletonMesh);
  if (skeletonFile) ReadScmBones(*skeletonFile, &bones);
  if (plan.previewed && job.stem.empty() && !bones.empty() && !plan.sources.empty() &&  // [machinery]
      vfs_->Exists(plan.preview + plan.file + "_machinery.json"))
    if (std::optional<std::string> finest = read(plan.sources[0].mesh)) AppendOwnBones(*finest, &bones);
  const bool unit = job.stem.empty() && (job.what.find('/') == std::string::npos ||
                                         (job.what.size() > 8 && job.what.compare(job.what.size() - 8, 8, "_unit.bp") == 0));
  bool firstSways = false, shaped = false;
  for (size_t i = 0; i < plan.sources.size(); ++i) {
    const LodSource& s = plan.sources[i];
    int stream = 0;
    const std::string kind = DrawnKind(Lower(s.shader), &stream);
    const bool ownSway = kind.compare(0, 10, "undulating") == 0 || kind == "labpbralpha";
    if (i == 0) firstSways = ownSway;
    const bool sways = ownSway || (i > 0 && firstSways);
    const bool first = i == 0;
    const bool shape = job.hitShapes && unit && !shaped;
    const int64_t most = i + 1 == plan.sources.size() ? INT64_MAX : kHitShapeTriangles;
    const bool spots = unit && first;
    auto scm = std::make_unique<ScmAhead>();
    std::optional<std::string> file = s.mesh == plan.skeletonMesh ? skeletonFile : read(s.mesh);
    if (!file) {
      scm->missing = true;
    } else {
      scm->ok = LoadScm(device_, *file, bones, &scm->lod, first ? &scm->boundsMin : nullptr, first ? &scm->boundsMax : nullptr, &scm->error,
                        first ? &scm->boneBoxes : nullptr, shape ? &scm->hitShape : nullptr, most, sways, spots ? &scm->paintSpots : nullptr,
                        scm.get(), first && unit ? &scm->deckHeight : nullptr);
    }
    if (scm->ok && shape && scm->hitShape) shaped = true;
    const bool stop = i == 0 && !scm->ok;  // (Build makes nothing more of a model whose finest level failed)
    made.emplace_back(ScmKey(s.mesh, bones, first, shape, most, sways, spots), std::move(scm));
    if (stop) break;
  }
  std::vector<std::unique_ptr<ScmAhead>> gone;  // (dropped meanwhile: let go of outside the lock, destroyed after it)
  std::lock_guard<std::mutex> lock(meshesAhead_->mutex);
  MeshesAhead& h = *meshesAhead_;
  if (h.dropped.count(job.what)) {
    for (auto& m : made) gone.push_back(std::move(m.second));
    if (h.pending[job.what] <= 1) h.dropped.erase(job.what);
    return;
  }
  for (auto& m : made) {
    if (h.scms.count(m.first)) continue;  // (another kind's job made the same already: the first is kept)
    h.held += m.second->bytes;
    h.scms.emplace(m.first, MeshesAhead::Kept{job.what, std::move(m.second)});
  }
  if (skeletonFile && !plan.skeletonMesh.empty() && !h.bones.count(plan.skeletonMesh))
    h.bones.emplace(plan.skeletonMesh, std::make_pair(job.what, std::move(bones)));
  if (!wreckTextures.empty()) h.textures[job.what] = std::move(wreckTextures);
}

// [spikes] Build's choice of files (its old own code, unchanged): the skeleton is that of the mesh the blueprint names, which
// is what the simulation poses; the remaster is looked for under the blueprint's own name first, then under the name of
// the mesh it draws (several projectiles draw one projectile's mesh by their MeshName - the cruise missiles Cruise01's -
// and take its remaster, at their own scale; visuals section 23); a remastered model brings as many levels as it has files
// (<file>_lod0.scm, _lod1, ...), whatever the blueprint lists.
ModelCache::BuildPlan ModelCache::PlanBuild(const std::string& base, std::vector<LodSource> sources, bool original, const std::string& stem) const {
  BuildPlan p;
  p.skeletonMesh = sources.empty() ? std::string() : sources[0].mesh;
  p.own = base;
  if (!original && !vfs_->Exists("/preview/" + base + "/" + base + "_lod0.scm") && !p.skeletonMesh.empty()) {
    std::string mesh = Lower(p.skeletonMesh.substr(p.skeletonMesh.rfind('/') + 1));
    const size_t lod = mesh.rfind("_lod0.scm");
    mesh = lod != std::string::npos ? mesh.substr(0, lod) : mesh.substr(0, mesh.rfind('.'));
    if (!mesh.empty() && mesh != base && vfs_->Exists("/preview/" + mesh + "/" + mesh + "_lod0.scm")) p.own = mesh;
  }
  p.preview = "/preview/" + p.own + "/";
  p.file = stem.empty() ? p.own : stem;  // (the files' own name in the folder: a wreck's is "<id>_wreck")
  p.previewed = !original && vfs_->Exists(p.preview + p.file + "_lod0.scm");
  if (p.previewed) {
    size_t count = 1;
    while (count < 8 && vfs_->Exists(p.preview + p.file + "_lod" + std::to_string(count) + ".scm")) ++count;
    sources.resize(count, sources.empty() ? LodSource{} : sources.back());
    for (size_t i = 0; i < count; ++i) sources[i].mesh = p.preview + p.file + "_lod" + std::to_string(i) + ".scm";
  }
  p.sources = std::move(sources);
  return p;
}

bool ModelCache::AheadStep() {
  auto dds = [](const std::string& f) { return f.size() > 4 && f.compare(f.size() - 4, 4, ".dds") == 0; };
  for (size_t k = 0; k < ahead_.size();) {
    Ahead& a = ahead_[k];
    bool drop = false;
    if (a.stage == 0) {  // its blueprint: what it upgrades to
      const int arrived = textures_->Arrived(a.path);
      if (arrived == 0) {
        ++k;
        continue;
      }
      // (2: another has taken the bytes read ahead - movement dust, deep tracks and the surf read unit blueprints too -
      // so it is read again here, a small file)
      std::optional<std::string> text = arrived > 0 ? textures_->ReadFile(a.path) : std::nullopt;
      BpValue bp;
      if (text && ReadBlueprintFile(*text, &bp)) a.to = Lower(bp["General"]["UpgradesTo"].StringOr(std::string()));
      bool known = a.to.empty() || UnitMade(a.to);
      for (size_t j = 0; j < ahead_.size() && !known; ++j) known = j != k && ahead_[j].to == a.to;
      if (!known) {
        AskFiles(&a);  // [spikes] (its textures made ahead off the frames' thread, the rest read ahead)
        known = a.files.empty();
      }
      drop = known;
      if (HitchLog::on) HitchLog::Add("read ahead: " + a.of + (a.to.empty() ? std::string(" upgrades to nothing") : " upgrades to " + a.to + (drop ? std::string(" (made or asked for already)") : ", " + std::to_string(a.files.size()) + " files asked for")), 0);
    }
    // (its first use came first: made as it was wanted; one with a wreck of its own goes on for that)
    if (!drop && a.stage >= 1 && a.stage <= 3 && !a.wreck && UnitMade(a.to)) drop = true;
    if (!drop && a.stage == 1) {  // [spikes] its textures made and its meshes prepared (off the frames' thread)
      const std::string wreck = "wreck:" + a.to;
      bool all = !MeshesPending(a.meshesOf) && !(a.wreck && MeshesPending(wreck));
      if (all && a.wreck && !a.wreckAsked) {  // (the textures its wreck's blueprint names, found by the meshes' thread)
        a.wreckAsked = true;
        a.wreckFiles = MeshesTextures(wreck);
        textures_->MakeAhead(a.wreckFiles);
      }
      for (const std::string& f : a.files) all = all && (!dds(f) || !textures_->Ahead(f));
      for (const std::string& f : a.wreckFiles) all = all && !textures_->Ahead(f);
      if (!all) {
        ++k;
        continue;
      }
      a.stage = 3;
    }
    if (!drop && a.stage == 3) {  // the model itself, as its first use would make it
      bool made = false;
      if (!UnitMade(a.to)) {
        HitchLog::Scope note("model made ahead,", a.of.empty() ? a.to : a.to + " (the next tier of " + a.of + ")");
        makingAhead_ = true;  // (its own next tier is asked about when it appears in the game, not now)
        Unit(a.to);
        makingAhead_ = false;
        ++madeAhead;
        made = true;
      }
      if (a.wreck) {
        a.stage = 4;  // (its wreck at the next step)
        if (made) return true;
      } else {
        FinishAhead(k);
        if (made) return true;
        continue;
      }
    }
    if (!drop && a.stage == 4) {  // [spikes] a remastered kind's own wreck, as the first of them to die would make it
      const bool made = !models_.count("wreck:" + a.to);
      if (made) {
        HitchLog::Scope note("model made ahead, the wreck of", a.to);
        RemasteredWreck(a.to);
      }
      FinishAhead(k);
      if (made) return true;
      continue;
    }
    if (drop) {
      FinishAhead(k);
      continue;
    }
    ++k;
  }
  return false;
}

void ModelCache::PrefetchBlueprints(const std::vector<std::string>& blueprintPaths) {
  std::vector<std::string> files;
  for (const std::string& given : blueprintPaths) {
    const std::string path = moho::Vfs::Normalize(given);
    if (models_.count(path)) continue;
    std::vector<LodSource> sources;
    std::string base = path.substr(path.rfind('/') + 1);
    const size_t under = base.rfind('_');
    base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);
    auto listed = display_.find(path);
    if (listed != display_.end()) {
      sources = listed->second.lods;
    } else {
      float scale = 1;
      std::string error;
      if (!ReadSources(path, &sources, &scale, &error)) continue;
      // (the lab's LabVisuals textures for the finest level, as Build looks for them)
      for (const char* kind : {"albedo", "normal", "spec", "ao"})
        files.push_back("/mods/labvisuals/textures/" + base + "/" + base + "_lab_" + kind + ".dds");
    }
    for (const LodSource& s : sources)
      for (const std::string* f : {&s.mesh, &s.albedo, &s.normals, &s.specular, &s.secondary})
        if (!f->empty()) files.push_back(*f);
  }
  textures_->Fetch(files);
}

// Loads a model's levels from their file names. `merged`: the names come from the simulation's merged
// blueprints (mods' changes are in them already; every texture has a name, there or not).
void ModelCache::Build(Model* m, const std::string& what, const std::string& base, std::vector<LodSource> sources, bool original, bool merged,
                       const std::string& faction, bool effect, const std::string& stem) {
  HitchLog::Scope made("model", stem.empty() ? what : what + " (" + stem + ")");  // [hitches]
  // A remastered model being tried out (the viewer's --preview): its folder, mounted at
  // /preview/<id>, replaces the meshes, and its LabPBR textures (there, or in textures/) replace the
  // blueprint's for every level. It brings as many levels as it has files (<id>_lod0.scm, _lod1, ...),
  // whatever the blueprint lists. The scale stays the blueprint's.
  // the skeleton is that of the mesh the blueprint names, which is what the simulation poses ([spikes] its plan: PlanBuild)
  BuildPlan plan = PlanBuild(base, std::move(sources), original, stem);
  const std::string& skeletonMesh = plan.skeletonMesh;
  std::optional<std::string> skeletonFile;
  bool skeletonRead = false;
  // [spikes] (made ahead: the skeleton's bones, read on the meshes' thread; else its file read here)
  if (!meshesAhead_ || !TakeBones(skeletonMesh, &m->bones)) {
    skeletonFile = skeletonMesh.empty() ? std::nullopt : textures_->ReadFile(skeletonMesh);
    skeletonRead = true;
    if (skeletonFile) ReadScmBones(*skeletonFile, &m->bones);
  }
  const std::string& own = plan.own;
  const std::string& preview = plan.preview;
  const std::string& file = plan.file;  // (the files' own name in the folder: a wreck's is "<id>_wreck")
  const bool previewed = plan.previewed;
  sources = std::move(plan.sources);
  // [machinery] a remastered factory's parts: their bones after the skeleton's (made ahead too: TakeBones gives them),
  // and how they move
  if (previewed && stem.empty() && !m->bones.empty() && !sources.empty() && vfs_->Exists(preview + file + "_machinery.json")) {
    if (std::optional<std::string> finest = textures_->ReadFile(sources[0].mesh)) AppendOwnBones(*finest, &m->bones);
    std::string why;
    if (std::optional<std::string> text = textures_->ReadFile(preview + file + "_machinery.json"))
      m->machinery = ReadMachinery(*text, m->bones, &why);
    if (!m->machinery) fprintf(stderr, "render: model %s: its machinery cannot be used: %s\n", what.c_str(), why.c_str());
    else printf("render: model %s: machinery, %zu parts on bones %zu-%zu\n", what.c_str(), m->machinery->parts.size(), m->machinery->simBones, m->bones.size() - 1);
  }
  // a remastered unit's lamps: a list beside its meshes (docs/render-unit-lights.md); a wreck has none
  if (!original && stem.empty()) {
    const std::string file = previewed ? preview + own + "_lights.bp" : "/mods/labvisuals/textures/" + base + "/" + base + "_lights.bp";
    auto text = vfs_->Exists(file) ? vfs_->ReadFile(file) : std::nullopt;
    BpValue list;
    if (text && !ReadBlueprintFile(*text, &list)) fprintf(stderr, "render: %s: the list of lights cannot be read\n", file.c_str());
    for (const BpValue& entry : list.items) {
      if (m->lights.size() >= 16) break;
      auto three = [&](const char* name, Vec3 fallback) {
        const BpValue& v = entry[name];
        if (v.items.size() < 3) return fallback;
        return Vec3{static_cast<float>(v.Item(0).NumberOr(0)), static_cast<float>(v.Item(1).NumberOr(0)), static_cast<float>(v.Item(2).NumberOr(0))};
      };
      ModelLight light;
      const Vec3 at = three("Position", {0, 0, 0});  // in the mesh's own coordinates, at rest
      light.place = at;
      const std::string bone = Lower(entry["Bone"].StringOr(std::string()));
      for (size_t b = 0; !bone.empty() && b < m->bones.size(); ++b)
        if (Lower(m->bones[b].name) == bone) {
          light.bone = static_cast<int>(b);
          light.place = Rotate(Conjugate(m->bones[b].rotation), at - m->bones[b].position);
          break;
        }
      if (!bone.empty() && light.bone < 0) fprintf(stderr, "render: %s: no bone named %s (the light rides on the unit itself)\n", file.c_str(), bone.c_str());
      light.color = three("Color", {1, 1, 1});
      light.radius = static_cast<float>(entry["Radius"].NumberOr(1.5));
      // ([unit lamps] lampScale: the viewer's --lamp-scale, to try a unit's lamps' strengths before they are written; 1)
      light.strength = static_cast<float>(entry["Strength"].NumberOr(1)) * lampScale;
      light.blink = static_cast<float>(entry["Blink"].NumberOr(0));
      // a beam: Direction is the lamp's facing in the mesh's coordinates at rest, and turns with the bone
      const Vec3 facing = three("Direction", {0, 0, 0});
      light.cone = static_cast<float>(entry["Cone"].NumberOr(0));
      if (Length(facing) < 0.01f || light.cone >= 179) light.cone = 0;
      light.direction = light.bone >= 0 ? Rotate(Conjugate(m->bones[static_cast<size_t>(light.bone)].rotation), facing) : facing;
      light.glow = static_cast<float>(entry["Glow"].NumberOr(0.15));
      light.hull = Lower(entry["Kind"].StringOr(std::string())) == "hull";
      m->lights.push_back(light);
    }
  }
  // a remastered unit's treads (its folder's <file>_treads.bp, Treads { Scrolling = true, Height = true }): whether they
  // scroll on the strips at the foot of its texture as the original's do, on every level (they share the texture), and
  // whether its albedo's alpha holds their height there ([treads] the eye looks into the grooves: ModelLod::treadDepth)
  bool ownTreadsScroll = false, ownTreadsHeight = false;
  if (previewed && stem.empty()) {
    const std::string treads = preview + file + "_treads.bp";
    auto text = vfs_->Exists(treads) ? vfs_->ReadFile(treads) : std::nullopt;
    BpValue t;
    if (text && !ReadBlueprintFile(*text, &t)) fprintf(stderr, "render: %s: cannot be read\n", treads.c_str());
    ownTreadsScroll = t["Scrolling"].kind == BpValue::Bool && t["Scrolling"].boolean;
    ownTreadsHeight = ownTreadsScroll && t["Height"].kind == BpValue::Bool && t["Height"].boolean;
  }
  // [hdprints] its own ground prints for the deep tracks (its folder's <file>_prints.bp: Model::prints); a picture named
  // without a folder lies in the unit's folder
  if (previewed && stem.empty()) {
    const std::string printsFile = preview + file + "_prints.bp";
    auto text = vfs_->Exists(printsFile) ? vfs_->ReadFile(printsFile) : std::nullopt;
    BpValue t;
    if (text && !ReadBlueprintFile(*text, &t)) fprintf(stderr, "render: %s: cannot be read\n", printsFile.c_str());
    auto print = [&](const char* kind, Model::Print* out) {
      const BpValue& e = t[kind];
      const std::string name = e["Texture"].StringOr(std::string());
      if (name.empty()) return;
      float w = static_cast<float>(e["Width"].NumberOr(0)), l = static_cast<float>(e["Length"].NumberOr(0));
      const BpValue& size = e["Size"];
      if (size.items.size() >= 2) w = static_cast<float>(size.Item(0).NumberOr(0)), l = static_cast<float>(size.Item(1).NumberOr(0));
      if (w <= 0 || l <= 0) {
        fprintf(stderr, "render: %s: %s has no size (not used)\n", printsFile.c_str(), kind);
        return;
      }
      out->texture = name[0] == '/' ? name : preview + name;
      out->width = w, out->length = l;
      if (!vfs_->Exists(out->texture)) {
        fprintf(stderr, "render: %s: %s's picture %s is not there (not used)\n", printsFile.c_str(), kind, out->texture.c_str());
        *out = Model::Print{};
      }
    };
    if (text) {
      print("Tread", &m->prints.tread);
      print("Tyre", &m->prints.tyre);
      print("Foot", &m->prints.foot);
    }
  }
  for (size_t i = 0; i < sources.size(); ++i) {
    const LodSource& s = sources[i];
    ModelLod lod;
    lod.serial = NextLodSerial();
    lod.cutoff = s.cutoff;
    lod.scrolling = previewed ? ownTreadsScroll : s.scrolling;
    if (lod.scrolling) m->scrolls = true;
    lod.shaderName = s.shader;
    // (the first game's names, which many blueprints still carry, mean today's techniques: the
    // original program's own table, docs/engine-answers.md, render 17; no name at all is "Unit")
    std::string kind = Lower(lod.shaderName);
    const bool solidScenery = kind == "tmeshnonormals";  // rocks, logs, debris
    lod.noShadow = kind.find("noshadow") != std::string::npos;
    {  // (the names as the first game's are turned into today's below; both spellings here)
      static const char* const kRgbOnly[] = {"glassalpha", "uefbuildcube", "cloud", "tmeshcloud", "outercloud", "tmeshoutercloud", "explosion",
                                             "tmeshexplosion", "effect", "tmeshadd", "nukeuef", "shieldseraphim"};
      for (const char* name : kRgbOnly) lod.rgbOnly = lod.rgbOnly || kind == name;
    }
    {  // (the first game's names as today's; [stream] a stream's water and its white water, stream_water.h: by their own shader
       // names, the visuals track's stream props, drawn as LabPBR and LabPBRAlpha while World::streamWater is off: DrawnKind)
      int stream = 0;
      kind = DrawnKind(kind, &stream);
      lod.stream = static_cast<decltype(lod.stream)>(stream);
      if (lod.stream) lod.streamMesh = s.mesh;
    }
    if (kind.compare(0, 4, "aeon") == 0) lod.faction = 1;
    else if (kind == "insect" || kind.compare(0, 6, "cybran") == 0) lod.faction = 2;
    else if (kind.compare(0, 8, "seraphim") == 0) lod.faction = 3;
    if (kind == "vertexnormal") {  // the far-away cards of trees: alpha test above 0x23, blended
      lod.alphaCut = 35.0f / 255;
      lod.vertexNormals = true;
      // (what the first game called "TMeshNoNormals" is rocks, logs and debris: solid things; they
      // keep their shadows and are not blended)
      lod.blend = !solidScenery;
      lod.teamMask = false;
    } else if (kind.find("aeonbuildpuddle") != std::string::npos) {  // the Aeon's pool of liquid metal
      lod.chrome = true;
      lod.teamMask = false;
    } else if (kind == "shieldfill") {
      lod.hidden = true;
    } else if (kind.find("shieldimpact") != std::string::npos) {
      // the patch that flashes on a shield where it is hit
      lod.blend = lod.glass = true;
      lod.teamMask = false;
      lod.impact = kind.find("cybran") != std::string::npos ? 2 : 1;
    } else if (kind.compare(0, 6, "shield") == 0) {
      // a shield's bubble, tinted by whose it is
      lod.blend = lod.glass = lod.shield = true;
      lod.teamMask = false;
      // ([basepattern] Chris, 2026-10-28: in the base game's colours: the Aeon's a pale sea green, the Cybran's violet, the
      // Seraphim's a pale blue; the UEF's blue as it was)
      if (kind.find("aeon") != std::string::npos) lod.shieldTint = {0.55f, 0.95f, 0.85f};
      else if (kind.find("cybran") != std::string::npos) lod.shieldTint = {0.5f, 0.3f, 1.0f};
      else if (kind.find("seraphim") != std::string::npos) lod.shieldTint = {0.45f, 0.78f, 1.0f};
      // [ogshield] whose kind of shield, for the original look's own way of drawing each (mesh_renderer.cpp)
      lod.faction = kind.find("aeon") != std::string::npos ? 1 : kind.find("cybran") != std::string::npos ? 2 : kind.find("seraphim") != std::string::npos ? 3 : 0;
    } else if (kind == "uefbuildcube") {  // the UEF's box of light round what it builds
      lod.blend = lod.frame = lod.glass = true;
      lod.teamMask = false;
    } else if (kind == "glassalpha") {
      // glass: a dome or a window, lit like the unit it sits on and seen through by its albedo's
      // alpha and its glow (an alpha cut of -1 says so to the shader); drawn after the water
      lod.blend = lod.glass = true;
      lod.alphaCut = -1;
    } else if (kind == "nukeuef" || kind == "nukeemp" || kind == "nukequantum" || kind == "outercloud" || kind == "cloud") {
      // a nuke's ball of fire, and the shells of cloud round one ("OuterCloud" is the EMP shell's
      // very look; "Cloud" stands still, and is lit by the sun in the original: here it is not)
      lod.blend = lod.glass = true;
      lod.teamMask = false;
      lod.nuke = kind == "nukeuef" ? 1 : 2;
      if (kind == "nukeemp" || kind == "outercloud") lod.nukeShell = {1.0f, -0.008f, 180.0f};
      if (kind == "nukequantum") lod.nukeShell = {0.5f, -0.006f, 150.0f};
      if (kind == "cloud") lod.nukeShell = {1.0f, 0.0f, 180.0f};
    } else if (kind == "alphafade") {  // a see-through slab that fades with its age: the UEF's build slices, its teleport box
      lod.blend = lod.glass = lod.fades = true;
      lod.teamMask = false;
    } else if (kind == "mapimager") {  // the machinery around the map
      lod.border = true;
      lod.teamMask = false;
    } else if (kind == "labpbralpha") {  // remastered foliage and the like
      lod.alphaCut = 0.5f;
      lod.leaves = true;
      lod.teamMask = false;
    } else if (kind.find("alpha") != std::string::npos) {  // NormalMappedAlpha and its relatives: alpha test above 0x80
      lod.alphaCut = 128.0f / 255;
      lod.teamMask = false;
    } else if (kind == "flat") {  // its texture's own colours, cut where the alpha is not above 0x7F
      lod.alphaCut = 127.0f / 255;
      lod.teamMask = false;
      lod.unlit = 1;
    } else if (kind == "uefquantumgate" || kind == "phalanxeffect") {
      lod.blend = lod.glass = true;
      lod.teamMask = false;
      lod.unlit = kind == "uefquantumgate" ? 2 : 3;
    } else if (kind.find("terrain") != std::string::npos || kind.find("tmesh") != std::string::npos || kind.find("clutter") != std::string::npos ||
               kind == "normalmappedglow" || kind == "effect" || kind == "explosion") {
      lod.teamMask = false;  // rocks and other scenery
    }
    HitchLog::Scope meshNote("mesh", s.mesh);  // [hitches] (its reading and making, up to its textures)
    std::string error;
    // (a unit's, for battle scars while they are on: the finest level small enough, else the coarsest; unit_wear.h)
    const bool unit = !effect && stem.empty() && (what.find('/') == std::string::npos ||
                                                  (what.size() > 8 && what.compare(what.size() - 8, 8, "_unit.bp") == 0));
    std::shared_ptr<const HitShape>* shape = hitShapes && unit && !m->hitShape ? &m->hitShape : nullptr;
    // (what sways, below: its trees of a group told apart, each to sway on its own; ModelLod::treeStep)
    const bool sways = kind.compare(0, 10, "undulating") == 0 || kind == "labpbralpha" || (!m->lods.empty() && m->lods[0].sway > 0);
    const bool first = m->lods.empty();
    const int64_t most = i + 1 == sources.size() ? INT64_MAX : kHitShapeTriangles;
    // [spikes] made ahead with these very inputs (MakeAhead, on the meshes' thread): its buffers finished here, nothing read
    std::unique_ptr<ScmAhead> pre = meshesAhead_ ? TakeScm(ScmKey(s.mesh, m->bones, first, shape != nullptr, most, sways, unit && first)) : nullptr;
    bool there = true, loaded = false;
    if (pre) {
      there = !pre->missing;
      error = pre->error;
      if (pre->ok) {
        lod.indexCount = pre->lod.indexCount;
        lod.vertexCount = pre->lod.vertexCount;
        lod.indexBytes = pre->lod.indexBytes;
        lod.treeStep = pre->lod.treeStep;
        lod.vertices = device_->FinishBuffer(std::move(pre->vertices));
        lod.indices = device_->FinishBuffer(std::move(pre->indices));
        loaded = lod.vertices && lod.indices;
        if (!loaded) error = "the device refused the mesh";
        if (loaded && first) m->boundsMin = pre->boundsMin, m->boundsMax = pre->boundsMax, m->boneBoxes = std::move(pre->boneBoxes);
        if (loaded && first) m->deckHeight = pre->deckHeight;  // [hull sit]
        if (loaded && shape) *shape = std::move(pre->hitShape);
        if (loaded && unit && first) m->paintSpots = std::move(pre->paintSpots);
      }
    } else {
      if (s.mesh == skeletonMesh && !skeletonRead) {  // ([spikes] the skeleton's bones were made ahead, this level was not)
        skeletonFile = textures_->ReadFile(skeletonMesh);
        skeletonRead = true;
      }
      auto mesh = s.mesh == skeletonMesh ? skeletonFile : textures_->ReadFile(s.mesh);
      there = mesh.has_value();
      loaded = mesh && LoadScm(device_, *mesh, m->bones, &lod, first ? &m->boundsMin : nullptr, first ? &m->boundsMax : nullptr, &error,
                               first ? &m->boneBoxes : nullptr, shape, most, sways, unit && first ? &m->paintSpots : nullptr, nullptr,
                               unit && first ? &m->deckHeight : nullptr);  // ([hull sit] a unit's deck)
    }
    if (!loaded) {
      // a missing coarser level is common: the finer ones then stand in
      if (i == 0 || there) fprintf(stderr, "render: model %s: mesh %s: %s\n", what.c_str(), s.mesh.c_str(), there ? error.c_str() : "file not found");
      if (i == 0) return;
      continue;
    }
    if (pre) meshNote.Append(" (made ahead)");  // [spikes]
    meshNote.End();  // [hitches]
    // a texture that is not there is simply not used (many props have no normal or specular
    // texture); one a blueprint file names itself and that is missing gets reported
    auto texture = [&](std::string name, bool named, gfx::Texture fallback, bool* remastered = nullptr, std::string* used = nullptr) {
      // (an effect's mesh, as a nuke's ball of fire: in the remastered look the visuals track's
      // texture of that path where it has made one, effect_files.h; for an effect entity's or a script
      // entity's mesh wherever its texture is, as the build cube's /meshes/generic/cube02_albedo.dds)
      if (!original && !name.empty() && (effect || moho::Vfs::Normalize(name).compare(0, 9, "/effects/") == 0)) {
        // (and first, with a faction, the visuals track's one for that faction: "<folder>/<faction>/<file>")
        std::string ours;
        if (!faction.empty()) {
          const std::string n = moho::Vfs::Normalize(name);
          const size_t slash = n.rfind('/');
          const std::string mine = std::string(kRemasteredEffects) + n.substr(0, slash + 1) + faction + n.substr(slash);
          if (vfs_->Exists(mine)) ours = mine;
        }
        if (ours.empty()) ours = EffectFile(*vfs_, name, true);
        if (remastered) *remastered = ours != name;
        name = ours;
      }
      if (name.empty() || (!(named && !merged) && !vfs_->Exists(name))) return fallback;
      if (used) *used = name;
      return textures_->Get(name, fallback);
    };
    lod.albedo = texture(s.albedo, s.albedoNamed, textures_->White(), &lod.remasteredEffect);
    lod.normals = texture(s.normals, s.normalsNamed, textures_->FlatNormal());
    std::string specularFile;  // (debris: where a unit's paint glows, Model::paintMaterial)
    lod.specular = texture(s.specular, s.specularNamed, gfx::Texture{}, nullptr, &specularFile);  // (none: black, or see plainMaterial below)
    lod.secondary = texture(s.secondary, s.secondaryNamed, textures_->White());
    lod.labPbr = kind == "labpbr" || kind == "labpbralpha";
    // Remastered textures from outside the blueprint: a previewed model's own, for every level; or,
    // when the names were read from a .bp file (which does not show what mods' scripts change), the
    // lab's LabVisuals mod's for the finest level, as that mod's script would set them.
    std::string lab;
    if (previewed) lab = preview + (vfs_->Exists(preview + "textures/" + file + "_lab_albedo.dds") ? "textures/" : "") + file + "_lab_";
    else if (!merged && !original && i == 0) lab = "/mods/labvisuals/textures/" + base + "/" + base + "_lab_";
    if (!lab.empty() && vfs_->Exists(lab + "albedo.dds")) {
      lod.albedo = textures_->Get(lab + "albedo.dds", lod.albedo);
      lod.normals = textures_->Get(lab + "normal.dds", lod.normals);
      lod.specular = textures_->Get(lab + "spec.dds", lod.specular);
      specularFile = vfs_->Exists(lab + "spec.dds") ? lab + "spec.dds" : specularFile;
      lod.secondary = vfs_->Exists(lab + "ao.dds") ? textures_->Get(lab + "ao.dds", textures_->White()) : textures_->White();
      lod.labPbr = true;
      lod.shaderName = "LabPBR";
    }
    // [grime] its wear art (ModelCache::wearArt), for every level of a remastered unit (they share their textures' layout):
    // the masks in place of the crease shading's texture (their red is the same), and its damage set
    if (wearArt && previewed && stem.empty() && !lab.empty() && lod.labPbr) {
      const bool tried = std::find(wearArtTrial.begin(), wearArtTrial.end(), own) != wearArtTrial.end();
      auto art = [&](const char* named, const std::string& trialName) {
        const std::string trial = "/wear-art/" + own + "/" + trialName;
        return tried && vfs_->Exists(trial) ? trial : vfs_->Exists(lab + named) ? lab + named : std::string();
      };
      const std::string masks = art("masks.dds", "masks/" + file + "_ao_masks.dds");
      const std::string damage = art("damage_albedo.dds", "damage/" + file + "_damage_albedo.dds");
      const std::string damageNormal = art("damage_normal.dds", "damage/" + file + "_damage_normal.dds");
      if (!masks.empty()) {
        lod.secondary = textures_->Get(masks, lod.secondary);
        lod.wearMasks = true;
      }
      if (!damage.empty() && !damageNormal.empty()) {
        lod.damageAlbedo = textures_->Get(damage);
        lod.damageNormal = textures_->Get(damageNormal);
      }
      if (i == 0 && (lod.wearMasks || lod.damageAlbedo))
        printf("render: %s: wear art%s%s\n", what.c_str(), lod.wearMasks ? ", masks " : "", lod.wearMasks ? masks.c_str() : "");
    }
    // [treads] (not the Seraphim's, whose albedo's alpha is their team colour)
    if (ownTreadsHeight && treadDepth > 0 && lod.labPbr && lod.faction != 3) lod.treadDepth = treadDepth;
    if (!lod.labPbr && !lod.specular) lod.specular = textures_->Black();
    if (kind.compare(0, 10, "undulating") == 0 || kind == "labpbralpha") lod.sway = 1;
    lod.plainMaterial = lod.labPbr && !lod.specular;
    if (lod.plainMaterial) lod.specular = textures_->Black();
    if (unit && m->lods.empty() && !lod.plainMaterial) m->paintMaterial = specularFile;
    m->lods.push_back(std::move(lod));
  }
  // a model that sways does so at every level (or it would stand still from afar and jump when
  // the level changes), by its height
  if (!m->lods.empty() && m->lods[0].sway > 0)
    for (ModelLod& l : m->lods) {
      l.sway = m->lods[0].sway;
      l.swayHeight = std::max(m->boundsMax.y, 1e-3f);
    }
  m->ok = !m->lods.empty();
  // the four-level standard (LOD0 above 250 pixels, LOD1 to 100, LOD2 to 30, LOD3 below)
  if (previewed && m->lods.size() == 4) m->levelPixels = {250, 100, 30};
}

}  // namespace render
