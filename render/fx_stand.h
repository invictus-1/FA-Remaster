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
//
// A unit's death (`death`): the unit made at the spot by its own script class, killed `killAt`
// seconds into each play by its own death path (OnKilled, DeathThread, the explosions, debris and
// wreck of /lua/defaultexplosions.lua and /lua/wreckage.lua), run on the stand's stand-ins of the
// engine (fx_entities.h); the stand draws the unit, posed as its scripts pose it, until it is gone,
// then its wreck.
//
// A unit on the move (`move`): made as for a death, then driven round a loop at its blueprint's speed
// (a long narrow one, its long sides away from the camera: on the two sides of a side by side the two
// loops lie side by side over the same ground), setting off a second and a half into the play and
// stopping where it began; an aircraft hovers at its elevation first. Its movement effects are the
// ones its scripts make as the engine's motion events reach them (fx_entities.h). A build (`build`
// builds `buildUnit`): a factory builds the unit at the spot (on its build bone), an engineer builds
// it at the spot from beside it, its build effects as the game's scripts make them; the unit drawn as
// far built as it is.
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
  // a unit (its id, "UEL0201") made at `at` and killed `killAt` seconds into each play, as the game
  // kills it (OnKilled with that overkill ratio: above 1 no wreck, above 2 its chassis thrown too)
  std::string death;
  float killAt = 1.0f, overkill = 0.2f;
  // whose faction's effects play in the remastered look ("uef", "aeon", "cybran", "seraphim"); "": as
  // what they are made on is (a shot: its own, by its id; an effect at a spot: none)
  std::string faction;
  // a unit (its id) driven round a loop at `at`, `laps` times each play
  std::string move;
  int laps = 1;
  // a builder (its id) building a unit (its id) at `at`; `buildSeconds`: how long the build takes
  // (0: the game's own time)
  std::string build, buildUnit;
  float buildSeconds = 0;
  // whether --fx-repeat and --fx-spacing were given (else a move's and a build's are worked out)
  bool repeatGiven = false, spacingGiven = false;
};

class FxStand {
 public:
  // Reads what it is to play. False with the reason when there is nothing.
  bool Init(moho::Vfs* vfs, const FxStandSetup& setup, std::string* error);
  // The map under a unit's death, move or build: the ground's height (under water the bottom's), the
  // water, and the map's terrain type code at a place.
  void SetGround(std::function<float(float, float)> terrain, bool water, float waterLevel,
                 std::function<int(float, float)> terrainType = nullptr);
  // Each frame after World::Begin: starts, moves and ends what it plays (effect entities: handed to
  // the world as a game's projectiles and effects, World::AddFrame and Effects). `right`: the camera's right
  // (the two sides are laid out along it); `ground`: the ground's height at a place.
  void Update(World& world, double now, Vec3 right, const std::function<float(float, float)>& ground);
  // What it read, for the log.
  std::string Describe() const;
  // How far across what it plays reaches, both sides together, for the camera to frame it (0: it says
  // nothing): --fx-build's builder and unit; --fx-move's units, which the camera follows (Focus).
  float Extent() const { return extent_; }
  // Where the driven units are (--fx-move: between the two sides'), for the camera to follow; false
  // when there are none.
  bool Focus(Vec3* at) const;

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
    double lastHit = -10;  // --fx-shield, a personal shield: its last hit, and where on the unit at rest
    Vec3 hitPoint;
    uint32_t unit = 0;   // --fx-death: the unit, and whether it is dead yet; --fx-move, --fx-build: the unit
    bool killed = false;
    uint32_t built = 0;  // --fx-build: the unit being built
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
  bool personalShield_ = false;  // the unit's own (Defense.Shield.PersonalShield): the unit drawn with its shield up
  std::string shotFaction_;  // whose the shot's effects are
  // --fx-move: the loop (half its straight's length, its turns' radius); --fx-build: the order
  float loopHalf_ = 8, loopRadius_ = 3;
  std::string buildOrder_;
  float extent_ = 0;
  // --fx-death, --fx-move, --fx-build: the map under it, and whether its world is set up
  std::function<float(float, float)> terrain_;
  std::function<int(float, float)> terrainType_;
  bool water_ = false, unitsReady_ = false, unitsFailed_ = false;
  float waterLevel_ = 0;
  std::unique_ptr<FxEntities> entities_;
  std::vector<FrameEffectEvent> events_;
  std::vector<Side> sides_;
  double started_ = -1e9;
};

}  // namespace render
