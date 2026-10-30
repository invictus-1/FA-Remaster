// The effects stand's small simulation of the game's effect entities: what a nuke's impact makes
// (effects/Entities/UEFNukeEffectController01, and the fireball, the smoke rings, the plumes it makes
// in turn), for making effect art without a game. The entities' own scripts run in a script state
// of the stand's own, with stand-ins for the simulation's calls they make: they move
// (SetVelocity, SetAcceleration, SetBallisticAcceleration, SetVelocityAlign, SetOrientation), grow
// (SetScaleVelocity) and make more of their kind (CreateProjectile); their threads (ForkThread,
// WaitTicks, WaitSeconds) run ten ticks a second; Random runs from a seed, so a play repeats. What they
// make (CreateEmitterAtEntity / OnEntity, CreateAttachedEmitter, CreateLightParticle, CreateDecal,
// CreateSplat) becomes the effect events a game sends, and they themselves the game's projectiles
// (with a mesh) or script entities (without): the world draws both as it does in a game.
//
// Each side of the stand's side by side plays from a Random of its own, started from the same seed:
// both sides draw the same numbers.
//
// A unit's death (InitUnits, AddUnit, Kill): the unit's own script class runs, with everything it is
// built on (the game's /lua/sim/Unit.lua and its subclasses, /lua/defaultexplosions.lua, wreckage),
// on stand-ins of the engine's objects: a unit that has a place, a turn, its mesh's bones (which can
// be hidden, carried off by debris and posed by its .sca animations), a layer and an army; the game's
// blueprint loading for its blueprint; props for its wreck. What the engine itself does is stood in
// for here: a dead aircraft falls (its speed, the simulation's gravity, a tumble) and calls the
// unit's OnImpact where it meets the ground or the water; what the scripts make falls as its blueprint
// says (Physics.UseGravity) and calls its OnImpact where it meets the ground or the water.
//
// A unit on the move (Drive): driven round a loop at its blueprint's speed, the engine's motion stood
// in for - its scripts told what the engine tells them (OnMotionHorzEventChange as it sets off, reaches
// its top speed, slows and stops; OnLayerChange where a hover unit goes from land onto water;
// OnTerrainTypeChange where the map's terrain type under it changes, GetTerrainType answering from the
// map's own terrain types and the game's /lua/TerrainTypes.lua), so that they make their movement
// effects as in a game. A build (Build): a unit made as begun by a builder, its fraction complete
// growing as the engine's build tasks make it grow, the builder's and the unit's build callbacks
// called as the engine calls them (OnStartBuild, OnBuildProgress, OnStopBeingBuilt, OnStopBuild).
//
// A unit firing (InitWeapons, Order): its weapons made and run as the engine runs them (fx_weapons.cpp),
// the manipulators its scripts make posing its bones, its shots flying as the engine's projectiles fly and
// its collision beams reaching to the ground; a unit of a mod (under /mods) is found as the game's own.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace moho {
class Vfs;
}

namespace render {

struct Model;

class FxEntities {
 public:
  FxEntities();
  ~FxEntities();
  // Sets up the script state. `seed`: where Random starts (the same seed plays the same way).
  bool Init(moho::Vfs* vfs, uint32_t seed, std::string* error);
  // Makes an effect entity of that blueprint at `at`, at the time of the last Advance, as a nuke's
  // impact makes its controller: made, then its EffectThread started unless its OnCreate started a
  // thread itself. `original`: what it and what it makes show in the original look (the stand's
  // side by side). `side`: which side of it (its own Random).
  bool Spawn(const std::string& blueprint, Vec3 at, bool original, std::string* error, int side = 0);
  // What the stand makes itself, as the game's scripts would (a shield and its hits): an entity drawn
  // with a mesh blueprint ("/effects/entities/shield01/shield01_mesh"), placed, turned and sized, gone
  // after `seconds` (0: at Clear); `shield`: a shield's bubble. Then emitters made on it ("CreateEmitterAtBone",
  // "CreateEmitterOnEntity", ...) and calls on them ("OffsetEmitter", ...). At the time of the last Advance.
  // `faction`: whose faction's effects the emitter plays in the remastered look ("": as the entity is).
  uint32_t AddEntity(const std::string& mesh, Vec3 at, const Quat& turn, Vec3 scale, bool shield, bool original, float seconds);
  uint32_t AddEmitter(const std::string& how, uint32_t entity, const std::string& blueprint, const std::string& faction = std::string());
  void CallEmitter(uint32_t effect, const std::string& name, const std::vector<float>& numbers);

