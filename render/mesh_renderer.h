// Draws models: every frame the caller lists what is in the world (a model, where, in which colour);
// each pass then draws what of that list it can see, sorted by mesh, each mesh's instances together.
#pragma once
#include <string>
#include <vector>

#include "frame.h"
#include "gfx.h"
#include "models.h"
#include "textures.h"
#include "view.h"

namespace render {

// A unit's pose between two beats: its bones that are not at rest at the beat shown and at the one
// after it (each list in rising bone order), and how far the moment drawn is from one to the other.
// The lists must stay where they are until the frame's last Draw.
struct Pose {
  const FrameBone* now = nullptr;
  size_t nowCount = 0;
  const FrameBone* next = nullptr;
  size_t nextCount = 0;
  float blend = 0;
};

class MeshRenderer {
 public:
  MeshRenderer(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  bool Init(std::string* error);

  // Empties the list.
  void Begin();
  // `world` places the model's own units in the world (so it includes the model's scale).
  // `teamColor` is rgb in 0..1; `built` is the fraction complete (1 for a finished unit).
  // `pose`: where its bones are, for a model that does not stand at rest (null: at rest).
  // `tint`: the model is shown as a ghost of that colour (a building's preview while its site is chosen).
  // `built` above 1.5: a finished unit with its personal shield up (a glow along its outline).
  void Add(const Model* model, const Mat4& world, Vec3 teamColor, float built = 1, const Pose* pose = nullptr, const Vec3* tint = nullptr);
  // After the frame's last Add and before its first Draw: builds the bones of every posed model
  // that one of the frame's views (the camera's, the shadow cascades') will draw, and hands them
  // to the shaders once for all passes.
  void Prepare(const std::vector<const View*>& views);
  // Draws the list as `view` sees it: into the scene, or (`shadowPass`) into a shadow map, where
  // only depth is written. `environment` is the cube map the original look reflects in shiny parts.
  // `glass`: only the see-through levels (ModelLod::glass: shields, frames of light), which the
  // ordinary pass leaves out: after the water, over what is behind them, writing no depth.
  // `part`: 0 everything, 1 the scenery alone (what was added while `addingScenery` was set: trees,
  // rocks), 2 everything else. The scene draws the scenery first, so that what lies on the ground
  // (range rings) can be laid over the trees and under the units, as the original has it.
  void Draw(const View& view, gfx::Texture environment, bool shadowPass = false, bool glass = false, int part = 0);
  bool addingScenery = false;

  // Level of detail is chosen by size on screen, not by distance: a model wishes for
  // `trianglesPerPixel` triangles for every pixel of the circle it covers and gets the level nearest
  // to that wish. So every model costs about the same per pixel, whatever its budgets and however
  // many levels it has. With levels a quarter of each other, the finest is used from about 180
  // pixels across.
  // A model built to the four-level standard (Model::levelPixels) is switched at the sizes its
  // levels were made for instead; its size is taken as three quarters of its bounding sphere's.
  float trianglesPerPixel = 0.55f;
  // The quality setting: scales that wish. 1 is full quality; 0.5 suits a slower card; a very large
  // value forces every model's finest level.
  float detail = 1;
  float minPixels = 1.5f;         // smaller than this on screen: not drawn (a strategic icon's job)
  float shadowMinPixels = 12;     // smaller than this: casts no shadow
  float shadowDetail = 0.25f;     // shadows are drawn from coarser levels: this share of the triangles
  // Numbers from the last scene (not shadow) pass.
  int drawnInstances = 0;
  int drawnTriangles = 0;
  int shadowTriangles = 0;        // summed over the shadow passes since the last scene pass
  int posedInstances = 0;         // of the drawn ones: those with bones away from rest
  float prepareMs = 0;            // what Prepare took on the processor, averaged over the last frames

 private:
  struct Instance {
    float row0[4], row1[4], row2[4];  // the top three rows of the world matrix
    float color[4];                   // team colour, w: fraction complete
    float bones[4];                   // x: where its bones' rows begin in the bone buffer, -1: at rest; yzw: a ghost's tint (0: none)
  };
  struct Entry {
    const Model* model;
    Vec3 centre;   // of its bounding sphere, in the world
    float radius;      // of the sphere it is surely inside (a posed model's is wider)
    float sizeRadius;  // of the model at rest: what its size on screen is counted by
    Instance instance;
    Pose pose;
    bool posed = false;
    bool scenery = false;
    int rows = -1;  // where its bones' rows begin in rows_ (Prepare)
  };
  struct Item {
    const ModelLod* lod;
    Entry* entry;
    float bones;  // Instance::bones.x for this pass
  };
  void BuildRows(Entry* entry);
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_, leaves_, shadow_, glass_, dome_;
  gfx::Buffer boneBuffer_;
  bool countedScene_ = true;
  std::vector<Entry> entries_;
  std::vector<Item> items_;
  std::vector<Instance> batch_;
  // Bones: for every posed model a pass has seen, three rows of four numbers per bone: the matrix
  // that takes a vertex of the model at rest to where the bone has carried it (in the model's own
  // space). rows_ holds them for the frame, in one buffer all passes read.
  std::vector<float> rows_;
  bool prepared_ = false;
};

}  // namespace render
