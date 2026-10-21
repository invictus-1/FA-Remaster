// Plants that react (World::plantsReact; Chris, 2026-10-18, the visuals session's proposal 5): the modern look's grass,
// ground plants and undergrowth answer what happens on them, to be seen only. Nothing goes back to the simulation, which
// owns the props: the trees it knocks over, burns and takes away are drawn as it says.
//  - Pressed: where units drive, the grass and the plants are laid over the way the unit pushes them and rise again over
//    some seconds after it has gone; the map's small props that sway (bushes, ferns, saplings, shrubs, grass tufts) bend
//    over under it and spring back. A field over a square round what the camera looks at keeps for each place how hard
//    it was pressed, which way and how long ago (a compute pass a frame, reading the last frame's field: Prepare).
//  - Burnt: where an explosion scorched the ground (the game's blasts, blasts.h: a scorch or a crater a script lays, a
//    death's flash, an area's damage) and where a fire burns on (a burning tree, a wreck, napalm: the effects' lasting
//    fires, Particles::HotEmitters; a fresh wreck: AddWreck), the ground, the grass, the plants and the undergrowth turn
//    black, then brown, the plants lower, and grow back over some minutes. A picture over the whole map keeps how hard and
//    how freshly each place was burnt (burns drawn into it, blended, so that a place burnt again is burnt anew; faded a
//    little every half second of the game: growing back and growing older).
//  - What the game fells (a tree knocked over: lying, its up turned away from the sky) no longer sways in the wind, and
//    what it burns (a tree's burnt mesh, ModelCache::Wreck) loses about half its needles: a charred skeleton. The game's
//    trees (Model::simTree) are never pressed nor burnt by the picture alone.
// Shaders read it through kPlantsShaderBlock (its functions take Constants' two float4); the grass (grass.h) and the
// models' shaders (mesh_renderer.cpp) have variants with it, made the first time it is on.
#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "blasts.h"
#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "particles.h"
#include "view.h"

namespace render {

// Before it: the registers of its two textures, as `#define PLANTS_PRESS t9` and `#define PLANTS_BURN t10` (each
// shader its own free ones). PlantsPressAt(xz, k0, k1): x, z which way it was laid times how hard (0..1), z how hard,
// w seconds since times how hard (0: not pressed); PlantsBurnLook(xz, k0, k1, random): x how black, y how brown, z
// how much of its height is gone, w embers (each 0..1; `random` 0..1 its own, ragging the burn's edge);
// PlantsBurnt(xz, k1) and PlantsBurnLookOf(burnt, random): the same in two steps; PlantsBurnColour(colour, look): a
// linear colour burnt so; PlantsEmber(ember, xz, time): its embers' light; PlantsUpright(up): 1 standing, 0 lying.
extern const char kPlantsShaderBlock[];

class PlantLife {
 public:
  explicit PlantLife(gfx::Device* device) : device_(device) {}
  ~PlantLife();

