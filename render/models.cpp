#include "models.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <optional>

#include "bp_reader.h"
#include "effect_files.h"
#include "core/vfs.h"

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

// `skeleton`: the bones the model's vertices are to follow by index (Model::bones). Where the file
// lists its own bones in another order, its vertices are renumbered by the bones' names.
uint32_t NextLodSerial() {
  static uint32_t made = 0;
  return ++made;
}

bool LoadScm(gfx::Device* device, const std::string& file, const std::vector<ModelBone>& skeleton, ModelLod* lod, Vec3* boundsMin,
             Vec3* boundsMax, std::string* error) {
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
  gfx::BufferDesc vb;
  vb.stride = sizeof(ScmVertex);
  vb.size = static_cast<uint32_t>(vertexCount * static_cast<int64_t>(sizeof(ScmVertex)));
  vb.data = vertices;
  lod->vertices = device->CreateBuffer(vb);
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = static_cast<uint32_t>(indexBytes);
  ib.size = static_cast<uint32_t>(indexCount * indexBytes);
  ib.data = file.data() + indexOffset;
  lod->indices = device->CreateBuffer(ib);
  lod->indexCount = static_cast<uint32_t>(indexCount);
  if (!lod->vertices || !lod->indices) return fail("the device refused the mesh");
  return true;
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
  if (!original) {
    auto listed = display_.find(id);
    if (listed != display_.end()) return FromDisplay(id, id, listed->second, std::string());
  }
  return Blueprint(UnitBlueprintPath(id), original);
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
  Build(m, key, base, entry.lods, false, true, faction, effect);
  return m;
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
    const std::string id = Lower(unitId);
    std::vector<LodSource> sources;
    auto listed = display_.find(id);
    if (listed != display_.end()) {
      if (models_.count("display:" + id)) continue;
      sources = listed->second.lods;
    } else {
      const std::string& path = UnitBlueprintPath(id);
      if (models_.count(path)) continue;
      float scale = 1;
      std::string error;
      if (!ReadSources(path, &sources, &scale, &error)) continue;
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
    } else {
      for (const char* kind : {"albedo", "normal", "spec", "ao"}) files.push_back("/mods/labvisuals/textures/" + id + "/" + id + "_lab_" + kind + ".dds");
    }
  }
  textures_->Fetch(files);
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
                       const std::string& faction, bool effect) {
  // A remastered model being tried out (the viewer's --preview): its folder, mounted at
  // /preview/<id>, replaces the meshes, and its LabPBR textures (there, or in textures/) replace the
  // blueprint's for every level. It brings as many levels as it has files (<id>_lod0.scm, _lod1, ...),
  // whatever the blueprint lists. The scale stays the blueprint's.
  // the skeleton is that of the mesh the blueprint names, which is what the simulation poses
  std::string skeletonMesh = sources.empty() ? std::string() : sources[0].mesh;
  auto skeletonFile = skeletonMesh.empty() ? std::nullopt : textures_->ReadFile(skeletonMesh);
  if (skeletonFile) ReadScmBones(*skeletonFile, &m->bones);
  // (the remaster is looked for under the blueprint's own name first, then under the name of the
  // mesh it draws: several projectiles draw one projectile's mesh by their MeshName - the cruise
  // missiles Cruise01's - and take its remaster, at their own scale; visuals section 23)
  std::string own = base;
  if (!original && !vfs_->Exists("/preview/" + base + "/" + base + "_lod0.scm") && !skeletonMesh.empty()) {
    std::string mesh = Lower(skeletonMesh.substr(skeletonMesh.rfind('/') + 1));
    const size_t lod = mesh.rfind("_lod0.scm");
    mesh = lod != std::string::npos ? mesh.substr(0, lod) : mesh.substr(0, mesh.rfind('.'));
    if (!mesh.empty() && mesh != base && vfs_->Exists("/preview/" + mesh + "/" + mesh + "_lod0.scm")) own = mesh;
  }
  const std::string preview = "/preview/" + own + "/";
  const bool previewed = !original && vfs_->Exists(preview + own + "_lod0.scm");
  // a remastered unit's lamps: a list beside its meshes (docs/render-unit-lights.md)
  if (!original) {
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
      light.strength = static_cast<float>(entry["Strength"].NumberOr(1));
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
  if (previewed) {
    size_t count = 1;
    while (count < 8 && vfs_->Exists(preview + own + "_lod" + std::to_string(count) + ".scm")) ++count;
    sources.resize(count, sources.empty() ? LodSource{} : sources.back());
    for (size_t i = 0; i < count; ++i) sources[i].mesh = preview + own + "_lod" + std::to_string(i) + ".scm";
  }
  for (size_t i = 0; i < sources.size(); ++i) {
    const LodSource& s = sources[i];
    ModelLod lod;
    lod.serial = NextLodSerial();
    lod.cutoff = s.cutoff;
    lod.scrolling = s.scrolling && !previewed;
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
    {
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
      if (kind.find("aeon") != std::string::npos) lod.shieldTint = {0.35f, 1.0f, 0.75f};
      else if (kind.find("cybran") != std::string::npos) lod.shieldTint = {1.0f, 0.2f, 0.12f};
      else if (kind.find("seraphim") != std::string::npos) lod.shieldTint = {0.95f, 0.85f, 0.55f};
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
    auto mesh = s.mesh == skeletonMesh ? skeletonFile : textures_->ReadFile(s.mesh);
    std::string error;
    if (!mesh || !LoadScm(device_, *mesh, m->bones, &lod, m->lods.empty() ? &m->boundsMin : nullptr, m->lods.empty() ? &m->boundsMax : nullptr, &error)) {
      // a missing coarser level is common: the finer ones then stand in
      if (i == 0 || mesh) fprintf(stderr, "render: model %s: mesh %s: %s\n", what.c_str(), s.mesh.c_str(), mesh ? error.c_str() : "file not found");
      if (i == 0) return;
      continue;
    }
    // a texture that is not there is simply not used (many props have no normal or specular
    // texture); one a blueprint file names itself and that is missing gets reported
    auto texture = [&](std::string name, bool named, gfx::Texture fallback, bool* remastered = nullptr) {
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
      return textures_->Get(name, fallback);
    };
    lod.albedo = texture(s.albedo, s.albedoNamed, textures_->White(), &lod.remasteredEffect);
    lod.normals = texture(s.normals, s.normalsNamed, textures_->FlatNormal());
    lod.specular = texture(s.specular, s.specularNamed, gfx::Texture{});  // (none: black, or see plainMaterial below)
    lod.secondary = texture(s.secondary, s.secondaryNamed, textures_->White());
    lod.labPbr = kind == "labpbr" || kind == "labpbralpha";
    // Remastered textures from outside the blueprint: a previewed model's own, for every level; or,
    // when the names were read from a .bp file (which does not show what mods' scripts change), the
    // lab's LabVisuals mod's for the finest level, as that mod's script would set them.
    std::string lab;
    if (previewed) lab = preview + (vfs_->Exists(preview + "textures/" + own + "_lab_albedo.dds") ? "textures/" : "") + own + "_lab_";
    else if (!merged && !original && i == 0) lab = "/mods/labvisuals/textures/" + base + "/" + base + "_lab_";
    if (!lab.empty() && vfs_->Exists(lab + "albedo.dds")) {
      lod.albedo = textures_->Get(lab + "albedo.dds", lod.albedo);
      lod.normals = textures_->Get(lab + "normal.dds", lod.normals);
      lod.specular = textures_->Get(lab + "spec.dds", lod.specular);
      lod.secondary = vfs_->Exists(lab + "ao.dds") ? textures_->Get(lab + "ao.dds", textures_->White()) : textures_->White();
      lod.labPbr = true;
      lod.shaderName = "LabPBR";
    }
    if (!lod.labPbr && !lod.specular) lod.specular = textures_->Black();
    if (kind.compare(0, 10, "undulating") == 0 || kind == "labpbralpha") lod.sway = 1;
    lod.plainMaterial = lod.labPbr && !lod.specular;
    if (lod.plainMaterial) lod.specular = textures_->Black();
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
