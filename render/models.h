// Models: a blueprint's meshes (.scm) and textures, loaded once and kept on the graphics card.
// A model has one or more levels of detail; the blueprint says up to which distance each is used.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
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

// A bone of a model's skeleton, standing at rest: where it is in the model's own space (model
// units), composed down the chain of parents the way the simulation composes it.
struct ModelBone {
  std::string name;
  int parent = -1;
  Vec3 position;
  Quat rotation;
};

// A lamp of a remastered unit (its <id>_lights.bp; docs/render-unit-lights.md): a point of light
// that rides on a bone.
struct ModelLight {
  int bone = -1;         // which of Model::bones it moves with (-1: the unit itself)
  Vec3 place;            // in that bone's own frame, model units
  Vec3 color{1, 1, 1};   // linear
  float radius = 1.5f;   // how far it lights, model units
  float strength = 1;
  float blink = 0;       // seconds for one on-and-off; 0: steady
  Vec3 direction;        // a beam: where it points, in the bone's frame
  float cone = 0;        // a beam: its whole opening angle, degrees; 0: it shines to all sides
  float glow = 0.15f;    // a beam: the share that still shines to all sides close to the lamp
  bool hull = false;     // Kind 'hull': the unit's own faint glow, drawn in its army's colour
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
  bool plainMaterial = false;  // a remastered level without that texture: rough, no metal, no glow
  float cutoff = 0;        // the blueprint's LODCutoff: the distance the ORIGINAL game uses this level up to.
                           // This renderer chooses levels by size on screen instead (mesh_renderer.h).
  std::string shaderName;  // "Unit", "Aeon", "Insect", "Seraphim", "UndulatingNormalMappedAlpha", ...
  // What the shader name means for drawing (from the original's techniques of those names):
  float alphaCut = 0;          // pixels whose albedo alpha is not above this are left out (foliage)
  bool blend = false;          // drawn blended over what is behind, by the albedo's alpha
  bool border = false;         // "MapImager": the machinery a map lies in: lit by the sun's angle alone, its
                               // albedo's alpha added to its colour, glowing where the specular's blue says
  float sway = 0;              // how much it sways in the wind (the original's "Undulating..." trees, the
                               // remaster's foliage): 0 not at all, 1 as a tree does
  float swayHeight = 1;        // the model's height in its own units: the sway grows from the foot to there
  bool leaves = false;         // "LabPBRAlpha": a remastered surface with holes: seen from both sides (lit as
                               // facing whoever looks), its holes' edges smoothed, and its specular texture's
                               // alpha is how much light comes through from the far side (no team colour)
  bool vertexNormals = false;  // lit by the mesh's own normals, no normal map
  bool glass = false;          // see-through, drawn after the water without writing depth (frames of light, shields)
  bool shield = false;         // "Shield...": a shield's bubble: a faint tinted shell, brighter toward its rim
  Vec3 shieldTint{0.25f, 0.5f, 1.0f};
  bool hidden = false;         // "ShieldFill": the original draws it into the depth alone; here it is not drawn
  bool wreck = false;          // wreckage (ModelCache::Wreck): burnt and dull, no team colour, no glow
  bool frame = false;          // "UEFBuildCube": a box of light round what the UEF builds: see-through, blue,
                               // drifting, not lit (its texture is clouds, with the holes in its alpha)
  bool teamMask = true;        // the team colour replaces the albedo where the spec-team texture says;
                               // otherwise the colour multiplies the albedo
  // Whose look the textures were painted for (the channels mean different things, mesh_renderer.cpp):
  // 0 the UEF's "Unit" and everything else, 1 Aeon, 2 Cybran ("Insect"), 3 Seraphim.
  int faction = 0;
};

