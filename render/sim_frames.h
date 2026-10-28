// Frames from a simulation someone else runs: after every beat the one who holds the simulation
// (the viewer's own simulation thread, or the interface's host) says Beat, and what the renderer
// needs of that beat is copied out; whoever draws asks for the frame at a moment between two beats
// and gets them blended. Beat may be called on one thread and the rest on another.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame.h"
#include "linalg.h"

namespace moho {
class Sim;
class Vfs;
struct PropEvent;
}  // namespace moho

namespace render {

class SimFrames {
 public:
  SimFrames();
  ~SimFrames();

  // Once the game has started: reads the armies' colours as the game shows them (the game's list
  // of colours is /lua/gamecolors.lua in `vfs`) and how every blueprint looks. On the simulation's thread.
  void ReadArmies(const moho::Sim& sim, const moho::Vfs& vfs);
  // The army the picture is for (the engine's number, from 1): from the next beat on every unit
  // and projectile says what that army knows of it (FrameUnit::recon). 0: no one army, as at first.
  void SetFocusArmy(int army) { focus_.store(army); }
  // The map's size, for the grid of what the focus army sees (TakeSight): `cells` x `cells` places
  // over the whole map, asked of the engine's ReconGrid every fifth beat. Without it there is no grid.
  void SetSightGrid(float mapWidth, float mapHeight, int cells = 256);
  // What the focus army sees, when that has been worked out anew since the last call: for every
  // place the bits of `SightBits` (frame.h), row by row from z = 0. An empty grid: there is no
  // focus army any more. False: nothing new.
  bool TakeSight(std::vector<uint8_t>* grid, int* cells);
  // After every beat, on the simulation's thread. For the scripts' effects the simulation must have
  // had `collectEffects` set from before its first beat, and for the scenery's changes `collectProps`.
  void Beat(const moho::Sim& sim);

  uint32_t FirstTick() const;  // of the beats kept
  uint32_t LastTick() const;   // the newest beat
  // The frame at a moment given in ticks, blended between the two beats around it (clamped to the
  // beats kept). False while there is no beat yet.
  bool FrameAt(double tick, Frame* out);
  // One unit's place and turn at that moment, blended as FrameAt blends it (orientation x, y, z, w).
  // False when the beat there does not have it.
  bool UnitAt(double tick, uint32_t id, float position[3], float orientation[4]) const;
  // The effect events of the beats since the last call, in order (each says its beat).
  void TakeEffectEvents(std::vector<FrameEffectEvent>* out);
  // How the scenery changed in the beats since the last call, in order (each says its beat).
  void TakePropEvents(std::vector<FramePropEvent>* out);
  // How the simulation changed the ground's heights in the beats since the last call, in order.
  void TakeGroundChanges(std::vector<FrameGroundChange>* out);
  // The hits, impacts and health changes of the beats since the last call (`out` is emptied first). Only those the
  // simulation kept: it must have `collectHits`, `collectImpacts`, `collectHealth` set (each list on its own). A
  // taker that falls far behind loses the oldest (at most kMostCombat hits and as many impacts are kept).
  void TakeCombat(FrameCombat* out);
  static constexpr size_t kMostCombat = 100000;
  // How the blueprints look, once: the list is handed over and this object's is emptied.
  void TakeLooks(std::vector<BlueprintLook>* out);
  // Each army's colour, by army index from 0; empty until ReadArmies.
  std::vector<Vec3> ArmyColors() const;
  // The session's seed (the same for every player of a game and in its replay); 0 until ReadArmies.
  uint32_t Seed() const;
  // [spikes] What the armies can build, for a host that makes unit kinds ahead of their first drawing (World::MakeAhead,
  // World::readAhead). While on, after each beat every unit kind there for the first time (any army's, whether the focus
  // army sees it or not) is asked, by one unit of it, which unit blueprints it may build now - the simulation's own test
  // for a build order (UnitMayBuild: its blueprint's BuildableCategory less its own and its army's restrictions) - and once
  // more by the first unit of it that is finished (what a building's script restricts once it stands); every kind named
  // for the first time is kept, a kind itself before what it builds. And every 10th beat one kind that builds is asked
  // again, in turn (a commander's restrictions lifted by an upgrade). TakeBuildable hands them over in that order and
  // empties the list. Each asking takes the simulation's thread a fraction of a millisecond (every unit blueprint of the
  // game asked about: 1,356 with Chris's mods). Off at first.
  void WatchBuildable(bool on) { watchBuildable_.store(on); }
  void TakeBuildable(std::vector<std::string>* out);
  // [session] How every blueprint looks as the simulation has it (what ReadArmies keeps for TakeLooks), and the unit
  // blueprints' ids among them. On the simulation's thread (or with no beat running).
  static std::vector<BlueprintLook> LooksOf(const moho::Sim& sim, std::vector<std::string>* unitIds);
  // [session] The props' events of the simulation taken before its first beat (WorldPicture::PrepareSession: what the game
  // makes of the map's props as it starts): handed back, ahead of its own, by the next Beat. Taken from `events`.
  void HoldPropEvents(std::vector<moho::PropEvent>* events);
  // [deposits] The effect events another SimFrames took of the same game before its first beat (PrepareSession's: what the
  // scripts made as the session began - the resource deposits' marks, ScenarioUtils' CreateResources - which the engine's
  // list hands out once only): given back here, ahead of the events of this object's own beats. Taken from `events`.
  void HoldEffectEvents(std::vector<FrameEffectEvent>* events);
  // [session] ReadArmies' result taken from another (`from`, which read the same game before its first beat: PrepareSession's)
  // in place of reading the game again: the armies' colours, the session's seed, the unit blueprints AskBuildable asks
  // about; not the blueprints' looks (TakeLooks gives none: the world was given them as the session was prepared).
  void AdoptArmies(SimFrames& from);

