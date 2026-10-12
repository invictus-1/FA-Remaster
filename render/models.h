// Models: a blueprint's meshes (.scm) and textures, loaded once and kept on the graphics card.
// A model has one or more levels of detail; the blueprint says up to which distance each is used.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "textures.h"

namespace moho {
class Vfs;
}

namespace render {

// A mesh file's vertex, exactly as stored (68 bytes) and as handed to the shaders.
struct ScmVertex {
  float position[3];
  float normal[3];
  float tangent[3];
  float binormal[3];
  float uv0[2];
  float uv1[2];
  uint8_t bones[4];  // only the first is used: each vertex follows one bone
};

struct ModelLod {
  gfx::Buffer vertices, indices;
  uint32_t indexCount = 0;
  gfx::Texture albedo, normals, specular;
  gfx::Texture secondary;  // remastered units: ambient occlusion in red
  // Remastered units ("LabPBR"): the specular texture holds the material itself - red: metal,
  // green: roughness, blue: glow, alpha: team colour mask. Otherwise it is the original's spec-team
  // texture (red: reflection, green: highlight, blue: glow, alpha: team colour mask), which the
  // modern lighting has to translate.
  bool labPbr = false;
  float cutoff = 0;        // used while the camera is no further away than this (the blueprint's LODCutoff)
  std::string shaderName;  // "Unit", "Aeon", "Insect", "Seraphim", "UndulatingNormalMappedAlpha", ...
  // What the shader name means for drawing (from the original's techniques of those names):
  float alphaCut = 0;          // pixels whose albedo alpha is not above this are left out (foliage)
  bool blend = false;          // drawn blended over what is behind, by the albedo's alpha
  bool vertexNormals = false;  // lit by the mesh's own normals, no normal map
  bool teamMask = true;        // the team colour replaces the albedo where the spec-team texture says;
                               // otherwise the colour multiplies the albedo
};

struct Model {
  bool ok = false;
  std::vector<ModelLod> lods;  // finest first
  float scale = 1;             // model units to world units (the blueprint's Display.UniformScale)
  Vec3 boundsMin, boundsMax;   // of the finest level, in model units
};

class ModelCache {
 public:
  ModelCache(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}

  // The model of a unit blueprint id ("uel0201"). Never null: a unit whose files cannot be read gets
  // a model with ok == false (reported once).
  const Model* Unit(const std::string& blueprintId);
  // The model of any blueprint file with a Display.Mesh ("/env/evergreen/props/rocks/rock01_prop.bp").
  const Model* Blueprint(const std::string& blueprintPath);

 private:
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  std::unordered_map<std::string, std::unique_ptr<Model>> models_;
  std::unordered_map<std::string, std::string> unitPaths_;  // unit id -> its blueprint file
};

}  // namespace render
