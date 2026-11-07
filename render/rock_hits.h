// Rocks that answer fire ([rockhits]; World::rockHits; Chris, 2026-11-07: "an environment that reacts to fire"). To be
// seen only: nothing goes back to the simulation, and no prop is moved or taken away by it (the game's props stay where
// the game has them; a rock the game takes away, reclaimed, is gone from the picture as before).
//
// What a hit on a rock does (a shot that lands on or beside one, within its size, or a blast that reaches it):
//  - Chips: a burst of small pieces of rock flies off where it was struck, out from the rock and up, tumbles and falls
//    (debris.h, Debris::Chip: lumps wearing patches of the rock's own texture, its fresh fractures paler stone). Some stay
//    lying round its foot as rubble (a budget of them, `Debris::mostChipRubble`), sinking away after some minutes; the
//    rest sink into the ground within seconds.
//  - Dust: a puff of rock dust where it was struck (the game's own rocky ground's impact dust: its grey "tti_rocky11"
//    emitters, small or medium by the hit's size), settling.
//  - Scars: the rock keeps how much it has been hit. Soot darkens it in patches, the more the darker; cracks open over it
//    as the hits add up (they stay); a fresh hit leaves a coat of pale rock dust on it that fades within half a minute.
//    The soot eases back slowly, to about half of what it was at its worst, over some minutes (rain and wind), never
//    all the way.
// Sized by the hit: a shell's direct hit throws a few small chips and blackens a rock a little; a blast as large as an
// artillery shell's chips and scorches every rock it reaches, the nearer the more.
//
// Which props are rocks: the map's props whose blueprint's file name says so (rock, boulder, stone, crag, cliff, slab,
// pebble: rockset_b_c_prop, boulder_01_prop, rock03_a_prop, fieldstone01_c_prop, the remastered maps' crag and cliff
// props), that do not sway in the wind (no tree or bush named so) and are no stream. Their own props only (none made in
// the game: wrecks are hot metal's).
//
// How a scarred rock is drawn: taken out of its square's group of still scenery (World: held) and drawn on its own, with
// its scars in the model shader's per-instance tint (Tint, below; mesh_renderer.cpp, RockScarSurface): the same model,
// the same place. A scarred rock stays held while the map is open; at most `mostScarred` of them (past it a rock hit
// for the first time chips and throws dust, but keeps its look; so does a rock larger than `scarsUpTo`).
//
// Budgets: a big battle must not flood the picture. Chips in the air and chips lying have their own most (Debris), the
// dust puffs at most `dustPerSecond` (a few more at once), the chips at most `chipsPerSecond` (as many again at once):
// past them a hit throws fewer, or none. The rocks are found by squares of the map (a hit looks at the few rocks round
// it, not at all of them).
// Modern look; off by default (proposed for the "Battle Effects" bundle, with the debris and the blasts' dust).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "linalg.h"
#include "models.h"

namespace render {

class RockHits {
 public:
  // --- tuning ---
  int mostScarred = 600;        // rocks that keep their scars at most
  // Rocks larger than this (half their size across, world units) chip and throw dust but keep their look: a rock's scars are
  // one amount over all of it, and a cliff or a crag blackened all over by one shell's hit at its foot would read wrong.
  float scarsUpTo = 4;
  float sootEaseSeconds = 300;  // how slowly the soot eases back (the time it takes to go about two thirds of the way)
  float sootKept = 0.55f;       // the share of its worst soot a rock keeps for good
  float dustSeconds = 8;        // how quickly a fresh coat of rock dust fades (two thirds of it in that time)
  float chipsPerSecond = 60;    // chips thrown a second at most, over time (twice as many at once)
  float dustPerSecond = 10;     // dust puffs a second at most, over time (twice as many at once)
  // The game's own impact dust of rocky ground (grey), for a small hit and a larger one.
  std::string dustSmall = "/effects/emitters/tti_rocky11_small01_01_emit.bp";
  std::string dustLarge = "/effects/emitters/tti_rocky11_medium01_01_emit.bp";