struct Model {
  bool ok = false;
  std::vector<ModelLod> lods;  // finest first
  float scale = 1;             // model units to world units (the blueprint's Display.UniformScale)
  Vec3 boundsMin, boundsMax;   // of the finest level, in model units
  // The skeleton of the mesh the blueprint names: the one the simulation poses. The vertices of
  // every level follow these bones by index (a level whose file lists its bones in another order,
  // or a remastered mesh tried out in the blueprint's place, is matched to them by name at loading).
  std::vector<ModelBone> bones;
  // For a model built to the four-level standard (a remastered unit's export for this engine): the
  // size on screen, in pixels, down to which each level but the last is used - the sizes its levels
  // were made and judged for. Empty: levels are chosen by their triangle counts (mesh_renderer.h).
  std::vector<float> levelPixels;
  std::vector<ModelLight> lights;  // a remastered unit's lamps (at most 16)
};

class ModelCache {
 public:
  ModelCache(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : device_(device), vfs_(vfs), textures_(textures) {}

  // The model of a unit blueprint id ("uel0201"). Never null: a unit whose files cannot be read gets
  // a model with ok == false (reported once).
  // `original`: the game's own model even when a remastered one is there (to show both side by side).
  const Model* Unit(const std::string& blueprintId, bool original = false);
  // Where that unit's blueprint file is: the game's own in /units, a mod's in its folder under /mods.
  const std::string& UnitBlueprintPath(const std::string& blueprintId);
  // Is there a remastered model or material for this unit (a --preview folder, the lab's LabVisuals mod)?
  bool Remastered(const std::string& blueprintId) const;
  // The model of any blueprint file with a Display.Mesh ("/env/evergreen/props/rocks/rock01_prop.bp"),
  // or of a mesh blueprint file itself ("/meshes/game/map-border_squ_uef_mesh.bp").
  const Model* Blueprint(const std::string& blueprintPath, bool original = false);
  // A model from a mesh file and an albedo texture named outright, with a shader's name (what the
  // game's interface script places in the world, placed.h). One level; `uniformScale` is its scale.
  const Model* Mesh(const std::string& meshPath, const std::string& albedoPath, const std::string& shader, float uniformScale = 1);
  // The same model as wreckage (the game's "..._mesh_wreck": the mesh with a burnt look). `ownScale`: keep
  // the model's scale (a burnt tree); otherwise it is 1 and whoever places it gives the size (a dead
  // unit's wreck: the script sizes the prop).
  const Model* Wreck(const Model* of, bool ownScale);
  // Reads the mesh and texture names of blueprints as the simulation has them after the game's
  // scripts and the mods changed them: the log the engine's tools/dump_display.lua writes. Models
  // listed there take their names from it instead of from the .bp file. Returns the levels read.
  int LoadDisplayTable(const std::string& hostPath);
  // The same from the simulation itself: how one blueprint looks (frame.h, BlueprintLook). Models
  // asked for afterwards take their names from it.
  void SetLook(const BlueprintLook& look);

 private:
  struct LodSource {
    float cutoff = 1e9f;
    std::string shader = "Unit";
    std::string mesh, albedo, normals, specular, secondary;  // virtual paths; empty: none
    bool albedoNamed = false, normalsNamed = false, specularNamed = false, secondaryNamed = false;
  };
  struct DisplayEntry {
    float scale = 1;
    std::vector<LodSource> lods;
  };
  const Model* FromDisplay(const std::string& key, const std::string& base, const DisplayEntry& entry);
  void Build(Model* m, const std::string& what, const std::string& base, std::vector<LodSource> sources, bool original, bool merged);
  std::unordered_map<std::string, DisplayEntry> display_;  // by unit id, or by a prop's blueprint path
  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  std::unordered_map<std::string, std::unique_ptr<Model>> models_;
  std::unordered_map<std::string, std::string> unitPaths_;  // unit id -> its blueprint file
  std::unordered_map<const Model*, std::unique_ptr<Model>> wrecks_[2];  // by the model it is of: [1] with its own scale
};

}  // namespace render
