#include "models.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bp_reader.h"
#include "core/vfs.h"

namespace render {
namespace {

// .scm: "MODL", then 11 ints (version 5, bone offset, weighted bone count, vertex offset, extra vertex
// offset, vertex count, index offset, index count, info offset, info count, total bone count);
// vertices of 68 bytes; indices of 16 bits, three per triangle.
bool LoadScm(gfx::Device* device, const std::string& file, ModelLod* lod, Vec3* boundsMin, Vec3* boundsMax, std::string* error) {
  auto fail = [&](const char* what) {
    *error = what;
    return false;
  };
  if (file.size() < 48 || file.compare(0, 4, "MODL") != 0) return fail("not an .scm file");
  int32_t h[11];
  std::memcpy(h, file.data() + 4, sizeof h);
  const int64_t vertexOffset = h[3], vertexCount = h[5], indexOffset = h[6], indexCount = h[7];
  if (h[0] != 5) return fail("unsupported .scm version");
  if (vertexCount <= 0 || indexCount <= 0 || vertexOffset < 0 || indexOffset < 0 ||
      vertexOffset + vertexCount * static_cast<int64_t>(sizeof(ScmVertex)) > static_cast<int64_t>(file.size()) ||
      indexOffset + indexCount * 2 > static_cast<int64_t>(file.size()))
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
  gfx::BufferDesc vb;
  vb.stride = sizeof(ScmVertex);
  vb.size = static_cast<uint32_t>(vertexCount * static_cast<int64_t>(sizeof(ScmVertex)));
  vb.data = vertices;
  lod->vertices = device->CreateBuffer(vb);
  gfx::BufferDesc ib;
  ib.kind = gfx::BufferKind::Index;
  ib.stride = 2;
  ib.size = static_cast<uint32_t>(indexCount * 2);
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

const Model* ModelCache::Unit(const std::string& blueprintId) {
  std::string id = Lower(blueprintId);
  return Blueprint("/units/" + id + "/" + id + "_unit.bp");
}

const Model* ModelCache::Blueprint(const std::string& blueprintPath) {
  std::string path = moho::Vfs::Normalize(blueprintPath);
  auto it = models_.find(path);
  if (it != models_.end()) return it->second.get();
  Model* m = models_.emplace(path, std::make_unique<Model>()).first->second.get();

  // "/units/uel0201/uel0201_unit.bp": the files beside it are named after the blueprint file
  const size_t slash = path.rfind('/');
  const std::string dir = path.substr(0, slash + 1);
  std::string base = path.substr(slash + 1);
  const size_t under = base.rfind('_');
  base = under == std::string::npos ? base.substr(0, base.rfind('.')) : base.substr(0, under);

  auto text = vfs_->ReadFile(path);
  BpValue bp;
  if (!text || !ReadBlueprintFile(*text, &bp)) {
    fprintf(stderr, "render: model %s: %s\n", path.c_str(), text ? "the blueprint cannot be read" : "no such blueprint");
    return m;
  }
  const BpValue& display = bp["Display"];
  const BpValue& lods = display["Mesh"]["LODs"];
  m->scale = static_cast<float>(display["UniformScale"].NumberOr(1));
  const size_t count = std::max<size_t>(1, lods.items.size());
  for (size_t i = 0; i < count; ++i) {
    const BpValue& entry = lods.Item(i);
    ModelLod lod;
    lod.cutoff = static_cast<float>(entry["LODCutoff"].NumberOr(1e9));
    lod.shaderName = entry["ShaderName"].StringOr("Unit");
    const std::string kind = Lower(lod.shaderName);
    if (kind == "vertexnormal") {  // the far-away cards of trees: alpha test above 0x23, blended
      lod.alphaCut = 35.0f / 255;
      lod.blend = lod.vertexNormals = true;
      lod.teamMask = false;
    } else if (kind.find("alpha") != std::string::npos) {  // NormalMappedAlpha and its relatives: alpha test above 0x80
      lod.alphaCut = 128.0f / 255;
      lod.teamMask = false;
    } else if (kind.find("terrain") != std::string::npos || kind.find("tmesh") != std::string::npos || kind.find("clutter") != std::string::npos) {
      lod.teamMask = false;  // rocks and other scenery
    }
    // a name in the blueprint is beside the blueprint unless it starts at the root
    auto file = [&](const char* key, const std::string& standard) {
      const std::string given = entry[key].StringOr(std::string());
      if (given.empty()) return dir + base + standard;
      return given[0] == '/' ? given : dir + given;
    };
    const std::string meshPath = file("MeshName", "_lod" + std::to_string(i) + ".scm");
    auto mesh = vfs_->ReadFile(meshPath);
    std::string error;
    if (!mesh || !LoadScm(device_, *mesh, &lod, i == 0 ? &m->boundsMin : nullptr, i == 0 ? &m->boundsMax : nullptr, &error)) {
      // a missing coarser level is common: the finer one before it is then used for its distances too
      if (i == 0 || mesh) fprintf(stderr, "render: model %s: mesh %s: %s\n", path.c_str(), meshPath.c_str(), mesh ? error.c_str() : "file not found");
      if (i == 0) return m;
      m->lods.back().cutoff = std::max(m->lods.back().cutoff, lod.cutoff);
      continue;
    }
    // a texture the blueprint does not name and that is not there is simply not used (many props
    // have no normal or specular texture); a named one that is missing gets reported
    auto texture = [&](const char* key, const std::string& standard, gfx::Texture fallback) {
      const std::string name = file(key, standard);
      if (entry[key].kind != BpValue::String && !vfs_->Exists(name)) return fallback;
      return textures_->Get(name, fallback);
    };
    lod.albedo = texture("AlbedoName", "_albedo.dds", textures_->White());
    lod.normals = texture("NormalsName", "_normalsts.dds", textures_->FlatNormal());
    lod.specular = texture("SpecularName", "_specteam.dds", textures_->Black());
    m->lods.push_back(std::move(lod));
  }
  m->ok = true;
  return m;
}

}  // namespace render
