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
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace moho {
class Vfs;
}

namespace render {

class FxEntities {
 public:
  FxEntities();
  ~FxEntities();
  // Sets up the script state. `seed`: where Random starts (the same seed plays the same way).
  bool Init(moho::Vfs* vfs, uint32_t seed, std::string* error);
  // Makes an effect entity of that blueprint at `at`, at the time of the last Advance, as a nuke's
  // impact makes its controller: made, then its EffectThread started unless its OnCreate started a
  // thread itself. `original`: what it and what it makes show in the original look (the stand's
  // side by side).
  bool Spawn(const std::string& blueprint, Vec3 at, bool original, std::string* error);
  // What the stand makes itself, as the game's scripts would (a shield and its hits): an entity drawn
  // with a mesh blueprint ("/effects/entities/shield01/shield01_mesh"), placed, turned and sized, gone
  // after `seconds` (0: at Clear); `shield`: a shield's bubble. Then emitters made on it ("CreateEmitterAtBone",
  // "CreateEmitterOnEntity", ...) and calls on them ("OffsetEmitter", ...). At the time of the last Advance.
  uint32_t AddEntity(const std::string& mesh, Vec3 at, const Quat& turn, Vec3 scale, bool shield, bool original, float seconds);
  uint32_t AddEmitter(const std::string& how, uint32_t entity, const std::string& blueprint);
  void CallEmitter(uint32_t effect, const std::string& name, const std::vector<float>& numbers);
  // Ends all that plays: the entities go, their threads stop. What they left (decals, light) runs out.
  void Clear();
  // Moves time on to `now` (seconds): the threads run tick by tick (tick n at n / 10 seconds), the
  // entities move in between.
  void Advance(double now);
  // The entities now: with a mesh as projectiles, without as script entities.
  const Frame& Current() const;
  // The effect events made since the last call (their ticks are on the same clock: tick = seconds x 10).
  void TakeEvents(std::vector<FrameEffectEvent>* out);
  int Count() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace render
