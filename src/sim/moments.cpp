// The moments of a replay for the interface's timeline: sim/moments.h says what is found and how.
#include "sim/moments.h"

#include <algorithm>
#include <memory>
#include <set>
#include <unordered_map>

#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "core/workers.h"
#include "script/mount.h"
#include "sim/command_stream.h"
#include "sim/fingerprint.h"
#include "sim/replay.h"
#include "sim/session.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

const char* MomentKindName(Moment::Kind kind) {
  switch (kind) {
    case Moment::CommanderDied: return "CommanderDied";
    case Moment::ExperimentalFinished: return "ExperimentalFinished";
    case Moment::ExperimentalDied: return "ExperimentalDied";
    case Moment::ArmyDefeated: return "ArmyDefeated";
    case Moment::GameEnded: return "GameEnded";
    case Moment::NukeLaunched: return "NukeLaunched";
    case Moment::NukeLanded: return "NukeLanded";
    case Moment::NukeIntercepted: return "NukeIntercepted";
  }
  return "?";
}

size_t Moments::CopySince(size_t nMoments, size_t nBuckets, std::vector<Moment>* moments,
                          std::vector<std::vector<FightBucket>>* buckets, int* lastTickNow, bool* finishedNow) const {
  std::lock_guard<std::mutex> lock(mutex);
  if (moments && nMoments < this->moments.size())
    moments->insert(moments->end(), this->moments.begin() + static_cast<std::ptrdiff_t>(nMoments), this->moments.end());
  size_t now = 0;
  if (buckets && buckets->size() < fights.size()) buckets->resize(fights.size());
  for (size_t a = 0; a < fights.size(); ++a) {
    const std::vector<FightBucket>& list = fights[a];
    now = std::max(now, list.size());
    if (buckets && nBuckets < list.size())
      (*buckets)[a].insert((*buckets)[a].end(), list.begin() + static_cast<std::ptrdiff_t>(nBuckets), list.end());
  }
  if (lastTickNow) *lastTickNow = lastTick;
  if (finishedNow) *finishedNow = finished;
  return now;
}

namespace {

// What the scan looks at after every beat. Everything here reads C++ values of the sim (no Lua, no lookup that
// inserts: a std::map is read with find, BpInCategory's per-sim cache is not used); its own state is its own.
class Observer {
 public:
  explicit Observer(const Sim& sim) {
    // the blueprints in a category, as bits by entity index (the categories as the blueprints were loaded:
    // SimBlueprints::Categories, which nothing changes during the game)
    const int n = sim.blueprints().EntityCount();
    auto bits = [&](const char* name) {
      std::vector<bool> v(static_cast<size_t>(n > 0 ? n : 0), false);
      auto it = sim.blueprints().Categories().find(name);
      if (it != sim.blueprints().Categories().end())
        for (int i : it->second)
          if (i >= 0 && i < n) v[static_cast<size_t>(i)] = true;
      return v;
    };
    commander_ = bits("COMMAND");
    experimental_ = bits("EXPERIMENTAL");
    const std::vector<bool> strategic = bits("STRATEGIC"), missile = bits("MISSILE");
    nuke_.assign(strategic.size(), false);
    for (size_t i = 0; i < nuke_.size(); ++i) nuke_[i] = strategic[i] && missile[i];
    // what a strategic missile does not explode on: FAF's NukeProjectile.OnImpact (lua/sim/projectiles/
    // NukeProjectile.lua 110-122) explodes unless what it met is in PROJECTILE * ANTIMISSILE * TECH3 (the strategic
    // missile defences' interceptors)
    const std::vector<bool> projectile = bits("PROJECTILE"), anti = bits("ANTIMISSILE"), tech3 = bits("TECH3");
    interceptor_.assign(projectile.size(), false);
    for (size_t i = 0; i < interceptor_.size(); ++i) interceptor_[i] = projectile[i] && anti[i] && tech3[i];
    // the armies as the session starts: those already out of the game, the kill counts so far
    for (const auto& a : sim.armies()) {
      outOfGame_.push_back(a->outOfGame);
      killed_.push_back(Killed(*a));
    }
  }

