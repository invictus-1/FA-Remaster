// Bounce light (the lighting upgrade's ray tracing, docs/render-lighting.md step 4; World::bounceLight, viewer
// --bounce): the light the ground, the hills and what stands on them throw back, from probes over the ground round
// the camera that rays update a little every frame (after DDGI: Majercik et al., "Dynamic Diffuse Global
// Illumination with Ray-Traced Irradiance Fields", 2019, shaped for a game seen from above).
//  - The probes: a grid of 32 x 32 columns `spacing` apart round the camera's target, moved in whole columns as it
//    goes (a column that comes in is started afresh, the others keep what they had), and in each column four
//    probes at `layers` heights over the ground under it: the probes follow the ground.
//  - Every frame each probe casts 64 rays (evenly round, turned anew each frame) through the ray tracing scene
//    (ray_scene.h). What a ray meets is lit by the sun (a ray toward it says whether it reaches) and by the probes'
//    own light of the frame before, so light bounces again and again over frames; a ray that meets nothing brings
//    the sky's light.
//  - Each probe keeps the light coming in from every side (an octahedral map, 6 x 6 with a border of one, RGBA16F)
//    and how far it sees that way (the mean distance and its square, 14 x 14 with a border, RG16F), each blended
//    with what it held (`keep`), and a probe that sees a wall closer than a place does not light that place.
// Every lit surface of the modern look reads the eight probes round it (view.h, BounceAt) in place of the sky's
// fitted light, which stays outside the grid. Where the device has no ray tracing nothing of it runs.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class RayScene;

class BounceLight {
 public:
  explicit BounceLight(gfx::Device* device) : device_(device) {}
  ~BounceLight();
  bool Init(std::string* error);
  // At the frame's start: the grid round `target`, the ground's heights under its columns (`groundHeight` at a
  // place x, z), and the view's values that say where the probes are (ViewConstants::bounce, bounceLayers).
  void Place(Vec3 target, const std::function<float(float, float)>& groundHeight, ViewConstants* constants);
  // Between passes, the ray tracing scene built: the probes' rays, then their light and sight blended in.
  void Update(const View& view, RayScene& rays, gfx::Texture groundNormals, gfx::Texture groundColours, gfx::Texture mapSky);
  // The probes' pictures where every shader reads them (view.h: kBounceLightSlot, kBounceSightSlot, kBounceColumnsSlot).
  void Bind();

  static constexpr int kColumns = 32, kLayers = 4, kRays = 64;
  float spacing = 8;                       // world units between columns
  float layers[kLayers] = {1.5f, 5, 12, 26};  // the probes' heights over the ground
  float keep = 0.97f;                      // what a probe keeps of what it held each frame
  float reach = 300;                       // how far a ray looks (world units)

 private:
  gfx::Device* device_;
  gfx::Pipeline trace_, light_, sight_, lightBorder_, sightBorder_, keep_;
  gfx::Texture rays_, lightAtlas_, sightAtlas_, columns_, identities_;
  float heights_[kColumns * kColumns] = {};
  int firstX_ = 0, firstZ_ = 0;
  uint32_t frame_ = 0;
  bool placed_ = false;
};

}  // namespace render