 private:
  struct Beat_ {
    uint32_t tick = 0;
    std::vector<FrameUnit> units;  // in entity-id order
    std::vector<FrameBone> bones;  // what the units' spans point into
    std::vector<FrameProjectile> projectiles;
    std::vector<FrameBeam> beams;
    std::vector<FrameEntity> entities;
  };
  struct Scratch;  // the simulation's view structures, kept out of this header
  std::unique_ptr<Scratch> scratch_;

  mutable std::mutex mutex_;
  std::deque<Beat_> beats_;            // the newest few
  std::vector<std::string> names_;     // blueprint ids, by FrameUnit::blueprint and FrameProjectile::blueprint
                                       // (0: unknown); only grows
  std::vector<Vec3> armyColors_;
  std::vector<BlueprintLook> looks_;
  uint32_t seed_ = 0;
  std::atomic<int> focus_{0};
  float sightWidth_ = 0, sightHeight_ = 0;  // (set before the game runs)
  int sightCells_ = 0;
  std::vector<uint8_t> sight_, sightWork_;
  std::vector<uint8_t> sightRecon_;  // (the engine's ReconGrid's answer, the simulation's thread)
  std::vector<uint8_t> sightEver_;  // what the focus army has seen at some time (the simulation's thread only)
  int sightEverOf_ = 0;
  bool sightNew_ = false, sightWasOn_ = false;
  uint32_t sightBeat_ = 0;
  std::vector<FrameEffectEvent> effectEvents_;  // not yet taken
  std::vector<FramePropEvent> propEvents_;      // not yet taken
  std::vector<FrameGroundChange> groundChanges_;  // not yet taken
  size_t groundSeen_ = 0;  // how many of the map's height changes have been read (the simulation's thread only)
  std::unordered_map<uint32_t, size_t> propWaiting_;  // a prop's change among them: its place in the list
  FrameCombat combat_;                                 // not yet taken
  bool combatKept_[3] = {false, false, false};         // the simulation's collectHits, collectImpacts, collectHealth
  std::unordered_map<uint32_t, size_t> healthWaiting_;  // an entity's place in combat_.health
  std::unordered_map<uint32_t, uint32_t> shellSince_, shellSinceNext_;  // projectiles: the beat each was first seen (the simulation's thread only)
  std::vector<std::string> namesForFrames_;     // the drawing side's copy of names_ (what Frame::blueprints points at)
  std::atomic<bool> watchBuildable_{false};    // [spikes] (WatchBuildable)
  std::vector<std::string> buildable_;         // [spikes] not yet taken
  void AskBuildable(const moho::Sim& sim);     // [spikes] (after a beat, on the simulation's thread)
};

}  // namespace render
