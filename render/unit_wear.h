// What fighting and the weather leave on units (the modern look): battle scars and snow, each a player's option
// (World::unitScars, World::unitSnow), off by default. The renderer only reads the game: it sees which shots stopped
// where (FrameProjectile::stopped), where weapons' beams end, and every unit's health from beat to beat.
//
// Battle scars. A unit keeps marks of the hits it survived, on the model at rest (world units, the unit's own frame,
// each on the part of the model its bone carries), so that they go with it and its turning parts: where the shot's
// line first meets the model's own surface (its hit shape, below), not the game's larger collision box. Each mark is of
// the kind of what made it, by the shot's blueprint (KindOf):
//   kinetic   (gauss, cannons, guns): a dent, its torn rim and scratches along the shot's way down to bare metal;
//   energy    (lasers, plasma, disruptors and the like, and every beam): melted metal stretched along the way,
//             heat-tinted round it, glowing for a moment and cooling;
//   explosive (missiles, artillery, bombs, torpedoes): a scorched hole and soot spreading round it;
//   fire      (napalm, flame): soot rising from it.
// Where its health dropped and no shot stopped on it, the nearest blast round it marks the side that faced it
// (explosive), or else a plain dent where no hit was seen (a replay's recording has no shots).
// Three tiers by all the damage it has taken over its life, in shares of its most health (`taken`):
//   1 from its first hit, below 0.35: up to 6 marks, no wear over all;
//   2 from 0.35 (it has lost a third of its life at some time: a real fight; a T1 tank dies in three to five hits):
//     up to 11 marks, a little larger, some soot and chipped paint over all;
//   3 from 1.0 (it has lost more than its whole life over its life and lived: repaired or regenerated, a veteran):
//     up to 16 marks, larger again, more soot and chips; never so much that its shape, its faction's look or its
//     team colour stop reading (the marks' soot is weaker on team colour, the chips cover a few hundredths).
// Its burning (the game's own fire on it below half its health, Unit.lua's FxDamage2 and 3) adds soot over all.
// As it is repaired (or regenerates) its marks are mended: a patch of fresh paint over a dent with its seam, the
// heat tint faded, the soot mostly cleaned. They stay, as trophies: a whole unit keeps them.
//
// Snow on units. Without the option snow lies on a unit's upward faces by the weather alone (lighting.h). With it,
// each unit holds a share of that of its own: it builds up while the unit stands still in snowfall (as fast as on
// the ground), and comes off when it moves fast, fires, is hit, burns, or a blast is near; hot parts (what glows)
// stay clear; a unit fresh from the factory has none. A puff of falling snow where much comes off at once. Its
// patches lie by its place on the model at rest (lighting.h's surfaceSnowPlace), so that they go with it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "linalg.h"
#include "models.h"

namespace render {

// The shape a hit is cast onto (Model::hitShape): the triangles of one of the model's levels at rest, by bone, each
// bone's in a tree of boxes, so that a mark lies on the surface the shot met (the game's collision box, where a shot
// stops, is often larger than the model, or misses parts of it). The level: the finest of at most
// kHitShapeTriangles triangles, else the coarsest (a remastered unit's finest has hundreds of thousands). Made at
// loading while ModelCache::hitShapes is set (World sets it with unitScars): models loaded before have none, and
// their marks lie on the boxes round their parts (Model::boneBoxes).
inline constexpr int64_t kHitShapeTriangles = 8000;
struct HitShape {
  struct Node {
    Vec3 lo, hi;
    uint32_t start = 0, count = 0;  // count above 0: a leaf, its triangles from `start`; 0: its two below at start, start + 1
  };
  std::vector<Vec3> corners;   // three per triangle, model units at rest, by bone
  std::vector<Node> nodes;
  std::vector<int32_t> roots;  // by bone (the skeleton's numbers): its tree's top node; -1: no part of the model
  // How far along the ray from + way * t (model units at rest; `way` unit length) the first triangle of that bone's
  // part lies, up to `far`; below 0: none.
  float Cast(size_t bone, Vec3 from, Vec3 way, float far) const;
};
// One made from a mesh file's vertices (each on its first bone, below `bones`) and triangles (models.cpp's LoadScm).
std::shared_ptr<const HitShape> MakeHitShape(const char* vertices, int64_t vertexCount, const char* indices, int64_t indexCount, int indexBytes,
                                             size_t bones);

// At most this many marks on one unit.
inline constexpr int kWearMarks = 16;
// The puff where much snow falls off a unit at once: the game's own snow thrown up where a shot lands on snowy ground
// (clumps falling) and its soft cloud where an aircraft lands on it.
inline constexpr const char* kSnowPuffClumps = "/effects/emitters/tti_snowy01_medium01_01_emit.bp";
inline constexpr const char* kSnowPuffCloud = "/effects/emitters/tt_snowy01_landing01_01_emit.bp";

// What a unit wears in one frame, as the model renderer hands it to its shader (MeshRenderer::Wear).
struct WearLook {
  float general = 0;  // its wear over all: 0 none .. 3 the third tier's (and its burning's soot)
  float snow = -1;    // the share of the lying snow it holds, 0..1; -1: as the weather lays it (the option off)
  int count = 0;      // its marks
  // three rows for each mark: where it struck on the model at rest (world units) and its radius; the shot's way
  // there (unit length) and its kind (0 kinetic, 1 energy, 2 explosive, 3 fire); how fresh it is (1 fresh .. 0
  // mended), how hot (1 glowing .. 0), a number of its own (0..1) and how strong it is (0..1)
  Vec4 rows[kWearMarks * 3];
};

class UnitWear {
 public:
  enum Kind { Kinetic = 0, Energy = 1, Explosive = 2, Fire = 3 };
  // What the world has on (World sets them each frame).
  bool scars = false, snow = false;
  // Which kind of mark a shot of that blueprint makes ("/projectiles/TDFGauss01/TDFGauss01_proj.bp", or its id):
  // by the game's way of naming them (faction letter, then DF direct fire, IF indirect, AA anti-air, AN anti-navy,
  // BO bomb) and by what the name says (Gauss, Laser, Missile, ...).
  static Kind KindOf(const std::string& blueprint);
  // The same with the damage's type as the game names it ("Normal", "Fire", ...): fire damage burns.
  static Kind KindOf(const std::string& blueprint, const std::string& damageType);