  // A map opened (World::Open): its size in world units. Forgets every burn and pressing.
  void SetMap(float mapWidth, float mapHeight);
  // A burn at (x, z): `radius` world units, `strength` the share of a full burn laid over what is there (0..1), at
  // `born` (the effects' clock, seconds: earlier than now for one laid as if then: tests). Drawn at the next Prepare.
  void AddBurn(float x, float z, float radius, float strength, double born);
  // The game's blasts (blasts.h): each that is new, on the ground (not the water, not high on a unit or in the air), a
  // burn as large as its scorch, half a second after it (when the scorch and the flash of one explosion are one).
  void WatchBlasts(const Blasts& blasts, double clock, const std::function<float(float, float)>& ground);
  // The lasting fires (Particles::HotEmitters, `hot`): each that stands still on the ground burns the ground under it,
  // the longer the more.
  void WatchFires(const std::vector<Particles::Hot>& hot, double clock, const std::function<float(float, float)>& ground);
  // A fresh wreck (its middle, its bounding radius; when it was made): its death burnt the ground round it. Told again
  // for the same place within half a minute (a wreck drawn frame after frame): once.
  void AddWreck(float x, float z, float radius, double born);
  // [scars] A wreck gone (reclaimed, or shot to pieces) at (x, z) at `when`: what burnt within `radius` of it - its death's
  // burn, its fires' - goes over the next minute or so (Chris, 2026-10-20: "stuff that has been reclaimed as in those
  // black masses should leave after some time"): browner at once, then thinning out by its patches, gone by about
  // a minute and a quarter (`healing` seconds: e to the minus the time over a fifth of it).
  void Heal(float x, float z, float radius, double when);
  float healing = 150;
  // A frame, before the scene's passes (it draws and dispatches): what presses this frame (`boxes`, 8 floats each: x, z of
  // the middle, its forward way's x, z (a unit vector), half its length, half its width, 0, 0: units standing on the
  // ground), the clock. The field round what `camera` looks at; nothing pressed when the camera is far.
  void Prepare(const View& view, const Camera& camera, const std::vector<float>& boxes, double clock);
  // The square the field covers round what `camera` looks at (x0, z0, x1, z1, world units), as Prepare places it; false
  // when the camera is too far for anything to be pressed. (What the host need not go through for its boxes: the game's
  // wrecks, World::PreparePlants.)
  bool FieldSquare(const Camera& camera, float square[4]) const;
  // The ground where it burnt, into the scene's colour alone (the decals' pass: after the ground and its marks, before
  // anything stands on it; `sceneDepth` the scene's depth with the ground in it): ash black, then a burnt brown, as the
  // grass on it. Nothing where nothing has burnt for half an hour.
  void DrawGround(const View& view, const Camera& camera, gfx::Texture sceneDepth);
  bool AnyBurnt() const { return ready_ && Burning(); }
  // What the shaders read (after Prepare): the field, the burnt picture (none: not made), and the constants for
  // kPlantsShaderBlock (two float4: the field's first texel's x, z, 1 / its width, the clock; 1 / the map's width and
  // height, 1 when there is a burnt picture, 1 when the field is there this frame).
  gfx::Texture Press() const { return press_[current_]; }
  gfx::Texture Burnt() const { return burnt_; }
  const float* Constants() const { return constants_; }
  bool Ready() const { return ready_; }

  float farthest = 120;     // no pressing when the camera is farther from what it looks at (world units)
  float regrow = 300;       // seconds over which what is burnt grows back to a third (the burnt picture fades so)
  int burnsLaid = 0;        // (the last Prepare: burns drawn)
  int boxesPressing = 0;    // (and the boxes gone through)

 private:
  bool Init();
  void FieldMiddle(const Camera& camera, float* x, float* z) const;  // (the field's middle: Prepare's, FieldSquare's)
  bool Burning() const { return burnt_ && lastBurnt_ > -1e30 && lastClock_ - lastBurnt_ < 1800; }  // (AnyBurnt's, before ready_)
  gfx::Device* device_;
  int tried_ = 0;  // 0 not yet, 1 made, -1 they cannot be
  gfx::Pipeline pressPass_, burnSplat_, burnFade_, ground_;
  gfx::Pipeline burnHeal_;  // [scars] (Heal's circles: the burnt picture multiplied down in them)
  gfx::Texture press_[2], burnt_;
  int current_ = 0;
  bool pressValid_ = false;   // (the last field is there to read)
  float pressOrigin_[2] = {0, 0};
  bool burntFresh_ = true;    // (the burnt picture to be cleared)
  float mapWidth_ = 0, mapHeight_ = 0;
  double lastClock_ = -1, fadedAt_ = -1;
  double blastsSeen_ = -1;    // (blasts this old and younger are not burnt yet)
  double lastBurnt_ = -1e31;  // (when the latest burn was laid, as it says)
  double pressedAt_ = -1;     // (when something last pressed in the field)
  struct Burn {
    float x, z, radius, strength;
    double born;
  };
  std::vector<Burn> burns_;   // (to be drawn at the next Prepare)
  std::unordered_map<uint32_t, Vec3> fireAt_;  // (each fire's place the frame before: those that move burn nothing)
  std::vector<Burn> wrecks_;  // (the wrecks of the last half minute that burnt: AddWreck's)
  std::vector<Burn> heals_;   // [scars] (Heal's, while under way: x, z, radius, -, when it began)
  float constants_[8] = {};
  bool ready_ = false;
};

}  // namespace render