  // After a beat (Tick or PausedBeat), with that beat's hits and impacts: the moments into *moments (in the order
  // moments.h gives), the fighting into *fights (as Moments::fights, one bucket: this beat's).
  void Beat(const Sim& sim, const std::vector<HitEvent>& hits, const std::vector<ImpactEvent>& impacts,
            std::vector<Moment>* moments, std::vector<FightBucket>* fights) {
    ++beat_;
    const int tick = static_cast<int>(sim.tick());
    auto add = [&](Moment::Kind kind, int army, const Vec3& pos, const std::string& unit, uint32_t entity) {
      Moment m;
      m.tick = tick;
      m.kind = kind;
      m.army = army;
      m.pos = pos;
      m.unit = unit;
      m.entity = entity;
      moments->push_back(std::move(m));
    };

    // Commanders and experimentals (sim.units(): every unit the sim holds, by entity id; killed ones stay in it
    // until their Destroy has run)
    for (const Unit* u : sim.units()) {
      const BlueprintInfo* bp = u->blueprint;
      if (!bp || bp->entityIndex < 0 || static_cast<size_t>(bp->entityIndex) >= commander_.size()) continue;
      const bool cmd = commander_[static_cast<size_t>(bp->entityIndex)];
      const bool exp = experimental_[static_cast<size_t>(bp->entityIndex)];
      if (!cmd && !exp) continue;
      auto [it, isNew] = units_.try_emplace(u->handle);
      Seen& s = it->second;
      s.beat = beat_;
      if (isNew) {  // (first seen: nothing has happened to it yet that we saw)
        s.building = u->beingBuilt;
        s.dead = u->dead;
        continue;
      }
      const int army = u->army ? u->army->index - 1 : -1;  // (0-based: Moment::army)
      if (exp && s.building && !u->beingBuilt && !u->dead) add(Moment::ExperimentalFinished, army, u->position, bp->id, u->id);
      if (!s.dead && u->dead) {
        if (cmd) add(Moment::CommanderDied, army, u->position, bp->id, u->id);
        if (exp && !s.building && !u->beingBuilt) add(Moment::ExperimentalDied, army, u->position, bp->id, u->id);
      }
      s.building = u->beingBuilt;
      s.dead = u->dead;
    }
    for (auto it = units_.begin(); it != units_.end();)  // (gone: destroyed)
      it = it->second.beat == beat_ ? std::next(it) : units_.erase(it);

    // Armies out of the game: defeated while an enemy (not a civilian army: those never leave) is still in the game
    // after this beat, else the game ended for it (the winners, or a draw: FAF tells them apart only in its scripts,
    // brain.Status and Sync's GameResult)
    const auto& armies = sim.armies();
    for (size_t i = 0; i < armies.size() && i < outOfGame_.size(); ++i) {
      const Army& a = *armies[i];
      if (!a.outOfGame || outOfGame_[i]) continue;
      bool enemyLeft = false;
      for (size_t j = 0; j < armies.size(); ++j)
        enemyLeft |= j != i && j < a.alliance.size() && a.alliance[j] == 0 && !armies[j]->outOfGame && !armies[j]->civilian;
      Vec3 at{a.startX, 0, a.startZ};
      if (sim.map()) at.y = sim.map()->TerrainHeight(a.startX, a.startZ);
      add(enemyLeft ? Moment::ArmyDefeated : Moment::GameEnded, static_cast<int>(i), at, std::string(), 0xffffffffu);
    }
    for (size_t i = 0; i < armies.size() && i < outOfGame_.size(); ++i) outOfGame_[i] = armies[i]->outOfGame || outOfGame_[i];

    // Strategic missiles: their own impacts, what met them, then the ones in the air. Whether what a missile met is
    // an interceptor: one seen in the air at the end of the last beat (interceptors_: their ids; an id is not given
    // again for 99 beats), or one still there now
    auto isInterceptor = [&](uint32_t id) {
      if (interceptors_.count(id)) return true;
      const Entity* e = sim.FindEntityById(id);
      return e && e->kind == Entity::Kind::Projectile && In(interceptor_, e->blueprint);
    };
    for (const ImpactEvent& ev : impacts) {
      for (auto& [handle, n] : nukes_) {
        if (n.ended) continue;
        if (ev.kind == ImpactEvent::Projectile && ev.source == n.id) {
          const bool intercepted = ev.entity != 0xffffffffu && isInterceptor(ev.entity);
          add(intercepted ? Moment::NukeIntercepted : Moment::NukeLanded, n.army,
              Vec3{ev.position[0], ev.position[1], ev.position[2]}, n.unit, n.id);
          n.ended = true;
        } else if (ev.entity == n.id && !n.met) {  // (an interceptor's impact on it)
          n.met = true;
          n.metAt = Vec3{ev.position[0], ev.position[1], ev.position[2]};
        }
      }
    }
    interceptors_.clear();
    for (const Projectile* p : sim.projectiles) {
      const BlueprintInfo* bp = p->blueprint;
      if (In(interceptor_, bp)) interceptors_.insert(p->id);
      if (!In(nuke_, bp)) continue;
      auto [it, isNew] = nukes_.try_emplace(p->handle);
      Nuke& n = it->second;
      n.beat = beat_;
      n.last = p->position;
      if (!isNew) continue;
      n.id = p->id;
      n.army = p->army ? p->army->index - 1 : -1;
      // the firing unit's blueprint id: the first part of the weapon's id ("<unit>-<index>-<label>"), read as the
      // impacts' list reads it (WeaponBlueprintId, sim/damage.cpp RecordImpact)
      const std::string weapon = WeaponBlueprintId(p->weapon);
      n.unit = weapon.empty() ? bp->id : weapon.substr(0, weapon.find('-'));
      add(Moment::NukeLaunched, n.army, p->position, n.unit, n.id);
    }
    for (auto it = nukes_.begin(); it != nukes_.end();) {
      Nuke& n = it->second;
      if (n.beat == beat_) {
        ++it;
        continue;
      }
      if (!n.ended) add(Moment::NukeIntercepted, n.army, n.met ? n.metAt : n.last, n.unit, n.id);
      it = nukes_.erase(it);
    }

    // The fighting: hits by army, kills of each army
    // ([i]: army i, 0-based; [armies]: no army. HitEvent::army is 1-based, 0 = none)
    fights->assign(armies.size() + 1, FightBucket{});
    for (const HitEvent& h : hits) {
      if (h.targetKind != 0 && h.targetKind != 1) continue;
      const size_t a = h.army >= 1 && static_cast<size_t>(h.army) <= armies.size() ? static_cast<size_t>(h.army - 1) : armies.size();
      (*fights)[a].damage += h.amount;
      ++(*fights)[a].hits;
    }
    for (size_t i = 0; i < armies.size() && i < killed_.size(); ++i) {
      const float k = Killed(*armies[i]);
      if (k > killed_[i]) (*fights)[i].deaths += static_cast<int>(k - killed_[i] + 0.5f);
      killed_[i] = k;
    }
  }

