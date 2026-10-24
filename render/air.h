// Light in the air (the modern look's volumetric fog; docs/render-lighting.md, step 1). A volume laid
// over the view: the picture's width and height in cells (160 across), and its depth in cells spaced
// by the log of the distance from the eye. Each frame compute shaders fill it:
//  - Gather: for each cell, how thick the air is there (the haze, the mist on low ground, a little dust
//    always) and the light it throws toward the eye: the sun through its shadow maps and the clouds'
//    shadow, the sky from all sides, the lamps from the light grid through their shadow maps. Each
//    frame takes its sample at another depth inside the cell and blends with what the cell held last
//    frame (read where the cell's middle was then), so the volume settles smoothly.
//  - Sum: along each column from the eye outward, the light gathered and what still comes through.
// (airfix) Both only above the ground: a cell gathers over its stretch above it, and a column sums nothing past where
// it meets it, which goes into the summed volume's last layer; AirAt takes the cell a place lies in only as far as the
// place, by how the mist is spread over it (thickest low down). Before, the cells under the ground held the thickest
// mist and the cell's share of the distance was taken: rings of fog round the eye, one to a cell.
// Every surface's shader then sees what is behind the air through it (view.h, AirAt, ThroughAirTo),
// in place of the haze and mist formulas; so do particles, trails and the water. The lamps' beams
// show in it with their shadows: the billboards of shafts.h are left out while it is on.
#pragma once
#include <utility>  // [views] (std::swap)
#include <cstdint>
#include <string>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class Lights;

class Air {
 public:
  explicit Air(gfx::Device* device) : device_(device) {}
  ~Air();
  bool Init(std::string* error);

  // Fills this frame's volume and says where it is in `view` (its constants' `air`). After the shadow
  // maps are drawn and the lights built (Lights::Build), before the scene's passes: it dispatches, so
  // it lies between passes. When off it only clears the view's `air`.
  // `floor`: the height the haze and dust thin above (the map's usual ground).
  void Build(View* view, const Camera& camera, gfx::Texture sunShadows, gfx::Texture lampShadows, Lights* lights, bool on, float floor);
  // In place of Build for a frame drawn for something else (World::sidePicture): none this frame, and what
  // it holds of the frames before is kept for the next Build.
  void Skip(View* view);
  // (airfix) A weather cut (World::weatherCut): the next Build keeps nothing of the frames before, so a sudden change of the
  // moment or the weather shows at once (half a second at its usual `keep` otherwise).
  void Restart() { haveLast_ = false; }
  // The summed volume, for the scene's shaders (kAirSlot); none while it is off.
  gfx::Texture Volume() const { return built_ ? summed_ : gfx::Texture{}; }

  // The look, all changeable between frames.
  int across = 160, deep = 64;         // the volume's cells across the picture and along the depth
  float nearest = 1, farthest = 4000;  // world units from the eye the volume holds
  float dust = 0.00015f;               // the air's own thickness at the ground (per world unit): lamps' beams show in clear air
  float thinsOver = 300;               // the haze and the dust thin to 1/e over this height above the floor
  // How much of each light the air throws toward the eye. The lamps' strengthens their light on the clear
  // air's dust alone (mist and fog scatter it as they are): in clear air a lamp's beam would hardly show,
  // and the game's lamps are meant to be seen.
  float sunInAir = 1, skyInAir = 1, lampsInAir = 150;
  float pointsInAir = 15;       // the same for the other small lights (flashes of explosions, glows): a battle is not to be a white cloud
  float lampGlowInAir = 0.25f;  // of a beam's glow to all sides near its lamp (lights.h), what the air shows (all of it made halos)
  float sunForward = 0.6f, lampForward = 0.35f;      // how much of it goes on forward (0: alike to all sides)
  float keep = 0.9f;                   // how much of last frame's a cell keeps
  // [sunlight] The light shafts (sunlight.h, World::lightShafts), set by the world before Build: the low band's haze and
  // the haze under the clouds thicken the air, and the band's sunlight near the eye is left to the shafts' pass; `heights`
  // the ground's (Terrain::HeightTexture), `woods` where the woods are (Sunlight::Woods). Off (or none): the gather as
  // without them.
  bool sunlight = false;
  gfx::Texture heights, woods;
  // (airfix) The ground, set by the world before Build (with `heights`): no air is gathered under it, and nothing is
  // summed past where each column of cells meets it (view.h, AirAt). Its heights' unit (Terrain::HeightUnit; 0: no
  // ground known, the cells go on under it as before), the map's width and height, the water's level (-1e9: none).
  float heightUnit = 0, mapWidth = 0, mapHeight = 0, waterLevel = -1e9f;

  // [views] What it keeps for one view (its pictures at that view's size, what it keeps of the frames before) swapped
  // with another's, so each of a picture's views has its own (views.h, ViewSide); its pipelines and settings stay.
  void SwapView(Air& o) {
    std::swap(cells_, o.cells_), std::swap(summed_, o.summed_), std::swap(width_, o.width_), std::swap(height_, o.height_);
    std::swap(depth_, o.depth_), std::swap(now_, o.now_), std::swap(frame_, o.frame_), std::swap(built_, o.built_);
    std::swap(haveLast_, o.haveLast_), std::swap(counted_, o.counted_), std::swap(lastViewProj_, o.lastViewProj_);
    std::swap(lastEye_, o.lastEye_), std::swap(lastNearest_, o.lastNearest_), std::swap(lastFarthest_, o.lastFarthest_);
  }
 private:
  bool Size(int width, int height);

  gfx::Device* device_;
  gfx::Pipeline gather_, sum_;
  gfx::Pipeline gatherSunlight_;  // [sunlight] (made the first time the light shafts are on)
  bool sunlightTried_ = false;
  gfx::Texture cells_[2];  // what each cell gathered: this frame's and last frame's, in turn
  gfx::Texture summed_;    // from the eye to each cell's far end: rgb the light gathered, a what comes through
  int width_ = 0, height_ = 0, depth_ = 0;
  int now_ = 0;            // which of cells_ this frame writes
  uint32_t frame_ = 0;
  bool built_ = false, haveLast_ = false;
  float counted_ = 0;  // (airfix) the places each cell has gathered since a fresh start (a weather cut)
  Mat4 lastViewProj_;
  Vec3 lastEye_;
  float lastNearest_ = 0, lastFarthest_ = 0;
};

}  // namespace render
