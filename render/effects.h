// The game's effects in a running game: what the simulation's scripts start, change and end
// (emitters, beams, trails) is turned into particle emitters and bands here, and every effect that
// hangs on something is kept where that thing is in the frame being shown.
//
// The simulation hands over events (frame.h, FrameEffectEvent) beat by beat; they take effect when
// the picture's clock reaches their beat. An effect's place is its entity's place in the frame: a
// unit (with the bone's pose), a projectile, or a weapon's beam (bone 0 its start, 1 and 2 its end).
//
// In the remastered look an emitter, beam or trail plays its faction's own blueprint where the visuals
// track has made one (effect_files.h). Its faction is that of the thing it is made on or at: a unit's
// by its blueprint id ([wakes] a mod's unit whose id says nothing: as its blueprint's General.FactionName or
// Categories name it, FactionOfUnit), a projectile's or an effect entity's by its blueprint's name, a script entity's
// by its mesh; where that says nothing (the debris a dying unit throws, the shared helpers), the
// faction of the army the script made it for - the faction most of that army's units in the frame are.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "decals.h"
#include "tracks.h"
#include "frame.h"
#include "lights.h"
#include "linalg.h"
#include "models.h"
#include "particles.h"
#include "ribbons.h"

namespace render {

class Blasts;
class BuildEffects;
class Exhausts;

// Whether a blueprint's path is a warp-in's or a teleport's own effect (/effects/entities/UnitTeleport01..03: a commander's
// warp-in, CommandUnit.lua, and a teleport): its light and its harm to the spot ('Fire', 'Disintegrate') are no explosion.
bool IsTeleportEffect(const std::string& path);

class Effects {
 public:
  Effects(Particles* particles, Ribbons* ribbons, Decals* decals, Lights* lights, ModelCache* models)
      : particles_(particles), ribbons_(ribbons), decals_(decals), lights_(lights), models_(models) {}

  // Events in the order the simulation made them.
  void Queue(std::vector<FrameEffectEvent>&& events);
  // Lets every event up to `tick` take effect, moves what hangs on something to where that is in
  // `frame`, and lists this frame's beams. Call before Particles::Update and Ribbons::Update, whose
  // clock must be tick / 10 seconds.
  void Update(double tick, const Frame& frame);
  // Where emitters that stay where they were made were moved out to since the last call (OffsetEmitter:
  // a shield's hit effects are made at its centre and moved out to where it was struck).
  void TakeHits(std::vector<Vec3>* out) {
    out->swap(hits_);
    hits_.clear();
  }
  // Does one of the game's own effects hang on that entity (so that no stand-in is needed for it)?
  bool OnEntity(uint32_t entity) const { return hanging_.count(entity) != 0; }

  int live = 0;        // effects being kept
  // The tracks in lying snow (tracks.h): the prints the game lays under units are pressed into it too.
  void SetTracks(Tracks* tracks) { tracks_ = tracks; }
  // The blasts' list (blasts.h): told of every flash of light and every scorch or crater a script makes.
  void SetBlasts(Blasts* blasts) { blasts_ = blasts; }
  // [scars] Whether an entity is a prop (a tree, a rock, a wreck: the world's scenery, standing or about to go): a flash
  // made on one is no explosion (Explosion). None: not known.
  std::function<bool(uint32_t)> isProp;
  // [whitecaps] Where a unit is drawn, when the picture moves it from the game's place (a ship riding the lifted waves:
  // ocean.h, World::RideWaves): given the unit (its blueprint's name, the frame's unit, its model) and the game's place and
  // turn, those it is drawn at (true), so that what hangs on its bones (its bow spray, its wake's emitters) follows the hull
  // drawn. None: the game's place.
  std::function<bool(const std::string&, const FrameUnit&, const Model*, Vec3*, Quat*)> drawnAt;
  bool drawnMoves = false;  // [whitecaps] (whether drawnAt is to be asked: the waves lift the sea; set each frame)
  // [snowkick] How much of the weather's snow lies (World::snowLying in the remastered look with the weather on, else 0; set
  // each frame): the ground's kick-up and footfall effects the game picks by the map's terrain type (dirt, sand, grass, rock)
  // are its snow's (the Snowy02 terrain type's) for that share of them, as the snow lies over that ground.
  float snowLying = 0;
  // (buildfx) The remastered build look (build_effects.h): told of every build beam drawn and of the frame's units under
  // construction, while it is on.
  void SetBuild(BuildEffects* build) { build_ = build; }
  // (unitsfx) The real exhausts (exhausts.h): told of every engine's beam on one thing while they are on, which they draw
  // as a plume in place of the band.
  void SetExhausts(Exhausts* exhausts) { exhausts_ = exhausts; }
  int shotLights = 0;  // lights given this frame to shots in flight and to weapons' beams
  float shotLight = 1.0f;  // how strong those are (0: none)
  int started = 0;     // since the beginning
  int curvesSet = 0;   // SetEmitterCurveParam calls acted on
  int notShown = 0;    // events for kinds of effect that are not drawn yet (decals of kinds other than albedo and normals, anything new)
  float cpuMs = 0;     // Update on the processor, averaged over the last frames
  bool verbose = false;  // says each kind of light flash and decal once, as it first appears

