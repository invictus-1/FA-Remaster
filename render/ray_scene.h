// The world as the card's ray tracing sees it (gfx.h's structures; the lighting upgrade's ray tracing,
// docs/render-lighting.md step 4), built again every frame where the device can (gfx::Device::RayTracing):
//  - the ground: a grid of its heights, every fourth point; the water's surface over it, where the map has water
//    (on a mask of its own, 2: what is under it is in the sun's light, so rays toward the sun, on mask 1, pass
//    through);
//  - every model the frame draws, by its coarsest solid level (what the rays are for - light thrown back - is soft:
//    the shapes need not be exact; a posed unit stands as it does at rest);
//  - for each model's level, a colour: its albedo's average (worked out once on the card, from a small level of
//    its texture, the holes of leaves left out) and how much of it its team colour takes (its specular's alpha);
//    for leaves (a level with holes), how much of it is leaf instead: a ray takes one of its triangles with that
//    chance (they are not opaque), so over many rays a canopy lets through what its holes would.
// A shader reads the scene (Scene, kSceneSlot), the colours (Colours: RGBA8, entry e at (e % 1024, e / 1024),
// rgb the albedo as stored, a the team colour's share or, for leaves, their coverage) and what each instance is
// (InstanceData, a StructuredBuffer of uint4 by the instance's id: x its colour entry; y flags: 1 the ground, 2 the
// water, 4 scenery (no team colour), 8 leaves; z its team colour as RGBA8).
//
// A check view (Check; World::rayView, viewer --ray-view) traces a ray from the camera through each pixel, the
// sun's shadow with another, and shows what they meet lit simply: the ground by its own normals and a colour of
// its own, models by their colours facing the ray.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "view.h"

namespace render {

class MeshRenderer;
struct MapData;

class RayScene {
 public:
  explicit RayScene(gfx::Device* device) : device_(device) {}
  ~RayScene();
  // Makes its passes. False where the device has no ray tracing (nothing else then does anything).
  bool Init(std::string* error);
  bool Available() const { return available_; }
  // The ground of a map just opened (and again where the simulation changes it): its heights every
  // `step` points, as a grid of triangles.
  void SetGround(const MapData& map, int step = 4);
  // Every frame, between passes, once the models of the frame are added and prepared: the scene of them and
  // the ground (it dispatches, for models seen the first time, and builds the scene).
  void Build(const MeshRenderer& meshes);
  gfx::Structure Scene() const { return scene_; }
  gfx::Texture Colours() const { return colours_; }
  gfx::Buffer InstanceData() const { return instanceData_; }
  int Instances() const { return static_cast<int>(info_.size()); }
  // The check view, for a scene `view` (one sample a pixel): the picture to show in place of the scene's
  // (none where it cannot be made). `groundNormals`: the terrain's normal map; `groundColor` linear.
  gfx::Texture Check(const View& view, gfx::Texture groundNormals, Vec3 groundColor);
  static constexpr int kSceneSlot = 33;  // where shaders find the scene (t33)

 private:
  struct Level {
    gfx::Structure structure;
    uint32_t colour = 0;
    bool leaves = false;  // (not opaque: taken by chance)
  };
  gfx::Device* device_;
  bool available_ = false;
  gfx::Pipeline average_, check_;
  gfx::Structure scene_, ground_, water_;
  gfx::Buffer groundVertices_, groundIndices_, waterVertices_;
  float mapWidth_ = 1, mapHeight_ = 1;
  gfx::Texture colours_, picture_;
  int pictureWidth_ = 0, pictureHeight_ = 0;
  gfx::Buffer instanceData_;
  std::unordered_map<uint32_t, Level> levels_;  // by ModelLod::serial
  uint32_t nextColour_ = 0;
  std::vector<gfx::SceneInstance> instances_;
  std::vector<uint32_t> info_;  // four for each instance (InstanceData)
};

}  // namespace render
