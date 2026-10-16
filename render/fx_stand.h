// The viewer's effects test stand, for making effect art: one effect of the game played at a spot over
// and over, or one shot fired from a spot to another with its trail and its hit, and with `compare`
// the remastered look and the original's side by side (remastered on the left of the picture,
// original on the right, the same effect started at the same moment on both).
//
// What a name means is read from the game's own scripts, run in a script state of the stand's own:
// an effect template from /lua/EffectTemplates.lua (its list of emitter blueprints), a projectile's
// trails and hits from its script class (PolyTrail(s), FxTrails, FxImpactLand / Unit / Water / ...).
// The simulation's functions are not there: what the scripts would get from it reads as an empty
// stand-in, which is enough to read their tables.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fx_entities.h"
#include "linalg.h"

namespace moho {
class Vfs;
}

namespace render {

class World;

struct FxStandSetup {
  std::string fx;            // an EffectTemplates name ("TGaussCannonFlash"), or emitters with commas between
  std::string shot;          // a projectile: its id ("TDFGauss01") or its blueprint's path
  std::string hit = "land";  // which of its hits plays where it lands: land, unit, water, underwater, prop, shield, air, none
  Vec3 at, from, to;         // where the effect plays; where the shot starts and ends (heights above the ground)
  float repeat = 2;          // seconds from one start to the next
  bool compare = false;      // remastered and original side by side
  float spacing = 8;         // how far apart the two are then
  // an effect entity (its id, "UEFNukeEffectController01", or its blueprint's path), made at `at` as a
  // nuke's impact makes its controller, its own scripts run (fx_entities.h)
  std::string entity;
  uint32_t seed = 1;         // where the entities' Random starts
  // a unit (its id, "UEB4301") with its shield up at `at` (its blueprint's Defense.Shield), hit from
  // random directions above the ground every `hitEvery` seconds with its own hit effects (ImpactMesh,
  // ImpactEffects), as the game's shield script makes them
  std::string shield;
  float hitEvery = 0.8f;
};

class FxStand {
 public:
  // Reads what it is to play. False with the reason when there is nothing.
  bool Init(moho::Vfs* vfs, const FxStandSetup& setup, std::string* error);
  // Each frame after World::Begin: starts, moves and ends what it plays (effect entities: handed to
  // the world as a game's projectiles and effects, World::AddFrame and Effects). `right`: the camera's right
  // (the two sides are laid out along it); `ground`: the ground's height at a place.
  void Update(World& world, double now, Vec3 right, const std::function<float(float, float)>& ground);
  // What it read, for the log.
  std::string Describe() const;

 private:
  struct Side {
    bool remastered = true;
    Vec3 shift;  // from the setup's places
    std::vector<uint32_t> emitters;  // the effect's, or the shot's hit
    std::vector<uint32_t> carried;   // emitters riding on the shot
    std::vector<uint32_t> trails;
    bool flying = false;
    double nextHit = 0;  // --fx-shield
    uint32_t rng = 1;
  };
  void Fire(World& world, Side& side, double now, Vec3 from);
  FxStandSetup setup_;
  std::vector<std::string> fxEmitters_;
  // the shot's
  std::string projectile_;  // its blueprint's path
  float speed_ = 30;
  std::vector<std::string> polyTrails_, fxTrails_, impact_;
  std::string entity_;            // the effect entity's blueprint: --fx-entity, or what the shot's impact makes
  // --fx-shield: the unit's, and what its blueprint says of its shield
  std::string shieldUnit_, shieldMesh_, impactMesh_;
  std::vector<std::string> impactEffects_;
  float shieldSize_ = 0, shieldOffset_ = 0;
  std::unique_ptr<FxEntities> entities_;
  std::vector<FrameEffectEvent> events_;
  std::vector<Side> sides_;
  double started_ = -1e9;
};

}  // namespace render
