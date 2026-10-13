#include "models.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <optional>

#include "bp_reader.h"
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
    if (listed != display_.end()) return FromDisplay(id, id, listed->second);
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
    std::vector<std::string> found = vfs_->FindFiles("/mods", id + "_unit.bp");
    if (!found.empty()) path = found.front();
  }
  return unitPaths_.emplace(id, path).first->second;
}

bool ModelCache::Remastered(const std::string& blueprintId) const {
  const std::string id = Lower(blueprintId);
  return vfs_->Exists("/preview/" + id + "/" + id + "_lod0.scm") || vfs_->Exists("/mods/labvisuals/textures/" + id + "/" + id + "_lab_albedo.dds");
}

const Model* ModelCache::FromDisplay(const std::string& key, const std::string& base, const DisplayEntry& entry) {
  auto it = models_.find("display:" + key);
  if (it != models_.end()) return it->second.get();
  Model* m = models_.emplace("display:" + key, std::make_unique<Model>()).first->second.get();
  m->scale = entry.scale > 0 ? entry.scale : 1;
  Build(m, key, base, entry.lods, false, true);
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
  return FromDisplay(key, base, entry);
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
    lod.wreck = true;
    lod.sway = 0;  // (a burnt tree does not wave)
  }
  return kept.emplace(of, std::move(wreck)).first->second.get();
}

const Model* ModelCache::Blueprint(const std::string& blueprintPath, bool original) {
  std::string path = moho::Vfs::Normalize(blueprintPath);
  // "/units/uel0201/uel0201_unit.bp": the files beside it are named after the blueprint file
  const size_t slash = path.rfind('/');
  const std::string dir = path.substr(0, slash + 1);
  std::string base = path.substr(slash + 1);
  const size_t under = base.rfind('_');
  base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);
  if (!original) {
    auto listed = display_.find(path);
    if (listed != display_.end()) return FromDisplay(path, base, listed->second);
  }
  const std::string key = original ? path + "#original" : path;
  auto it = models_.find(key);
  if (it != models_.end()) return it->second.get();
  Model* m = models_.emplace(key, std::make_unique<Model>()).first->second.get();

  auto text = vfs_->ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) {
    fprintf(stderr, "render: model %s: %s\n", path.c_str(), text ? "the blueprint cannot be read" : "no such blueprint");
    return m;
  }
  const BpValue& display = bp["Display"];
  const bool meshFile = !display.IsTable() && bp["LODs"].IsTable();  // a mesh blueprint itself
  if (!meshFile && !display["Mesh"].IsTable()) return m;  // nothing to draw (many projectiles are effects only)
  const BpValue& lods = meshFile ? bp["LODs"] : display["Mesh"]["LODs"];
  m->scale = static_cast<float>((meshFile ? bp : display)["UniformScale"].NumberOr(1));
  std::vector<LodSource> sources;
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
    s.shader = entry["ShaderName"].StringOr("Unit");
    s.mesh = file("MeshName", "_lod" + std::to_string(i) + ".scm", nullptr);
    s.albedo = file("AlbedoName", "_albedo.dds", &s.albedoNamed);
    s.normals = file("NormalsName", "_normalsts.dds", &s.normalsNamed);
    s.specular = file("SpecularName", "_specteam.dds", &s.specularNamed);
    s.secondary = file("SecondaryName", "", &s.secondaryNamed);
    sources.push_back(std::move(s));
  }
  Build(m, path, base, sources, original, false);
  return m;
}