 private:
  static bool In(const std::vector<bool>& set, const BlueprintInfo* bp) {
    return bp && bp->entityIndex >= 0 && static_cast<size_t>(bp->entityIndex) < set.size() &&
           set[static_cast<size_t>(bp->entityIndex)];
  }
  static float Killed(const Army& a) {
    auto it = a.stats.find("Units_Killed");
    return it == a.stats.end() ? 0.0f : it->second;
  }
  struct Seen {
    uint64_t beat = 0;  // the last beat it was seen in
    bool building = false, dead = false;
  };
  struct Nuke {
    uint64_t beat = 0;
    uint32_t id = 0xffffffffu;
    int army = -1;
    std::string unit;
    Vec3 last;
    bool ended = false;  // its own impact was seen
    bool met = false;    // something's impact met it
    Vec3 metAt;
  };
  std::vector<bool> commander_, experimental_, nuke_, interceptor_;  // by blueprint entity index
  std::set<uint32_t> interceptors_;  // the interceptors in the air at the end of the last beat (entity ids)
  std::vector<bool> outOfGame_;
  std::vector<float> killed_;
  // (by entity handle: never reused; the order of these maps never reaches the moments, which follow the sim's
  // own lists)
  std::unordered_map<uint32_t, Seen> units_;
  std::map<uint32_t, Nuke> nukes_;
  uint64_t beat_ = 0;
};

}  // namespace

