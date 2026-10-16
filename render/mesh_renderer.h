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
  // Scenery that stands still (trees, rocks), in groups (one square of the map's): the renderer keeps
  // each group from one frame to the next, so a frame only says which groups it lists (ListGroup,
  // after Begin), not where each tree stands and how large it is. A group is filled anew
  // (ClearGroup, then AddToGroup for each) when something in it has changed. It counts as scenery.
  // The groups' models come first in the frame's list, in the order they are listed.
  uint32_t AddGroup();  // a new, empty group's number
  void ClearGroup(uint32_t group);
  void AddToGroup(uint32_t group, const Model* model, const Mat4& world);
  void ListGroup(uint32_t group) { groupsListed_.push_back(group); }
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
  // The scene's solid models that are large on screen first lay their depth alone (with the holes of
  // leaves and of what is being built), and are then shaded writing no depth, so that the full shading
  // runs once a pixel, for what is in front, and not again for every leaf behind it. The same picture.
  // ON (2026-10-15, RTX 4090, 1440p, the best of three runs each): units and scenery on the rebuilt
  // Seton's at middle zoom 2.49 -> 1.84 ms, from far 1.97 -> 1.82; the stock map and a battle the same
  // either way. (A first try kept writing depth in the full shading: the card then tests depth only
  // after shading what cuts holes, and it saved nothing. Small models are left out: drawing them twice
  // cost more than it spared.) Viewer --no-prepass: off.
  bool depthPrepass = true;
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
  // What a pass tests first, for every model of the list, kept apart from the rest so that going
  // through tens of thousands of them reads little memory.
  enum : uint8_t { kScenery = 1, kPosed = 2, kHasGlass = 4, kHasSolid = 8 };
  struct Cull {
    Vec3 centre;
    float radius;
    float sizeRadius;
    uint8_t flags;
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
    uint8_t flags = 0;  // Cull's
  };
  Entry MakeEntry(const Model* model, const Mat4& world, Vec3 teamColor, float built, const Vec3* tint) const;
  void Push(const Entry& e) {
    entries_.push_back(e);
    culls_.push_back({e.centre, e.radius, e.sizeRadius, e.flags});
  }
  struct Item {
    const ModelLod* lod;
    Entry* entry;
    float bones;  // Instance::bones.x for this pass
    bool near;    // large on screen (depthPrepass: only these have their depth laid first)
  };
  void BuildRows(Entry* entry);
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_, leaves_, shadow_, glass_, dome_, adding_;
  gfx::Pipeline prepass_, prepassLeaves_;  // (depthPrepass: depth alone, the scene's samples)
  gfx::Pipeline laid_, laidLeaves_;        // (depthPrepass: then the full shading, writing no depth)
  // two of the game's lookup textures for the original look (where the game's files have them):
  // the Cybran's highlight by how the sun's mirror image and the sun itself stand to the surface,
  // and the Seraphim's ramp of how strongly a surface mirrors by the angle it is seen at
  gfx::Texture insectLookup_, seraphimFalloff_;
  bool haveInsectLookup_ = false, haveSeraphimFalloff_ = false;
  // the visuals track's lattice for personal shields, a layer per faction (where it has made one)
  gfx::Texture shieldPatterns_;
  bool shieldPatternOf_[4] = {}, triedShieldPatterns_ = false;
  gfx::Buffer boneBuffer_;
  bool countedScene_ = true;
  std::vector<Entry> entries_;
  std::vector<Cull> culls_;  // one for each of entries_, in the same order
  struct Group {
    std::vector<Entry> entries;
    std::vector<Cull> culls;  // one for each of entries
    // the box round all of their bounding spheres: a pass that does not see it passes the group over
    // whole (what each of them would have been tested for one by one)
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  };
  std::vector<Group> groups_;
  std::vector<uint32_t> groupsListed_;
  void Consider(const Cull& c, Entry& e, const View& view, bool shadowPass, bool glass, int part, uint8_t needed, float focal,
                float smallest, float wish, std::vector<Item>* out) const;
  // A pass's models are tested in pieces of up to kPieceModels, spread over the processor's cores;
  // each piece's items are put together in the pieces' order afterwards (the same order as one loop).
  static constexpr size_t kPieceModels = 1024;
  struct Piece {
    const Cull* culls;
    Entry* entries;
    size_t count;
  };
  std::vector<Piece> pieces_;
  std::vector<std::vector<Item>> pieceItems_;
  std::vector<Item> sorting_;     // (the sort's second buffer)
  std::vector<uint32_t> buckets_;  // (and its counts)
  void SortItems();
  void DrawItems(const View& view, gfx::Texture environment, bool shadowPass, bool glass);
  std::vector<Item> items_;
  std::vector<Instance> batch_;
  // Bones: for every posed model a pass has seen, three rows of four numbers per bone: the matrix
  // that takes a vertex of the model at rest to where the bone has carried it (in the model's own
  // space). rows_ holds them for the frame, in one buffer all passes read.
  std::vector<float> rows_;
  bool prepared_ = false;
};

}  // namespace render