// Loads a model's levels from their file names. `merged`: the names come from the simulation's merged
// blueprints (mods' changes are in them already; every texture has a name, there or not).
void ModelCache::Build(Model* m, const std::string& what, const std::string& base, std::vector<LodSource> sources, bool original, bool merged) {
  // A remastered model being tried out (the viewer's --preview): its folder, mounted at
  // /preview/<id>, replaces the meshes, and its LabPBR textures (there, or in textures/) replace the
  // blueprint's for every level. It brings as many levels as it has files (<id>_lod0.scm, _lod1, ...),
  // whatever the blueprint lists. The scale stays the blueprint's.
  // the skeleton is that of the mesh the blueprint names, which is what the simulation poses
  std::string skeletonMesh = sources.empty() ? std::string() : sources[0].mesh;
  auto skeletonFile = skeletonMesh.empty() ? std::nullopt : vfs_->ReadFile(skeletonMesh);
  if (skeletonFile) ReadScmBones(*skeletonFile, &m->bones);
  const std::string preview = "/preview/" + base + "/";
  const bool previewed = !original && vfs_->Exists(preview + base + "_lod0.scm");
  // a remastered unit's lamps: a list beside its meshes (docs/render-unit-lights.md)
  if (!original) {
    const std::string file = previewed ? preview + base + "_lights.bp" : "/mods/labvisuals/textures/" + base + "/" + base + "_lights.bp";
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
    while (count < 8 && vfs_->Exists(preview + base + "_lod" + std::to_string(count) + ".scm")) ++count;
    sources.resize(count, sources.empty() ? LodSource{} : sources.back());
    for (size_t i = 0; i < count; ++i) sources[i].mesh = preview + base + "_lod" + std::to_string(i) + ".scm";
  }
  for (size_t i = 0; i < sources.size(); ++i) {
    const LodSource& s = sources[i];
    ModelLod lod;
    lod.cutoff = s.cutoff;
    lod.shaderName = s.shader;
    const std::string kind = Lower(lod.shaderName);
    if (kind.compare(0, 4, "aeon") == 0) lod.faction = 1;
    else if (kind == "insect" || kind.compare(0, 6, "cybran") == 0) lod.faction = 2;
    else if (kind.compare(0, 8, "seraphim") == 0) lod.faction = 3;
    if (kind == "vertexnormal") {  // the far-away cards of trees: alpha test above 0x23, blended
      lod.alphaCut = 35.0f / 255;
      lod.blend = lod.vertexNormals = true;
      lod.teamMask = false;
    } else if (kind.find("aeonbuildpuddle") != std::string::npos) {  // the Aeon's pool of liquid metal
      lod.chrome = true;
      lod.teamMask = false;
    } else if (kind == "shieldfill") {
      lod.hidden = true;
    } else if (kind.compare(0, 6, "shield") == 0 || kind.find("shieldimpact") != std::string::npos) {
      // a shield's bubble (and the patch that flashes where it is hit), tinted by whose it is
      lod.blend = lod.glass = lod.shield = true;
      lod.teamMask = false;
      if (kind.find("aeon") != std::string::npos) lod.shieldTint = {0.35f, 1.0f, 0.75f};
      else if (kind.find("cybran") != std::string::npos) lod.shieldTint = {1.0f, 0.2f, 0.12f};
      else if (kind.find("seraphim") != std::string::npos) lod.shieldTint = {0.95f, 0.85f, 0.55f};
    } else if (kind == "uefbuildcube") {  // the UEF's box of light round what it builds
      lod.blend = lod.frame = lod.glass = true;
      lod.teamMask = false;
    } else if (kind == "nukeuef" || kind == "nukeemp" || kind == "nukequantum") {  // a nuke's ball of fire
      lod.blend = lod.glass = true;
      lod.teamMask = false;
      lod.nuke = kind == "nukeuef" ? 1 : 2;
      if (kind == "nukeemp") lod.nukeShell = {1.0f, -0.008f, 180.0f};
      if (kind == "nukequantum") lod.nukeShell = {0.5f, -0.006f, 150.0f};
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
    } else if (kind.find("terrain") != std::string::npos || kind.find("tmesh") != std::string::npos || kind.find("clutter") != std::string::npos) {
      lod.teamMask = false;  // rocks and other scenery
    }
    auto mesh = s.mesh == skeletonMesh ? skeletonFile : vfs_->ReadFile(s.mesh);
    std::string error;
    if (!mesh || !LoadScm(device_, *mesh, m->bones, &lod, m->lods.empty() ? &m->boundsMin : nullptr, m->lods.empty() ? &m->boundsMax : nullptr, &error)) {
      // a missing coarser level is common: the finer ones then stand in
      if (i == 0 || mesh) fprintf(stderr, "render: model %s: mesh %s: %s\n", what.c_str(), s.mesh.c_str(), mesh ? error.c_str() : "file not found");
      if (i == 0) return;
      continue;
    }
    // a texture that is not there is simply not used (many props have no normal or specular
    // texture); one a blueprint file names itself and that is missing gets reported
    auto texture = [&](const std::string& name, bool named, gfx::Texture fallback) {
      if (name.empty() || (!(named && !merged) && !vfs_->Exists(name))) return fallback;
      return textures_->Get(name, fallback);
    };
    lod.albedo = texture(s.albedo, s.albedoNamed, textures_->White());
    lod.normals = texture(s.normals, s.normalsNamed, textures_->FlatNormal());
    lod.specular = texture(s.specular, s.specularNamed, gfx::Texture{});  // (none: black, or see plainMaterial below)
    lod.secondary = texture(s.secondary, s.secondaryNamed, textures_->White());
    lod.labPbr = kind == "labpbr" || kind == "labpbralpha";
    // Remastered textures from outside the blueprint: a previewed model's own, for every level; or,
    // when the names were read from a .bp file (which does not show what mods' scripts change), the
    // lab's LabVisuals mod's for the finest level, as that mod's script would set them.
    std::string lab;
    if (previewed) lab = preview + (vfs_->Exists(preview + "textures/" + base + "_lab_albedo.dds") ? "textures/" : "") + base + "_lab_";
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