bool ScanReplayMoments(const ScanArgs& args, Moments* out, std::atomic<int>* tick, const std::atomic<bool>* cancel) {
  auto fail = [&](const std::string& why) {
    Logf(LogLevel::Error, "moments: %s", why.c_str());
    std::lock_guard<std::mutex> lock(out->mutex);
    out->error = why;
    return false;
  };
  // The copy's own worker pool (1 thread by default), as --together's second copy has its own
  ThreadsOverride threads(args.threads > 0 ? args.threads : 1);
  // Its own file system, mounted as the host's (src/app/main.cpp MountGame): the in-memory Game.prefs first (the
  // rules must not read the original's), the init file, the extra map folders; the prefs then name the replay's
  // own map folder (RunTimes)
  Vfs vfs;
  std::vector<std::string> hookDirs;
  vfs.MountMemoryFile(kGamePrefsPath, PreGameDataPrefs(""));
  if (!MountFromInitFile(args.init, args.folders, vfs, hookDirs, &args.skipMounts))
    return fail("the init file " + args.init + " could not be run");
  for (const std::string& folder : args.maps)
    if (!vfs.Mount(folder, "/maps")) Logf(LogLevel::Warning, "moments: no map folder %s", folder.c_str());
  for (const auto& [folder, point] : args.mounts)
    if (!vfs.Mount(folder, point)) Logf(LogLevel::Warning, "moments: %s could not be mounted at %s", folder.c_str(), point.c_str());
  const auto data = hostfs::ReadFile(args.replay);
  if (!data) return fail("cannot read the replay " + args.replay);
  std::string why;
  const auto header = ReadReplayHeader(*data, &why);
  if (!header) return fail(why);
  SessionDescription session;
  session.header = *header;
  session.replay = true;
  vfs.SetMemoryFile(kGamePrefsPath, PreGameDataPrefs(SessionMapFolder(session.header)));

  Sim sim(&vfs, hookDirs, args.modUids);
  sim.sessionMods = header->mods;
  sim.recordSuffix = ".scan";  // (MOHO64_RECORD, if set, is the watched game's)
  if (!sim.LoadRules()) return fail("the blueprints could not be loaded (see the log)");
  sim.collectHits = sim.collectImpacts = args.observe;
  if (!sim.Start(session)) return fail("the session could not be started (see the log)");
  std::unique_ptr<Fingerprint> fingerprint = args.fingerprint ? Fingerprint::FromEnvironment(".scan") : nullptr;
  if (fingerprint) fingerprint->Beat(sim, 0);
  std::unique_ptr<Observer> observer = args.observe ? std::make_unique<Observer>(sim) : nullptr;
  {
    std::lock_guard<std::mutex> lock(out->mutex);
    out->armies = static_cast<int>(sim.armies().size());
    out->fights.assign(sim.armies().size() + 1, {});
  }
  Logf(LogLevel::Info, "moments: scanning %s (%zu armies, %d thread(s))%s", args.replay.c_str(), sim.armies().size(),
       ThreadsFromEnvironment(), args.observe ? "" : ", not looking (a check)");

  // The beats, as the headless host plays them (src/app/main.cpp RunSession): the messages before each Advance,
  // then the beat, or while paused / after EndGame only its end-of-beat part
  CommandStream stream(*data, header->streamOffset);
  int done = 0, paused = 0;
  bool ended = false;
  std::vector<Moment> moments;
  std::vector<FightBucket> fights;
  auto afterBeat = [&] {
    if (fingerprint) fingerprint->Beat(sim, static_cast<uint32_t>(done + paused));
    if (tick) tick->store(static_cast<int>(sim.tick()), std::memory_order_release);
    const int t = static_cast<int>(sim.tick());
    if (!observer) {
      std::lock_guard<std::mutex> lock(out->mutex);
      out->lastTick = t;
      return;
    }
    const std::vector<HitEvent> hits = sim.TakeHits();
    const std::vector<ImpactEvent> impacts = sim.TakeImpacts();
    moments.clear();
    observer->Beat(sim, hits, impacts, &moments, &fights);
    const size_t bucket = static_cast<size_t>(t / kFightBucketTicks);
    std::lock_guard<std::mutex> lock(out->mutex);
    out->moments.insert(out->moments.end(), moments.begin(), moments.end());
    for (size_t a = 0; a < out->fights.size() && a < fights.size(); ++a) {
      std::vector<FightBucket>& list = out->fights[a];
      while (list.size() <= bucket) {
        FightBucket b;
        b.tick0 = static_cast<int>(list.size()) * kFightBucketTicks;
        list.push_back(b);
      }
      list[bucket].damage += fights[a].damage;
      list[bucket].hits += fights[a].hits;
      list[bucket].deaths += fights[a].deaths;
    }
    out->lastTick = t;
  };
  while (!(cancel && cancel->load(std::memory_order_acquire))) {
    if (args.ticks > 0 && done >= args.ticks) {
      ended = true;
      break;
    }
    if (stream.AtEnd() || !stream.ApplyUntilAdvance(sim)) {  // (the replay's last beat has been played)
      ended = true;
      break;
    }
    if (sim.Paused()) {
      sim.PausedBeat();
      ++paused;
      afterBeat();
      if (sim.frozen && stream.AtEnd()) {
        ended = true;
        break;
      }
      continue;
    }
    sim.Tick();
    ++done;
    afterBeat();
  }
  if (fingerprint) fingerprint->End(static_cast<uint32_t>(done + paused));
  Logf(LogLevel::Info, "moments: %s after %d beats (%d paused), tick %u", ended ? "done" : "cancelled", done + paused,
       paused, sim.tick());
  std::lock_guard<std::mutex> lock(out->mutex);
  out->finished = ended;
  return ended;
}

}  // namespace moho
