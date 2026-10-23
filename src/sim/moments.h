// The moments of a replay, for the interface's replay timeline (ui REQUESTS 48; docs/engine-answers.md "ui 48"): a
// commander died, an experimental was finished or died, an army was defeated, a strategic missile was launched and
// where it came down, and how much fighting there was per 5 seconds per army.
//
// ScanReplayMoments plays the replay a second time, headless and at full speed, in a Sim of its own on the caller's
// thread, as moho64 --together plays its second copy (src/app/main.cpp RunTimes): its own Vfs, mounted again from
// the init file, with its own in-memory Game.prefs; its own worker pool through ThreadsOverride; no process
// environment changed (MOHO64_* are read process-wide). Its record and fingerprint, when a check asks for them, go
// to MOHO64_RECORD / MOHO64_FINGERPRINT + ".scan", never over the watched game's.
//
// It only READS the simulation: after each beat it looks at the units, armies and projectiles as C++ values, and
// takes the beat's hits and impacts (Sim::collectHits / collectImpacts, render REQUESTS 36, which record without a
// script call, a Lua object or a random draw). No Lua runs for it, nothing is written into the sim. The game it
// plays is the same beat for beat as without it: moho64 --scan-moments with MOHO64_FINGERPRINT, once as it is and
// once with --scan-off, gives the same fingerprint files, the lua part included (tools/twocopies.py --compare).
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sim/entity.h"  // Vec3

namespace moho {

// One moment. tick: the game tick (Sim::tick()) after the beat it happened in: a host that seeks there by playing
// until Sim::tick() reaches it shows the beat right after. army: 0-based, the sim's index (Lua's army number - 1;
// ArmyView::index - 1), -1 for none. entity: the unit's or the missile's
// entity id (as UnitView::id, ProjectileView::id), 0xffffffff for none.
//   CommanderDied         a unit of the category COMMAND (the category the engine itself goes by for commanders:
//                         Unit::Kill 0x6a8090's test, Enemies_Commanders_Destroyed) was killed (Entity::dead set:
//                         Kill, not a Destroy: a recalled or a transferred commander is not a death). pos: where it
//                         stood at the end of that beat; unit: its blueprint id.
//   ExperimentalFinished  a unit of the category EXPERIMENTAL that was seen under construction stands finished
//                         (beingBuilt cleared: OnStopBeingBuilt). One made finished outright (a script's CreateUnit,
//                         a unit given to another army: a new unit there) is not "finished". pos: where it stands.
//   ExperimentalDied      a finished EXPERIMENTAL unit was killed (one killed under construction is not listed;
//                         its kill counts in the fight buckets' deaths).
//   ArmyDefeated          the army is out of the game as the simulation records it (Army::outOfGame: the scripts'
//                         SetArmyOutOfGame, which FAF's victory conditions call for a defeated army,
//                         AbstractVictoryCondition.lua ToObserver; IsDefeated reads it) while at least one army
//                         that is its enemy (civilian armies left out: they never leave) is still in the game after
//                         that beat. Armies already out when the session starts (simInit's merged team armies) are
//                         not listed. pos: the army's start (y on the terrain); unit: "", entity: none.
//   GameEnded             the same, but no enemy army is left in the game: FAF puts the winners out of the game too
//                         when it ends (VictoryForArmy, and DrawForArmy for a draw, both call ToObserver). Which of
//                         the two it was lives only in the scripts (brain.Status, Sync.GameResult), which the scan
//                         does not read. One per army, with the army's start as pos.
//   NukeLaunched          a projectile whose blueprint is in both STRATEGIC and MISSILE (the stock and FAF nukes of
//                         every faction, the Yolona Oss's, the submarines'; what the strategic missile defences'
//                         TargetRestrictOnlyAllow 'STRATEGIC MISSILE' shoots at; not the UEF commander's tactical
//                         nuke, TACTICAL / TACTICALNUKE) is in the air. pos: where it was at the end of that beat;
//                         unit: the blueprint id of the unit whose weapon fired it (the projectile's own when no
//                         weapon is known); entity: the missile.
//   NukeLanded            that missile's own impact (ImpactEvent, Projectile::Impact) on anything but an
//                         interceptor: the ground, the water, a unit, a shield, the air at the end of its life.
//                         FAF's rule (NukeProjectile.OnImpact, lua/sim/projectiles/NukeProjectile.lua 110-122): it
//                         explodes unless what it met is in PROJECTILE * ANTIMISSILE * TECH3. pos: where it struck.
//   NukeIntercepted       that missile ended without exploding: its impact met an interceptor, or it is gone without
//                         an impact of its own. pos: where it met the interceptor, else where it was last seen.
struct Moment {
  enum Kind {
    CommanderDied, ExperimentalFinished, ExperimentalDied, ArmyDefeated, NukeLaunched, NukeLanded, NukeIntercepted,
    GameEnded
  };
  int tick = 0;
  Kind kind = CommanderDied;
  int army = -1;
  Vec3 pos;
  std::string unit;
  uint32_t entity = 0xffffffff;
};
// "CommanderDied", ... (the enum's own names)
const char* MomentKindName(Moment::Kind kind);

// The fighting of one army in one stretch of kFightBucketTicks game ticks, from tick0 on (the beats whose
// Sim::tick() after them is tick0 .. tick0 + kFightBucketTicks - 1). Summed from the beat's hits (HitEvent) and
// the army's own kill count:
//   damage, hits  the hits on units and shields (targetKind 0 and 1; props, projectiles and other entities left
//                 out) whose instigator is of this army (HitEvent::army): the damage it DEALT, after armour, and
//                 how many hits that was;
//   deaths        units of this army that were killed (the army's statistic Units_Killed, which Unit::Kill adds
//                 1 to for every kill, units under construction included): what it LOST.
// So across all armies the damage is all the damage dealt and the deaths all the units lost.
struct FightBucket {
  int tick0 = 0;
  float damage = 0;
  int hits = 0;
  int deaths = 0;
};
inline constexpr int kFightBucketTicks = 50;  // 5 seconds

// What a scan found so far. While a scan runs it appends after every beat, holding `mutex`; a reader on another
// thread copies what is new since its last look with CopySince (which locks it), or locks it itself. After
// ScanReplayMoments has returned no lock is needed.
struct Moments {
  mutable std::mutex mutex;
  std::vector<Moment> moments;  // in tick order (within a beat: units by entity id, then armies, then missiles)
  // fights[army] (0-based, as Moment::army): the army's buckets, one per kFightBucketTicks from tick 0, dense
  // (buckets with nothing in them too) up to the last beat played, every army's list as long as the others';
  // fights[armies] (the last): the hits whose instigator has no army (deaths stay 0). The outer list has the
  // session's armies + 1 entries from the first beat on.
  std::vector<std::vector<FightBucket>> fights;
  int armies = 0;         // the session's armies (set before the first beat)
  int lastTick = 0;       // Sim::tick() after the last beat played
  bool finished = false;  // the replay was played to its end (or to ScanArgs::ticks)
  std::string error;      // why the scan could not start, "" otherwise

