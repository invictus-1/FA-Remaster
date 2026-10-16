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
  bool InitUnits(std::function<float(float, float)> terrain, bool water, float waterLevel,
                 std::function<const Model*(const std::string&)> unitModel, std::string* error);
  // A unit of that blueprint id ("uel0201") made at `at` as the game makes one (its script's class:
  // OnPreCreate, OnCreate, OnStopBeingBuilt), facing `forward`: on the ground, on the water (a ship),
  // or flying level at its blueprint's elevation and speed (an aircraft: if it dies `fallAfter`
  // seconds from now, it comes down at `at`). 0 when it cannot be made (the reason in `error`).
  uint32_t AddUnit(const std::string& id, Vec3 at, Vec3 forward, bool original, int side, float fallAfter, std::string* error);
  // Kills it as the game does: its script's OnKilled(nil, "Normal", overkill).
  void Kill(uint32_t unit, float overkill);

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
    std::string unit;  // blueprint id
    bool wreck = false, original = false;
    Vec3 position;
    Quat turn;
    float scale = 1;      // of the drawing: the unit's model's, or what the script sized the wreck to
    std::vector<FrameBone> bones;
  };
  const std::vector<Body>& Bodies() const;
  // The effect events made since the last call (their ticks are on the same clock: tick = seconds x 10).
  void TakeEvents(std::vector<FrameEffectEvent>* out);
  int Count() const;
  // Every effect made from now on plays this faction's effects in the remastered look ("": as what it
  // is made on is).
  void SetFaction(const std::string& faction);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
