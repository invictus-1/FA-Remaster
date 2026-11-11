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
#include <map>
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
  // [heldworld] The same while the world is not shown (WorldPicture::SetWorldShown off: the interface's jump back in a
  // replay holds its last picture while the game played again runs ahead behind it, and the game's ren_Oblivion blacks the
  // world out under a cinematic). No frame is prepared then, so nothing can take effect; but the simulation says each thing
  // once, and what lasts must not be lost with what is of the moment (review of 2026-11-09, effects 3: after a jump back
  // every structure built on the way stood without its pad, units without their running emitters and their beams, and an
  // effect ended during a hold stayed for good). Kept, in the order they came, and queued before anything else the moment
  // the world is shown again (the next Queue or Update):
  //   a Create of what hangs on something, lies for good or for a time, or is still playing: emitters on an entity or at a
  //     place, trails, beams, marks on the ground (a mark for a time until that has run out, an emitter with an end until
  //     it would have nothing left to show: an explosion under way when the world is shown again is there, one long over
  //     is not);
  //   the last Call of each kind on such a one, or on an effect alive from before;
  //   a Destroy of an effect alive from before. One made and destroyed within the hold goes with all that was said of it.
  // Let go of, as before: flashes of light and the prints under feet and treads (a print is laid where its bone is at the
  // moment, which no later frame has). `tick`: the moment the picture is asked for, the game's clock in beats, where the
  // host gives one (a host that holds the picture may ask for moment 0: then all but the newest two beats' events count as
  // past). Events of beats not yet reached are kept untouched, and queued after the others when the world is shown.
  void QueueHeld(std::vector<FrameEffectEvent>&& events, double tick);
  // Lets every event up to `tick` take effect, moves what hangs on something to where that is in
  // `frame`, and lists this frame's beams. Call before Particles::Update and Ribbons::Update, whose
  // clock must be tick / 10 seconds.
  void Update(double tick, const Frame& frame);
  // [restart] (World::RestartGame, the interface's request 45 R1) A new game on the same map: every effect kept and every
  // event not yet due let go of (its beats begin again near 0, and its entities' numbers are used again), the counts begun
  // again. What the effects made is let go of by its owners (Particles::Clear, Ribbons::Clear, Decals::ForgetGame,
  // Lights::Forget).
  void Clear();
  // Where emitters that stay where they were made were moved out to since the last call (OffsetEmitter:
  // a shield's hit effects are made at its centre and moved out to where it was struck).
  void TakeHits(std::vector<Vec3>* out) {
    out->swap(hits_);
    hits_.clear();
  }
  // [devicelost] What the scripts made that still lasts, as the events that made it, for a world made anew after the
  // graphics device was lost (World::Keep; the simulation says each thing once and cannot say it again): every effect
  // that hangs on something still there (a unit's running emitters, its beams, an adjacency's links, a shot's trail) with
  // the calls that changed it (the last of each kind), every mark laid on the ground for good (the pads under buildings,
  // the deposits' marks) and every mark laid for a time that has not run out (scorches, craters), and the events not yet
  // due. Queue them on the new world's effects. The hanging effects begin at `tick` (an emitter started "then" would
  // throw out at once all it should have thrown since); the marks keep their own beat, so they fade and go when they
  // would have. Not kept: what was made at a place and plays out there (an explosion's emitters under way), the prints
  // under feet and treads, flashes of light. Reads this object alone: also right when the device is lost.
  void Keep(std::vector<FrameEffectEvent>* out, uint32_t tick) const;
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
  // [fogbeam] Does the army the picture is for see that place now (the world's sight; true where there is no fog of war)?
  // Asked each frame for a beam or a trail that hangs on a thing in none of the frame's lists (Update), which no list can
  // answer for. None: such a one stays as it was made (by the place's answer then, the event's `seen`), as an emitter on
  // such a thing always does.
  std::function<bool(Vec3)> seenAt;
  int unlistedHidden = 0;  // [fogbeam] (this frame: effects on a thing in no list not shown because the army does not see the place)
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
  // [warpin] (World::warpInColours, the modern look with the remastered effects) the light flashes a warp-in's or a
  // teleport's own effect throws (UnitTeleport01..03's scripts: CreateLightParticleIntel with a ramp's name, which no
  // emitter's folder reaches) take the army's faction's ramp where the visuals track has made one beside her warp-in
  // textures, /remastered/textures/particles/warpin/<faction>_<ramp>.dds (her section 119, ask 3: ramp_blue_13 for the
  // Aeon, the Cybran and the Seraphim; the UEF keeps the game's blue). The light and the glow on the ground both.
  bool warpInColours = false;
  int warpInFlashes = 0;  // (how many took a faction's ramp: for the check)

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
    bool unlistedUnseen = false;  // [fogbeam] made on an entity in no list where the army did not see (hidden while so)
    bool unlisted = false;        // [fogbeam] made on an entity in no list, and it has been in none since (Update asks seenAt)
    bool original = false;     // (buildfx) made in the original effects' look whatever the world's (the stand's right side)
    float beamLength = -1e30f, beamThickness = -1e30f;  // (unitsfx) what SetBeamParam set (below -1e29: not set; exhausts.h)
    // [devicelost] (Keep) the event that made it and the calls that changed it since, the last of each kind (the events
    // themselves, moved here when they had taken effect: nothing is copied); empty for one that hangs on nothing
    std::vector<FrameEffectEvent> made;
  };
  void Apply(const FrameEffectEvent& event, const Frame& frame);
  void Remember(FrameEffectEvent&& event);  // [devicelost] (an event that has taken effect, kept where it made something lasting)
  void End(uint32_t id);
  // Where an entity's bone is in the frame. False when the entity is not in it.
  // `model`: where the unit's model is remembered between calls (may be null).
  bool AttachPoint(const Frame& frame, uint32_t entity, int bone, Vec3* position, Quat* orientation, const Model** model = nullptr);
  // Is that entity one the army the picture is for does not see (a unit that is not seen now, a
  // shot where it does not see)? What hangs on such a one is not shown.
  // (by this frame's lists of shots and script entities: Update makes them first)
  bool Hidden(const Frame& frame, uint32_t entity) const;
  bool Listed(const Frame& frame, uint32_t entity) const;  // [fogwar] is it in those lists at all (else Hidden says no)
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
  // [heldworld] (QueueHeld) what is kept while the world is not shown, by the order it came; and each effect's events
  // among them, by the effect's number (the Create that made it, where it was made in the hold, and the calls on it)
  std::map<uint64_t, FrameEffectEvent> held_;
  std::unordered_map<uint32_t, std::vector<uint64_t>> heldOf_;
  std::deque<FrameEffectEvent> heldLater_;  // (those of beats the hold has not reached yet, as they came)
  uint64_t heldNext_ = 0;
  size_t heldSwept_ = 0;  // (how many were kept after the last sweep)
  double heldNow_ = 0;    // (the moment the hold has reached, in beats)
  void Hold(FrameEffectEvent&& event);  // (one event whose beat has come)
  void SweepHeld();       // (those that have run out let go of)
  void ReleaseHeld();     // (the world is shown again: all of them into pending_)
  std::unordered_map<uint32_t, Effect> effects_;
  // the decals a script laid for good, by the effect's number: taken away when it destroys them
  std::unordered_map<uint32_t, uint32_t> decalOf_;
  // [devicelost] (Keep) the events that laid them; and those that laid marks for a time (in the order laid; the ones
  // run out dropped from the front now and then)
  std::unordered_map<uint32_t, FrameEffectEvent> decalMade_;
  std::deque<FrameEffectEvent> timedMade_;
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