  // Whether a map's prop is a rock (above), by its blueprint's path and its model as drawn.
  static bool IsRock(const std::string& blueprint, const Model* model);
  // The map's rocks anew (a map opened): Begin with the number of its props, Add each rock (its index in the map's list,
  // its model and its place), then Finish with the map's size. Their scars forgotten.
  void Begin(size_t props);
  void Add(uint32_t prop, const Model* model, const Mat4& world);
  void Finish(float mapWidth, float mapHeight);
  bool Made() const { return made_; }
  size_t Count() const { return rocks_.size(); }
  // Every rock's scars forgotten (a game started over; the switch off): none held any more.
  void Forget();

  // A hit at `at` of `size` (world units: about its radius; a shell's direct hit some 0.25), at `clock` (seconds on the
  // game's clock). The rocks it touches (a rock is touched where `at` lies within its box widened by the hit's size and
  // a little), each with where on it it was struck, which way out from it, and how hard (0..1, by how near it was):
  // their scars are added to here; what flies off is the caller's (Touched). Returns how many.
  struct Touched {
    uint32_t prop = 0;
    Vec3 at, out;          // on its surface, and out from it (unit length)
    float strength = 0;    // 0..1
    float across = 1;      // the rock's size across (world units): its chips are smaller than a small rock
    bool fresh = false;    // its first scar: the caller takes it out of its group (held)
    const Model* model = nullptr;
  };
  // `way`: which way the shot was going (unit length; none: not known): what it strikes flies back against it.
  int Hit(Vec3 at, float size, double clock, std::vector<Touched>* out, Vec3 way = {});
  // The budgets: how many of `want` chips may be thrown now, and whether a puff of dust may (each takes what it gets).
  int TakeChips(int want, double clock);
  bool TakeDust(double clock);

  // The scars eased on to `clock`.
  void Update(double clock);
  // Whether a prop of the map is held out of its group (its index); those held, in the order they were first hit.
  bool Held(uint32_t prop) const { return prop < held_.size() && held_[prop]; }
  const std::vector<uint32_t>& HeldList() const { return heldList_; }
  // Its scars as the model shader reads them, in the instance's tint (a ghost's colour, which is never negative): x -0.35
  // less its fresh dust's share times 0.1 (so between -0.35 and -0.45: above -0.5, which the vertex shader takes for a
  // tank's treads or a factory's belt, and no hot wreck's -0.25: hot_metal.h), y its soot times -0.49, z -0.001 less its
  // cracks (0..1).
  Vec3 Tint(uint32_t prop) const;

  int hits = 0, touched = 0;  // hits told and rocks they touched, since the map was opened

 private:
  struct Rock {
    uint32_t prop = 0;
    const Model* model = nullptr;
    Mat4 world, toModel;   // its place (the model's own units into the world), and back
    float scale = 1;       // the model's units to the world's (its largest)
    Vec3 centre;           // its box's middle in the world
    float radius = 1;      // of the sphere round its box
  };
  struct Scar {
    float soot = 0, worst = 0, crack = 0, dust = 0;
  };
  std::vector<Rock> rocks_;
  std::vector<std::vector<uint32_t>> cells_;  // rocks (indices into rocks_) by squares of kCell, each listed where its sphere reaches
  int cellsX_ = 0, cellsZ_ = 0;
  float largest_ = 0;  // the largest rock's radius
  static constexpr float kCell = 16;
  bool made_ = false;
  std::vector<uint8_t> held_;
  std::vector<uint32_t> heldList_;
  std::unordered_map<uint32_t, Scar> scars_;
  double clock_ = -1;
  double chipClock_ = -1, dustClock_ = -1;
  float chipsLeft_ = 0, dustLeft_ = 0;
};

}  // namespace render