  // THE HOOK FOR EXACT HITS: a hit as plain data. Today the effects stand's target feeds it (fx_entities.h); the
  // engine's per-hit report (sim/sim_view.h's HitEvent, taken each beat with Sim::TakeHits while Sim::collectHits is
  // set: the renderer's request 36) feeds it the same way, one for one, its units' hits only (targetKind 0, or 1 a
  // shield's, which names the shield's owner; props, projectiles and other entities left out):
  //   unit = target; shielded = shield; at = position; radius = radius; way = direction; blueprint = projectile;
  //   beam = beam; weapon = weapon; damageType = damageType; amount = amount; tick = the beat SimFrames::Beat took
  //   it in (as FrameEffectEvent::tick).
  // Told hits are applied at the Watch of their beat (or the next, the frame being drawn a beat or two behind the
  // simulation), where the unit's place, pose and health are known; for that unit they stand in for what Watch
  // would guess from the shots it sees stop (its health's drops are matched against them first). The engine's
  // health list (TakeHealth) is not needed: the frame's health says the same each beat.
  struct HitEvent {
    uint32_t tick = 0;       // the beat it happened in (0: applied at the next Watch, whatever its beat)
    uint32_t unit = 0;       // the entity hit (a shield's hit: its owner)
    Vec3 at, way;            // where (world) and along which way the shot came (world; need not be unit length; 0 0 0:
                             // from `at` toward the unit)
    float radius = 0;        // above 0: an area's damage, `at` its blast's centre (the mark on the side that faced it)
    std::string blueprint;   // the shot's blueprint ("TDFGauss01", or its path): what kind of mark (KindOf); "": none
    bool beam = false;       // a beam's hit (energy)
    std::string weapon;      // the weapon that fired ("uel0201-1-MainGun"; "": not known), where there is no blueprint
    std::string damageType;  // "Normal", "Fire", ... ("": by the blueprint alone)
    float amount = 0;        // hit points (after armour)
    bool shielded = false;   // a shield took it: no mark, only a shake of its snow
  };
  void Tell(const HitEvent& hit) { told_.push_back(hit); }
  // Set by a host that tells every hit (the engine's TakeHits wired in LiveSim): health drops then make no marks and
  // add nothing to the damage taken of their own (a drop no hit was told for is no damage: reclaim, a script's
  // SetHealth). Unset (a recording, the stand): marks are guessed from the shots seen to stop and the drops.
  bool exactHits = false;
  // The kind of mark a told hit makes: fire damage burns; a beam's is energy; else by the shot's blueprint, or
  // without one by the weapon's label (FAF's "<unit>-<n>-<Label>": Missile, Laser, ...); with neither (a pulse of
  // damage over time after the first, a death weapon, a script's own Damage) a general mark: an area's a blast's
  // scorch, else a plain dent.
  static Kind KindOf(const HitEvent& hit);