  // The dying units' world (the stand's --fx-death): the game's unit classes and what they stand on.
  // `terrain`: the ground's height (under water the bottom's); `water`: the map has water at
  // `waterLevel`; `unitModel`: a unit's model by its blueprint id (its bones and scale).
  // `terrainType`: the map's terrain type code at a place (-1: none).
  bool InitUnits(std::function<float(float, float)> terrain, bool water, float waterLevel,
                 std::function<const Model*(const std::string&)> unitModel, std::string* error,
                 std::function<int(float, float)> terrainType = nullptr, float mapWidth = 0, float mapHeight = 0);
  // How a unit is made: facing `forward`, in the look and with the Random of its side; an aircraft
  // either flying in level at its speed so that if it dies `fallAfter` seconds from now it comes down
  // at the spot (`flyIn`), or hovering at its elevation over it; begun by `builder` (its fraction
  // complete from 0: OnStartBeingBuilt) or finished (OnStopBeingBuilt).
  struct UnitPlace {
    Vec3 forward{0, 0, 1};
    bool original = false;
    int side = 0;
    bool flyIn = false;
    float fallAfter = 0;
    uint32_t builder = 0;
  };
  // The weapons' world (the stand's --fx-fire), after InitUnits and before the first unit: from then on a unit
  // is made with its weapons as the engine makes them (one weapon object per blueprint weapon that is not a
  // dummy, of the class its script's Weapons table names, OnCreate called), and the stand stands in for the
  // engine's side of them (fx_weapons.cpp): the manipulators its scripts make pose its bones (aim controllers
  // turning its turrets toward their targets, rotators, sliders, slavers), its weapons fire as the engine's fire
  // clock lets them (OnFire), their shots fly as the engine's projectiles do, and its collision beams reach from
  // their muzzles to where they meet the ground. The units of the other modes stay as they were.
  // (`mapWidth`, `mapHeight`: the map's size, 0 when there is no map: the scripts ask it)
  bool InitWeapons(std::string* error, float mapWidth = 0, float mapHeight = 0);
  // Orders for a unit's weapons, against a point on the ground (`target`, in the world): the weapons named by
  // `labels` (their blueprints' Label; none: every weapon that can shoot at the ground and is no death weapon)
  // get it as their target, as the engine's targeting hands them one (OnGotTarget, then they fire by
  // themselves); the manual-fire weapons named by `manual` are fired at it as a player's order fires them (a
  // Tactical or Nuke order: the engine's fire-at task; an OverCharge order: the attack task in overcharge mode).
  // `stop`: the weapons lose their targets (OnLostTarget), the manual-fire ones too (as when the unit is given
  // another order: one that unpacked to fire packs up again then). Returns what it did, for the log.
  std::string Order(uint32_t unit, Vec3 target, const std::vector<std::string>& labels, const std::vector<std::string>& manual,
                    bool stop = false);
  // A unit's blueprint file: the game's own in /units, a mod's in its folder under /mods ("": none).
  static std::string UnitBlueprintFile(const moho::Vfs* vfs, const std::string& id);
  // A unit of that blueprint id ("uel0201") made at `at` as the game makes one (its script's class:
  // OnPreCreate, OnLayerChange, OnCreate, then OnStopBeingBuilt or OnStartBeingBuilt): on the ground,
  // on the water (a ship, a hover unit there), or in the air. 0 when it cannot be made (the reason in
  // `error`).
  uint32_t AddUnit(const std::string& id, Vec3 at, const UnitPlace& how, std::string* error);
  // The builder builds the unit (made begun by it): its OnStartBuild(unit, order) now ("MobileBuild",
  // "FactoryBuild"), then the build as the engine carries it out, in `seconds` (0: the game's own time,
  // BuildTime / BuildRate).
  void Build(uint32_t builder, uint32_t unit, const std::string& order, float seconds);
  // Drives a unit round `loop` (points on the ground, closed) `laps` times, setting off at `start`
  // (seconds, on Advance's clock) and stopping at the loop's start.
  void Drive(uint32_t unit, const std::vector<Vec3>& loop, double start, int laps);
  // Kills it as the game does: its script's OnKilled(nil, "Normal", overkill). From then on its unit in the frame is
  // dead (FrameUnit::dead), as a game's is while its death plays.
  void Kill(uint32_t unit, float overkill);
  // A unit the weapons' shots and beams hit (the stand's --fx-target, for battle scars): where they cross its model's box
  // they meet it ('Unit': its impact effects for a unit), taking the hit points their damage data say from its health
  // (`health`; 0: its blueprint's Defense.MaxHealth). An engineer stands by: below three tenths of its health it is
  // repaired to whole in three seconds, so it lives on and its damage over its life grows past its health. The frame
  // gives its health. Heal: repaired to whole now.
  void SetTarget(uint32_t unit, float health);
  void Heal(uint32_t unit);
  // The hits on targets since the last call, as the world's battle scars take them (unit_wear.h, UnitWear::Tell).
  struct TargetHit {
    uint32_t unit = 0;
    Vec3 at, way;           // world
    std::string blueprint;  // the shot's (a beam's: its weapon's projectile is none: "beam")
    std::string damageType;
    float amount = 0;
  };
  void TakeTargetHits(std::vector<TargetHit>* out);

  // Ends all that plays: the entities go, their threads stop. What they left (decals, light) runs out.
  void Clear();
  // Moves time on to `now` (seconds): the threads run tick by tick (tick n at n / 10 seconds), the
  // entities move in between.
  void Advance(double now);
  // The entities now: with a mesh as projectiles, without as script entities, the units as units
  // (with their bones, for what hangs on them; the stand draws them itself, Bodies).
  const Frame& Current() const;
  // The units and their wrecks as the stand draws them: the unit's model (its wreck's, for a wreck),
  // placed, turned and sized, its bones as the scripts posed them (as a frame's bones are, in world
  // units of its model: a wreck's model has the scale 1, its size is `scale`), in the look of its side.
  struct Body {
    uint32_t id = 0;   // its entity's (the same while it lives: what the world knows its ripples, battle scars and snow by)
    std::string unit;  // blueprint id
    bool wreck = false, original = false;
    Vec3 position;
    Quat turn;
    float scale = 1;      // of the drawing: the unit's model's, or what the script sized the wreck to
    float built = 1;      // its fraction complete
    std::vector<FrameBone> bones;
  };
  const std::vector<Body>& Bodies() const;
  // The effect events made since the last call (their ticks are on the same clock: tick = seconds x 10).
  void TakeEvents(std::vector<FrameEffectEvent>* out);
  // What met the water since the last call (a shot, what a death threw out, an aircraft coming down), as the engine
  // tells its script OnImpact 'Water': where (x, y, z) and about how large it is (w: world units across its middle).
  void TakeWaterHits(std::vector<Vec4>* out);
  int Count() const;
  // Every effect made from now on plays this faction's effects in the remastered look ("": as what it
  // is made on is).
  void SetFaction(const std::string& faction);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