  // For a reader on another thread while the scan fills it (takes `mutex`): appends to *moments the moments from
  // index nMoments on, and to (*buckets)[a] each list's buckets from index nBuckets on (*buckets gets as many lists
  // as fights has). Returns how many buckets each list has now; the moments now number nMoments + those appended.
  // While the scan runs its LAST bucket is still being summed: ask again from that bucket's index (the return value
  // - 1) to have it again with its final sums. lastTick / finished (may be null): their values now.
  size_t CopySince(size_t nMoments, size_t nBuckets, std::vector<Moment>* moments,
                   std::vector<std::vector<FightBucket>>* buckets, int* lastTickNow = nullptr,
                   bool* finishedNow = nullptr) const;
};

struct ScanArgs {
  // How the host mounted the game (MountFromInitFile, script/mount.h): its init file, SHGetFolderPath answers, and
  // the map folders mounted at /maps beside the game's own (moho64 --maps).
  std::string init;
  std::map<std::string, std::string> folders;
  std::vector<std::string> maps;
  std::vector<std::string> modUids;  // the mods, as the host plays the replay with them (moho64 --mods)
  std::string replay;                // the replay file (a host path)
  // The copy's worker pool (ThreadsOverride): 1 leaves the cores to the game being watched.
  int threads = 1;
  int ticks = 0;  // stop after this many game ticks; 0: at the replay's end
  // Checks (moho64 --scan-moments): observe = false plays the copy without looking at it (no lists collected, no
  // reads), for comparing its fingerprint with a run that looks; fingerprint: write MOHO64_FINGERPRINT + ".scan"
  // when that variable is set.
  bool observe = true;
  bool fingerprint = false;
};

// Plays args.replay to its end in a Sim of its own on this thread and fills *out as it goes. tick (may be null):
// Sim::tick() after each beat, for a progress bar (the replay's length in ticks is not known before it is played).
// cancel (may be null): when it turns true the scan stops after the beat it is in (it may take as long as one
// beat). Returns true when the replay was played to its end (or to args.ticks); false when it could not start
// (out->error says why) or was cancelled.
bool ScanReplayMoments(const ScanArgs& args, Moments* out, std::atomic<int>* tick, const std::atomic<bool>* cancel);

}  // namespace moho