 private:
  enum class Kind { Emitter, Trail, Beam, TextureBeam };
  struct Effect {
    Kind kind = Kind::Emitter;
    uint32_t handle = 0;      // the emitter's or trail's
    std::string blueprint;    // beams: the blueprint, or the texture
    uint32_t entity = 0xffffffff, entity2 = 0xffffffff;
    int bone = -1, bone2 = -1;
    bool attached = false;    // follows its entity
    Vec3 offset;              // from the attach point, in its frame (OffsetEmitter)
    Vec3 position;            // where it was last
    Quat orientation;
    float length = 0, halfWidth = 0.05f;  // texture beams
    const Model* model = nullptr;         // of the unit it hangs on, once looked up (for the bone's place at rest)
    // a beam between two things: were its own thing and its far end ever found in a frame? (A
    // script hangs beams on things the frame does not list: the hubs of an adjacency's link have
    // no mesh. Its own end then stays where it was made; with a far end never found it is not drawn.)
    bool everHere = false, everEnd = false;
    bool hasEnd = false;  // the event said where the far end was
    Vec3 end;
    // the colour it lights a shot in flight with, looked up the first time it is needed (its
    // blueprint does not change): -1 not yet, 0 it does not glow, 1 it glows `glowColor`
    int8_t glows = -1;
    bool glowTracers = false;  // (a trail's: whether hot tracers were on when it was looked up)
    Vec3 glowColor;
    std::string faction;       // beams and trails: whose blueprint they play (FactionOf)
    bool buildBeam = false;    // (buildfx) a build beam (BuildEffects::IsBuildBeam)
    bool original = false;     // (buildfx) made in the original effects' look whatever the world's (the stand's right side)
    float beamLength = -1e30f, beamThickness = -1e30f;  // (unitsfx) what SetBeamParam set (below -1e29: not set; exhausts.h)
  };
  void Apply(const FrameEffectEvent& event, const Frame& frame);
  void End(uint32_t id);
  // Where an entity's bone is in the frame. False when the entity is not in it.
  // `model`: where the unit's model is remembered between calls (may be null).
  bool AttachPoint(const Frame& frame, uint32_t entity, int bone, Vec3* position, Quat* orientation, const Model** model = nullptr);
  // Is that entity one the army the picture is for does not see (a unit that is not seen now, a
  // shot where it does not see)? What hangs on such a one is not shown.
  // (by this frame's lists of shots and script entities: Update makes them first)
  bool Hidden(const Frame& frame, uint32_t entity) const;
  // [scars] Is a flash of light made on that entity an explosion's (a blast, blasts.h)? A shot's (on its projectile, or
  // on one already gone), a dying unit's, an effect entity's (a nuke's, a death's) are; one a script makes on a living
  // unit (a structure begun, a unit being built, a teleport, a tractor claw's grip, a shield's discharge), on a warp-in's or
  // a teleport's own effect (UnitTeleport01..03) or on a prop (one a new structure clears away, one reclaimed, a tree
  // catching fire) is not: it burns nothing, pushes nothing and makes no volume explosion.
  bool Explosion(const FrameEffectEvent& event, const Frame& frame) const;
  // (volume explosions) Half the width of the unit a flash is made on, when that unit is dying (its model's, the larger of
  // across and along, in world units); 0 otherwise.
  float DyingWidth(const FrameEffectEvent& event, const Frame& frame);
  // Whose effects an event makes in the remastered look ("uef", ...; "" none known). See the top.
  std::string FactionOf(const FrameEffectEvent& event, const Frame& frame);
  // [wakes] Whose a unit is by its blueprint id: the id's (effect_files.h UnitFaction), else, for a mod's unit with an id
  // of its own, as its blueprint names it (ModelCache::Faction: General.FactionName or Categories). "" none known.
  std::string FactionOfUnit(const std::string& id);
  // The faction most of an army's units in the frame are (army from 0), worked out once a beat.
  const std::string& ArmyFaction(const Frame& frame, int army);

  Particles* particles_;
  Ribbons* ribbons_;
  Decals* decals_;
  Tracks* tracks_ = nullptr;
  Blasts* blasts_ = nullptr;
  BuildEffects* build_ = nullptr;  // (buildfx)
  Exhausts* exhausts_ = nullptr;   // (unitsfx)
  Lights* lights_;
  ModelCache* models_;
  std::deque<FrameEffectEvent> pending_;
  std::unordered_map<uint32_t, Effect> effects_;
  // the decals a script laid for good, by the effect's number: taken away when it destroys them
  std::unordered_map<uint32_t, uint32_t> decalOf_;
  std::unordered_map<std::string, int> said_;
  std::unordered_map<uint32_t, int> hanging_;  // entity -> how many effects hang on it
  // this frame: what the glowing things on each shot come to (the shot's place in the frame's list ->
  // the strongest colour among them, the largest scale, how many)
  struct ShotGlow {
    Vec3 color;
    float scale = 1;
    int count = 0;
  };
  std::unordered_map<uint32_t, ShotGlow> shotGlow_;
  std::unordered_set<uint64_t> shotCells_;  // squares of ground that have a shot's light this frame
  std::unordered_map<uint32_t, uint32_t> projectileAt_, beamAt_, entityAt_;  // this frame: entity id -> index in the frame's lists
  std::vector<std::string> armyFactions_;  // ArmyFaction's, for the beat armyFactionsTick_
  std::vector<Vec3> hits_;                 // TakeHits
  uint32_t armyFactionsTick_ = 0xffffffffu;
};

}  // namespace render
