// Ripples on the water (World::waterRipples; the modern look's water, water.cpp): shells that land in the
// water, ships, hover units and amphibious units wading leave rings and wakes that spread, run through each
// other and die out. Purely for the eye: nothing of it goes back to the game.
//
// A square of the water round what the camera looks at is a grid of heights on the card (kCells a side), moved
// on by the wave equation in a compute pass, at a fixed sixty steps a second of the game's clock (a paused game's
// ripples stand still). Each step:
//   next = now + (now - before) x kept + k^2 x laplacian(now)    (k: the speed of the waves in cells a step,
//   slower where the water is shallow; the Laplacian over the eight cells round it, so that rings stay round),
//   then what was put into the water since the last step.
// Where the ground stands above the water a cell is held at 0 (the ripples stop at the shore, and shallow water
// takes the last of them); toward the square's edges the ripples are let die out (a sponge), so that none come
// back from the edge. The square follows the camera in whole cells, so the field never swims under the eye, and
// the size of a cell follows the camera's distance in steps of two (a cell two to three pixels across), the field
// carried over from one size to the next.
//
// What is put in: round bumps of the surface, each a place, a radius and an amount (Add: a shell's splash, its
// amount swinging up and down for half a second or so, so that it leaves a train of rings; AddHull: what a hull
// pushes ahead of it and leaves behind as it moves). The water's shader reads the field's heights and slopes
// (Surface) and tilts its surface by them, with a little foam on the steepest crests.
//
// Off (or with nothing put in for a few seconds) nothing is done and the water is drawn as without it.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"

namespace render {

struct MapData;

class Ripples {
 public:
  explicit Ripples(gfx::Device* device) : device_(device) {}
  ~Ripples();

  // The map the water lies on (its heights say where the shore is) and the water's level. Ripples are kept only
  // where it has water.
  void SetMap(const MapData* map);

  // --- each frame ---
  // At the frame's start (World::Begin): `clock` the game's time in seconds; `on` the option and what it needs
  // (the modern look, a map with water); `side` a frame drawn for something else (the minimap): nothing moves on
  // then and the water there shows no ripples.
  void Begin(double clock, bool on, bool side);
  // A disturbance of the water: a splash at (x, z), `radius` world units across its middle (a shell's: about
  // 0.5; a large shell 1), `strength` 1 for a usual shell, up to 4 or so for a blast: rings run out from it and
  // die in two or three seconds. Anyone may call it between Begin and Render (World::Render moves the field on).
  // Nothing happens where there is no water, or when ripples are off.
  void Add(float x, float z, float radius, float strength);
  // Something that may stand, float or wade in the water, every frame it is drawn (alive or in its death): `id`
  // which thing it is (an entity id, the same each frame), its place and turn, its bounds in its own frame in
  // world units (the model's times its scale). From how it moved since the last step: a moving hull's bow and
  // stern push the water (ships, hover units over it, units wading in the shallows), a hull sinking or coming
  // down into the water makes rings. Things well clear of the water, or under it, are left out.
  void AddHull(uint32_t id, Vec3 position, const Quat& orientation, Vec3 lo, Vec3 hi);
  // A shot of a game's frame (FrameProjectile, each drawn frame): where it is, whether it has hit something this
  // beat (`stopped`), its size (radius of its model, 0 when it has none). It splashes once, when it hits the
  // water or goes down through it (a torpedo dropped from the air).
  void AddShot(uint32_t id, Vec3 position, bool stopped, float size);
  // Moves the field on (between passes: it dispatches). From World::Render, before the scene's pass.
  void Update(const Camera& camera, int viewHeight, gfx::Texture heights, float heightUnit);
  // Whether the water reads the field this frame, the field's heights and slopes (RGBA16F: height, its slope
  // along x and along z, 0), and where it lies: x, y its first cell's corner (world x, z), z 1 / its side's
  // length, w a cell's size (world units).
  bool Active() const { return active_; }
  bool On() const { return on_ && !side_; }  // (this frame moves the field on: Update is to be called)
  gfx::Texture Surface() const { return surface_; }
  const float* Area() const { return area_; }

  // Tuning (viewer --ripple-*): how fast the rings run (world units a second, in deep water), and after how many
  // seconds they are half as high.
  float speed = 1.6f;
  float halfLife = 0.8f;
  // Splashes guessed from the drawn frame: AddShot's, and AddHull's for something coming down into the water. A host
  // that hands over the engine's impacts (sim_view.h ImpactEvent: 'Water', 'Underwater', a crash in 'Water') to Add
  // sets it false, so that nothing splashes twice. (The hulls' wakes and a sinking ship's rings stay either way.)
  bool guessSplashes = true;
  // Set by World each frame: the host's simulation hands over its impacts (World::AddCombat), and World puts in their
  // splashes itself: the guesses stop then too, whatever guessSplashes says.
  bool impactsGiven = false;
  // A view for checking (viewer --ripple-view n): the water shows the field's heights in place of itself, red where
  // it stands up, blue where down, full at 1 / n world units; 0 off.
  float view = 0;
  int Live() const { return static_cast<int>(splashes_.size()); }  // splashes still putting rings in
  int Added() const { return added_; }                             // splashes added since the map was opened
  int Entries() const { return lastEntries_; }                     // bumps put in at the last step
  // Makes the field's compute pipelines and its 24 MB of targets, the first time (Water::PrepareRipples: at the first
  // frame the option is on); whether they are there.
  bool Ready();

  static constexpr int kCells = 1024;  // a side of the square
  static constexpr int kMostEntries = 256;  // bumps put in a step (the nearest kept)

 private:
  struct Entry {
    float x, z, radius, amount;
  };
  struct Splash {
    float x, z, radius, strength;
    double born;
    float period;    // of its swing (seconds)
    float last = 0;  // the displacement put in so far (it swings as Swing says)
  };
  struct Hull {
    Vec3 place;     // where it was at the last step it was looked at
    float bottom = 0;
    double clock = -1;
    uint32_t stamp = 0;  // the frame it was last seen in
  };
  struct Shot {
    float y = 0;         // its height when last seen
    uint32_t stamp = 0;  // the frame it was last seen in
    bool done = false;   // it has made its splash
  };
  float Swing(const Splash& s, double at) const;  // the displacement a splash has put in by then
  void Put(float x, float z, float radius, float amount);  // (a negative radius: with a hollow round it, Step)

  gfx::Device* device_;
  const MapData* map_ = nullptr;
  float level_ = 0;
  bool haveWater_ = false;
  gfx::Pipeline step_, carry_;
  bool tried_ = false, made_ = false;
  gfx::Texture now_[2], before_[2], surface_;  // the heights at this step and the one before (two sets: read one, write the other)
  int cur_ = 0;
  bool on_ = false, active_ = false, fresh_ = true, side_ = false;
  double clock_ = 0, simClock_ = -1, lastPut_ = -1e9;
  int stepsDue_ = 0, lastEntries_ = 0, added_ = 0;
  // the square: its first cell (in cells of `cell_`) and a cell's size, as the field holds them now
  int originX_ = 0, originZ_ = 0;
  float cell_ = 0;
  float area_[4] = {0, 0, 0, 0};
  std::vector<Entry> entries_;  // put in since the last step
  std::vector<Splash> splashes_;
  std::unordered_map<uint32_t, Hull> hulls_;
  std::unordered_map<uint32_t, Shot> shots_;
  uint32_t stamp_ = 0;
};

}  // namespace render