  // A game's beat (World::AddFrame, or a host that draws units itself): the shots that stopped since the last beat
  // and the beams' ends, each unit's health, place and burning. Only the first call for a beat counts; units the
  // frame does not list are kept. `model`: a unit's model by its FrameUnit::blueprint (null: none); with `make` false
  // only one already made (Watch makes none for units only seen, that the picture may never draw: in a game of M28s
  // that loaded every kind of unit on the map at once, a second; it asks for one to be made where a unit is hit).
  // `clock`: the game's seconds; `snowing`: how hard it snows now (0..1); `snowLies`: how much snow lies
  // (World::snowLying).
  void Watch(const Frame& frame, double clock, float snowing, float snowLies, const std::function<const Model*(uint32_t, bool make)>& model);
  // A hit told straight (a host's own units: the viewer's --wear-test): on the unit known as `id`, whose model is
  // `model`, at `rest` on it at rest (world units, its own frame), along `way` (the same frame), taking `share` of
  // its most health with `damage` hit points (what sizes the mark).
  void Hit(uint32_t id, const Model* model, Vec3 rest, Vec3 way, Kind kind, float share, float damage, double clock);
  // Health given back to a unit, a share of its most (repair, regeneration): its marks look mended the more.
  void Heal(uint32_t id, float share);
  // What it wears now; null when it wears nothing of its own (the options off, or it is not known).
  const WearLook* Look(uint32_t id, double clock);
  // The tier it is in (0: never hurt), for the log and tests.
  int Tier(uint32_t id) const;
  // Where snow fell off units at once since the last call: the place (xyz) and how large a puff (w, world units).
  void TakePuffs(std::vector<Vec4>* out) {
    out->swap(puffs_);
    puffs_.clear();
  }
  // Counts since the start, for the log: shots seen to stop on a unit, beams' ends on one, blasts that marked one,
  // drops of health no hit was found for, marks made (merged ones not counted), heals.
  int toldHits = 0, shotHits = 0, beamHits = 0, blastHits = 0, unseenHits = 0, marksMade = 0, heals = 0;
  int known() const { return static_cast<int>(units_.size()); }

 private:
  struct Mark {
    Vec3 at, way;  // on the model at rest, world units; the shot's way there
    float radius = 0.3f;
    int kind = Kinetic;
    float fresh = 1;     // 1 fresh .. 0 mended
    float strength = 1;  // 0..1
    float seed = 0;
    double born = 0;     // (energy: when it was last hit, for its glow)
  };
  struct Unit {
    uint32_t blueprint = 0xffffffffu;  // FrameUnit::blueprint (another unit under the same id is another one)
    const Model* model = nullptr;
    float health = -1, most = 0;
    float taken = 0;   // all the damage taken over its life, in shares of its most health
    float burn = 0;    // soot of its burning, 0..1
    float snow = 1;    // the share of the lying snow it holds
    Vec3 place;
    bool placed = false;
    float speed = 0;   // world units a second, smoothed
    bool built = true; // finished (a unit under construction holds no snow)
    double seen = 0, puffAt = -10;
    uint32_t hits = 0; // marks asked for (a number for each mark's own)
    float toldAhead = 0;  // hit points told (Tell) that its health has not yet been seen to lose
    std::vector<Mark> marks;
    WearLook look;
  };
  static int TierOf(float taken) { return taken <= 0 ? 0 : taken < 0.35f ? 1 : taken < 1.0f ? 2 : 3; }
  void AddMark(Unit& u, Vec3 rest, Vec3 way, Kind kind, float share, float damage, double clock);
  std::unordered_map<uint32_t, Unit> units_;
  // shots by id: the beat each was last seen (top bit: its stop was counted), to tell new shots (who fired) and
  // stops counted once
  std::unordered_map<uint32_t, uint32_t> shots_;
  std::vector<int8_t> kinds_;  // by Frame::blueprints index: KindOf (-1 not yet)
  uint32_t lastTick_ = 0;
  bool haveTick_ = false;
  std::vector<Vec4> puffs_;
  std::vector<HitEvent> told_;  // (Tell's, until the Watch of their beat)
};

// The model shader's part (mesh_renderer.cpp, modern look): a unit's marks and wear over all on its surface,
// before it is lit. `rows`: the buffer the marks lie in (from wear.x on, wear.y of them; wear.z its wear over all);
// `p`, `n`: the place on the model at rest and its surface's way there (world units); `world`: the place in the world;
// `teamMask`: how much of the surface is team colour; `glowDim`: what glowing is multiplied by under the hour's light.
// Changes the surface's colour (linear), metal, roughness, glow and its way N (the dents).
extern const char kWearShaderBlock[];  // (its text in unit_wear.cpp)

}  // namespace render
