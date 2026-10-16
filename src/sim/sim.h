// The simulation: its Lua state, armies and their brains, the world, and the tick loop.
//
// Start-up follows the original (see lua/simInit.lua's header): blueprints are copied in,
// simInit.lua runs, ScenarioInfo is filled from the session, SetupSession() runs, the armies
// and their brains are created (OnCreateArmyBrain), then BeginSession().
#pragma once
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/economy.h"
#include "sim/entity.h"
#include "sim/replay.h"
#include "sim/skeleton.h"
#include "sim/units.h"

namespace moho {

class Vfs;
class ScriptState;
class ThreadScheduler;
class AiBrain;
class TerrainMap;
class Unit;
class Prop;
class EntityGrid;
class Platoon;
class Navigation;
struct UnitCommand;
struct SessionDescription;

class Army {
 public:
  int index = 0;  // 1-based, as Lua sees it
  std::string name;      // "ARMY_1"
  std::string nickname;  // player name
  int faction = 0;       // 0-based faction index (UEF = 0)
  bool civilian = false;
  bool human = false;
  bool outOfGame = false;
  // What the interface shows of the army: the lobby's colour numbers (the options' ArmyColor / PlayerColor,
  // indices into the game's colour tables, 0 = not given), a colour a script set outright (SetArmyColor,
  // -1 = none), and SetArmyShowScore.
  int armyColor = 0, playerColor = 0;
  int rgb[3] = {-1, -1, -1};
  bool showScore = true;
  std::string plans;
  AiBrain* brain = nullptr;
  Platoon* pool = nullptr;      // the "ArmyPool" platoon every army has
  std::vector<Platoon*> platoons;  // CArmyImpl+0x228, in creation order (sim/ai_platoon.cpp)
  float startX = 0, startZ = 0;
  bool hasStart = false;
  uint32_t serial = 0;          // entity ids of this army
  ArmyEconomy econ;  // sim/economy.cpp
  std::vector<uint64_t> buildRestricted;  // AddBuildRestriction(army, category)
  std::map<std::string, float> stats;  // army statistics (GetArmyStat)
  // stat triggers (engine-ref army_stats.md): SetArmyStatsTrigger appends, CArmyStats::Update fires and removes
  struct StatCond {
    std::string stat;
    int op = 0;  // 0 >, 1 >=, 2 <, 3 <=
    float thr = 0;
    bool category = false;  // category conditions sum per-blueprint stats (not kept: they read 0)
  };
  struct StatTrigger {
    std::string name;
    std::vector<StatCond> conds;
  };
  std::vector<std::shared_ptr<StatTrigger>> statTriggers;
  float unitCap = 1000;
  float unitCost = 0;  // sum of the live units' General.CapCost (GetArmyUnitCostTotal)
  bool ignoreUnitCap = false;
  // SetIgnorePlayableRect (CArmyImpl+0x178, SSTIArmyVariableData+0xf8, 0 from its ctor 0x6fd475): the army's units
  // move, build, path and see on the whole map instead of the playable rect (Sim::UseWholeMap).
  bool ignorePlayableRect = false;
  // CArmyImpl+0x158: "Human" for a human army, else the options' AIPersonality (the ctor, 0x6fe953 / 0x6fe99d);
  // SetArmyAIPersonality replaces it with a non-empty name. Nothing in the simulation reads it back.
  std::string personality;
  std::vector<int> sources;  // command sources allowed to command this army (army+0x130, replay header)
  std::vector<int> alliance;  // per army index: 0 enemy, 1 neutral, 2 ally
  std::vector<Unit*> units;   // the army's live units, by entity id
};

// A resource deposit (CreateResourceDeposit): cells [x0, x1) x [z0, z1); type 1 mass, 2 hydrocarbon.
struct ResourceDeposit {
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0, type = 0;
};

class AiPersonality;
class AiBrain : public ScriptObject {
 public:
  AiBrain() { typeBits |= kTypeBrain; }
  Army* army = nullptr;
  AiPersonality* personality = nullptr;  // sim/ai_personality.h (owned by the Sim)
  std::shared_ptr<struct AiBuildPlans> buildPlans;  // brain+0x98: BuildStructure's pending plans (sim/ai_build.cpp)
};

template <> struct ScriptTypeOf<AiBrain> { static constexpr uint32_t bit = kTypeBrain; };

class Sim {
 public:
  Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids);
  ~Sim();

  // Rules state: run the blueprint loader and keep the blueprints for the sim.
  bool LoadRules();
  // Start the session described by a replay header (no commands are played yet).
  bool Start(const ReplayHeader& replay);
  // Start the session a lobby launch describes (sim/session.h): the same start, with the launch's focus army.
  // As with a replay, sessionMods must hold the session's mods (description.header.mods) before LoadRules.
  bool Start(const SessionDescription& session);
  // Advance one tick (runs the script threads that are due).
  void Tick();
  // A paused beat (AdvanceBeat steps 15-19 + Sync): destroy queue and id pools only; the game tick stays.
  void PausedBeat();
  // The scripts' Sync table of each beat, as bytes (SerializeLua), oldest first, for a host that shows the
  // game: set collectSync before the beats and take the list after every Tick / PausedBeat (each entry is one
  // beat; the host makes it the user state's Sync and calls OnSync()). With or without a host the table is
  // emptied after every beat (the scripts' ResetSyncTable), as the original does.
  bool collectSync = false;
  std::vector<std::string> TakeSync();
  // Changing the army the session looks through during a game (the replay's army list, an observer's choice,
  // NoteGameOver's SetFocusArmy(-1), the score screen), the original's way (ui REQUESTS 29):
  // - Call RequestFocusArmy(index0): the 0-based army index, or -1 for no army (an observer's or a replay's view
  //   of everyone). The host checks it first, as the original's interface does (CWldSession::RequestFocusArmy
  //   0x8965e0: -1 .. armies - 1, else only the log line; ValidateFocusArmyRequest 0x896670); the engine takes
  //   the number as it comes (SimDriver::SetArmyIndex 0x73b1b0 checks nothing). It may be called from the host's
  //   thread while a beat runs on another.
  // - It only waits (the driver's sync filter, SimDriver+0xb0) until the next Sync: the end of the next Tick or
  //   PausedBeat (or of the beat running now). That beat still runs with the old focus. At the start of its Sync
  //   (Sim::Sync 0x7474b0): focusArmy takes the new value first (sim+0xa88, 0x7474ef), then the scripts'
  //   NoteFocusArmyChanged(new, old) runs (1-based, -1 for none; GetFocusArmy already answers the new army in
  //   it), and the beat's Sync table is handed over after that (0x747b11). With FAF's SimSync.lua that Sync then
  //   holds Sync.FocusArmyChanged = {new = , old = }, Sync.UnitData of the new army's units, Sync.ReleaseIds of
  //   the old army's, the enhancements again (SyncUnitEnhancements), a ping flush; score.lua sends Sync.Score by
  //   the sim's GetFocusArmy from then on. Asking twice before the Sync: the last one counts; asking back for the
  //   army in focus before the Sync: nothing happens. (Before the first beat: applied with beat 1's Sync, into
  //   which ours folds the original's initial "beat 0" Sync.)
  // - The sim's own scripts change it the same way, with FAF's sim global SetFocusArmy(index0) (simInit.lua's
  //   MergeArmyPair): applied at the end of the beat that called it.
  // - Read focusArmy after each Tick / PausedBeat, with that beat's TakeSync (not while a beat runs on another
  //   thread: its Sync writes it): it is the focus that beat's Sync was made with (the original's sync packet +4,
  //   0x74762c), and the scripts' GetFocusArmy answers it + 1. When it differs from the host's own, the host does
  //   its part as CWldSession::DoBeat 0x894530 does (its GetFocusArmy / IsObserver, the selection cleared, the
  //   sounds' army, the picture's focus and allies). The command source never changes with it.
  void RequestFocusArmy(int index0) { requestedFocus_.store(index0); }
  int RequestedFocusArmy() const { return requestedFocus_.load(); }  // what the next Sync applies (= focusArmy: none)
  // The scripts' effect calls are listed for a host that shows the game (EffectEvents in sim/sim_view.h); the
  // host takes the list every beat. Off: nothing is kept.
  bool collectEffects = false;
  // The same for the sounds the scripts play in the world (SoundEvents in sim/sim_view.h).
  bool collectSounds = false;
  // The same for the camera shakes the scripts ask for (ShakeEvents in sim/sim_view.h).
  bool collectShakes = false;
  // Run Lua source in the sim state (debugging aid: --sim-lua).
  bool RunString(const std::string& code, const std::string& chunkName);

  lua_State* L() const;
  Vfs* vfs() const { return vfs_; }
  uint32_t tick() const { return tick_; }
  uint32_t sessionSeed() const { return sessionSeed_; }
  Army* GetArmy(lua_State* L, int idx);  // army by 1-based index or name
  const std::vector<std::unique_ptr<Army>>& armies() const { return armies_; }
  const SimBlueprints& blueprints() const { return bps_; }
  const TerrainMap* map() const { return map_.get(); }
  TerrainMap* mutableMap() { return map_.get(); }
  // The playable rect (STIMap +0x08..+0x14: x0, z0, x1, z1, ints). The STIMap ctor 0x5779c0 makes it the whole map,
  // (0, 0, width, height); the scripts' SetPlayableRect changes it (STIMap::SetPlayableMapRect 0x577df0). A campaign
  // operation keeps the player inside part of the map; skirmish maps leave it whole.
  int playableRect[4] = {0, 0, 0, 0};
  // STIMap::IsWithin 0x62ca60: (x, z) with a margin m inside the playable rect (none of x0 > x - m, z0 > z - m,
  // x + m > x1, z + m > z1, compared as floats), or with wholeMap inside the map itself (x - m and z - m not below 0,
  // x + m < width, z + m < height: strictly).
  bool IsWithin(float x, float z, float m, bool wholeMap) const;
  // CArmyImpl::UseWholeMap (army vt+0xb8, 0x6fded0): the army's SetIgnorePlayableRect flag; false without an army.
  static bool UseWholeMap(const Army* a) { return a && a->ignorePlayableRect; }
  // The sim stream (Sim+0x904, CMersenneTwister; engine-ref sim_random.md): MT19937 seeded once with the
  // session seed. Every engine and sim-Lua draw comes from it, in the original's order.
  uint32_t NextUInt32() {
    uint32_t r = rng_();
    ++rngDraws_;
    if (rngTrace_ || rec_) RngTrace(r);
    return r;
  }
  // Tick recorder (MOHO64_RECORD, sim/recorder.cpp; the labhook 0.4 format)
  void RecorderOpen();
  void RecorderDraw(uintptr_t where);
  void RecorderBeat();
  bool Recording() const { return rec_ != nullptr; }
  void RecorderRaw(const void* p, size_t n) { if (rec_) fwrite(p, 1, n, rec_); }
  uint32_t RecorderTick() const { return beatStarted_ ? tick_ : 0xffffffffu; }
  std::vector<uint32_t> attachedEntities;  // plain entities attached to a bone (they follow it each tick)
  // Before an entity moves in the entity stage: its collision shape stays where it is until step 13.
  void HoldShape(Entity* e) {
    if (e->shapeHeld) return;
    e->shapeHeld = true;
    e->shapePos = e->position;
    e->shapeOri = e->orientation;
    heldShapes_.push_back(e);
  }
  std::vector<Entity*> heldShapes_;
  FILE* rec_ = nullptr;
  uint32_t recTick_ = 0xffffffffu;  // the last game tick RecorderBeat wrote
  std::set<uint32_t> recNamed_;     // units whose 'U' record is written, by creation handle (per sim: handles restart)
  uint64_t rngDraws_ = 0;
  bool beatStarted_ = false;
  void RngTrace(uint32_t r);
  bool rngTrace_ = false;
  float Random() { return U01(); }  // = U01
  float U01();                       // (double)u * 2^-32 rounded to float: [0, 1] (1 for u >= 0xFFFFFF80)
  double FRand(float lo, float hi);  // CRandomStream::FRand 0x51b5c0 (callers round)
  float BpUniform(float base, float range);  // base + uniform(-range, range), one rounding
  uint32_t IntRange(uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(NextUInt32()) * n) >> 32); }
  double Gauss();  // 0x40eec0: polar method, the second value cached in the stream
  void GenerateArmyStart(Army* a);  // the army ctor's / Lua GenerateArmyStart's two draws

  // Entities. Creation runs the scripts the way the original does (see sim/entities.cpp).
  // keepY: the construction params' +0x25 (Lua CreateUnit with an explicit layer keeps the given height).
  // wantLayer: the params' +0x28, a layer bit or 0 = Entity::GetStartingLayer chooses.
  // ownHeading: the params' +0x24 is 0 (the factory's and the upgrade's build task, 0x5f54d0): q is not used, the
  // unit stands unturned, or turned by a random angle when its blueprint says Display.SpawnRandomRotation.
  Unit* CreateUnit(lua_State* L, const BlueprintInfo& bp, Army* army, Vec3 pos, Quat q, bool complete,
                   Unit* builder = nullptr, bool keepY = false, uint8_t wantLayer = 0, bool ownHeading = false);
  // Unit::Warp 0x6aa310 (the script's Warp() on a unit): the layer and the height are chosen for the new place
  // as at creation (the given y is not used), the motion is reset there and a move in progress is aborted.
  void WarpUnit(lua_State* L, Unit* u, Vec3 pos, Quat q);
  // PROJ_Create (sim/projectile.cpp): runs OnPreCreate, OnLayerChange and OnCreate; nullptr when
  // it destroyed itself (a homing projectile without a target).
  class Projectile* CreateProjectile(lua_State* L, const BlueprintInfo& bp, Army* army, Entity* launcher, Vec3 pos,
                                     Quat q, float damage, float damageRadius, const std::string& damageType,
                                     const AiTarget& target, bool ignoresAlly);
  std::vector<class Projectile*> projectiles;  // live projectiles, in creation order
  // The shell whose step is running in the projectile pass (null outside the pass): its children start where the
  // step has put it, any other shell's children at that shell's transform of the beat's start.
  class Projectile* steppingProjectile = nullptr;
  std::vector<class ShieldEntity*> shields;    // shield entities (area damage pre-pass, collisions)
  std::vector<class CollisionBeam*> beams;
  std::vector<Prop*> props;
  // entities the scripts made that are neither unit, projectile nor prop (_c_CreateEntity, _c_CreateShield), in
  // creation order: for EntityViews (sim/sim_view.h)
  std::vector<Entity*> scriptEntities;
  std::vector<Entity*> gridScratch;  // (entity_grid.cpp's list for the beat's AdvanceCoords, kept to save allocations)
  // A manipulator's place among those of equal precedence (0x63ace0 puts it behind them): stamped when it is
  // made, when its precedence is set, and when an animator is given an animation (it re-adds its bones).
  uint32_t manipSeq = 0;
  uint32_t NextManipSeq() { return ++manipSeq; }
  // PropEvents (sim/sim_view.h): while set, every prop that goes is noted (its id, its place in the map file)
  bool collectProps = false;
  std::vector<std::pair<uint32_t, int>> propsGone;
  template <class F>
  void ForPropsInRect(float x0, float z0, float x1, float z1, F&& f) {
    if (propGridDirty_) RebuildPropGrid();
    if (x1 < 0 || z1 < 0) return;
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(gridW_ - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gridH_ - 1, static_cast<int>(z1) >> 4);
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (Prop* p : propGrid_[static_cast<size_t>(cz) * gridW_ + cx]) f(p);
  }
  void MarkPropsMoved() { propGridDirty_ = true; }
  // Entity id for a new projectile / shield / beam of an army (separate id families).
  uint32_t ReserveId(Army* army, uint32_t family);
  // Register an engine-created entity (collision beams): owned by the sim, found by id.
  void AddEntity(std::unique_ptr<Entity> e) {
    RegisterEntity(e.get());
    owned_.push_back(std::move(e));
  }
  // A new entity becomes findable: its handle (EntityRef) and its game id.
  void RegisterEntity(Entity* e) {
    e->handle = nextHandle_++;
    entities_[e->handle] = e;
    byId_[e->id] = e;
  }
  Entity* FindEntityById(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
  }
  // EntityDB::ReleaseId (the id is quarantined for 99 beats) and the per-beat IdPool::Update.
  void ReleaseId(uint32_t id);
  void UpdateIdPools();
  // The unit's armour multiplier for a damage type (armordefinition.lua, AlterArmor): 1 if none.
  float ArmorMult(const Unit* u, const std::string& damageType) const;
  Prop* CreateProp(lua_State* L, const BlueprintInfo& bp, Vec3 pos, Quat q, Vec3 scale);
  // _c_CreateEntity / _c_CreateShield: bind a script-made table to a new engine entity.
  Entity* AdoptScriptEntity(lua_State* L, int tableIdx, int specIdx, bool shield);
  Platoon* CreatePlatoon(lua_State* L, Army* army, const std::string& name, const std::string& plan);
  Entity* FindEntity(uint32_t id) const;
  void QueueDestroy(Entity* e);  // Entity:Destroy(): OnDestroy and removal happen at the end of the tick
  // For what dies with a command object (a ferry beacon): the original deletes an emptied command only after
  // the beat's destroy queue has run, so the entity is queued then and goes at the end of the beat after.
  void QueueDestroyAfterBeat(Entity* e);
  const std::map<uint32_t, Entity*>& entities() const { return entities_; }
  // Live units of all armies, by entity id (kept up to date on creation and destruction).
  const std::vector<Unit*>& units() const { return units_; }
  // The command stage (0x958) runs the units' command threads oldest first; a thread suspended on a
  // move and resumed by its navigator is re-linked at the stage's new end (beat_order.md 2.6).
  const std::vector<Unit*>& CommandOrder();
  // Stage sim+0x930 (Entity::TaskTick -> Unit::MotionTick): FIFO by creation, stable across beats
  // (beat_order.md 2.6); entity ids are army-major, so this is not id order.
  const std::vector<Unit*>& MotionOrder();
  // The armies' own stages (brain+0xa8 navigators, +0xac steering, +0xb0 attackers; CArmyImpl::OnTick): army by
  // army, and inside an army the units in the order they were created, which is not id order once an id has
  // been used a second time.
  const std::vector<Unit*>& ArmyStageOrder();
  void ResumeCommandThread(Unit* u);
  uint64_t CommandSeqHigh() const { return cmdSeqNext_; }
  // Units within [x0,x1] x [z0,z1] by position (a 16-unit grid rebuilt when units moved, were
  // created or destroyed); calls f(unit) in id order within each cell row-major - callers that
  // need id order sort.
  template <class F>
  void ForUnitsInRect(float x0, float z0, float x1, float z1, F&& f) {
    if (gridDirty_) RebuildUnitGrid();
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(gridW_ - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gridH_ - 1, static_cast<int>(z1) >> 4);
    if (x1 < 0 || z1 < 0) return;
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
        for (Unit* u : grid_[static_cast<size_t>(cz) * gridW_ + cx]) {
          const Vec3& p = u->position;
          if (p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1) f(u);
        }
  }
  void MarkUnitsMoved() { gridDirty_ = true; }
  // The entity grid behind GetUnitsInRect / GetReclaimablesInRect (sim/entity_grid.h).
  EntityGrid& entityGrid();
  bool anyAttached = false;  // some unit rides a transport (sim/transport.cpp)
  // Call obj:method(args...) for the nargs values on L's stack; logs script errors.
  // (Every function taking a lua_State works on the caller's state: it may be a thread.)
  bool CallMethod(lua_State* L, ScriptObject* obj, const char* method, int nargs);
  // The army the session looks through (sim+0xa88): 0-based, -1 = none (an observer, a replay). Hosts read it;
  // they change it with RequestFocusArmy (applied at the next Sync), never by writing it.
  int focusArmy = -1;
  bool isReplay = true;  // false for a session launched from a game configuration (Start(SessionDescription))
  // Deliberate departures from the original that repair something broken in it or in the game's scripts
  // (docs/deviations.md lists each one). On in a new game, off while a replay is played back, so that a
  // recording of the original still plays as the original did; on again for the replay of a game this engine
  // played with them (its mark: replay.h ScenarioMarksFixes). MOHO64_FIXES=0 / 1 overrides. Those made in the
  // game's Lua are the hook files of the repository's data/fixes/hook, given to the sim state only with `fixes`.
  bool fixes = false;
  // The session's mod list as the lobby sent it (a replay's serialized mods): becomes
  // __active_mods in the rules and sim states. Empty: built from the mod uids instead.
  std::string sessionMods;
  // The session's scenario info as it was launched (serialized Lua; what the interface's SessionGetScenarioInfo
  // returns). The scripts' own ScenarioInfo grows engine objects during the game and is not for handing out.
  std::string sessionScenario;
  bool cheats = false;

  // Command stream state (engine-ref command_stream.md 4 and 7; sim/command_stream.cpp plays the messages)
  struct CmdSource {
    std::string name;
    int pausesLeft = -1;  // source+0x20
  };
  std::vector<CmdSource> cmdSources;  // sim+0x91c, from the replay header
  int commandSource = 0xff;           // sim+0x92c, 0xff = none
  int pauseOwner = -1;                // sim+0x8e0: the source that paused, -1 = running
  bool singleStep = false;            // sim+0x8e4
  bool endGame = false;               // sim+0x8dc (EndGame message)
  bool frozen = false;                // sim+0x8dd (= endGame after the next Sync): beats do nothing
  bool Paused() const { return frozen || (pauseOwner != -1 && !singleStep); }
  bool OkayToMessWith(const Army* a) const;  // 0x747320
  // Stream-issued commands by their stream id (source << 24 | n); Lua-issued ones are in commandsById.
  std::map<uint32_t, std::weak_ptr<UnitCommand>> streamCommands;

  static Sim* From(lua_State* L);
  bool PushImport(lua_State* L, const std::string& module);  // import(module) -> table, or false (logged)

  // Movement (M3): pathfinding, path searches waiting their turn, issued commands.
  Navigation& navigation();
  std::deque<Unit*> pathQueue;
  std::vector<ResourceDeposit> deposits;
  std::map<uint32_t, std::weak_ptr<UnitCommand>> commandsById;
  uint32_t nextCommandId = 1;
  // Keep an engine object alive for the session (objects scripts may still hold).
  ScriptObject* Own(std::unique_ptr<ScriptObject> o) {
    owned_.push_back(std::move(o));
    return owned_.back().get();
  }

 private:
  Vfs* vfs_;
  std::vector<std::string> hookDirs_;
  std::vector<std::string> modUids_;
  std::unique_ptr<ScriptState> rules_;
  std::unique_ptr<ScriptState> state_;
  std::unique_ptr<ThreadScheduler> threads_;
  std::unique_ptr<TerrainMap> map_;
  std::unique_ptr<Navigation> nav_;
  std::unique_ptr<EntityGrid> egrid_;
  SimBlueprints bps_;
  std::vector<std::unique_ptr<Army>> armies_;
  std::mt19937 rng_;
  uint32_t sessionSeed_ = 0;  // what rng_ was seeded with (the replay header's / the lobby's seed)
  float gauss_ = 0;
  bool hasGauss_ = false;
  uint32_t tick_ = 0;
  std::vector<std::unique_ptr<ScriptObject>> owned_;
  std::map<uint32_t, Entity*> entities_;
  std::vector<Unit*> units_;
  std::vector<std::string> pendingSync_;
  void SyncStep();  // the script part of Sim::Sync
  std::atomic<int> requestedFocus_{-1};  // the pending focus army (SimDriver+0xb0); Start sets it to focusArmy
  void ApplyFocusArmy();  // the start of Sim::Sync: a pending focus army takes effect
  std::vector<Unit*> cmdOrder_;
  uint64_t cmdSeqNext_ = 0;
  bool cmdOrderDirty_ = true;
  std::vector<Unit*> motionOrder_;
  uint64_t entSeqNext_ = 0;
  bool motionOrderDirty_ = true;
  std::vector<Unit*> armyOrder_;
  bool armyOrderDirty_ = true;
  std::vector<std::vector<Unit*>> grid_;
  int gridW_ = 0, gridH_ = 0;
  bool gridDirty_ = true;
  void RebuildUnitGrid();
  std::vector<std::vector<Prop*>> propGrid_;
  bool propGridDirty_ = true;
  void RebuildPropGrid();
  // EntityDB id pools (ids_repair_placement.md 1), one per (family, army): the lowest free serial,
  // released ids quarantined in a 100-slot ring for 99 beats.
  struct IdPool {
    uint32_t next = 0;
    std::set<uint32_t> free;
    std::vector<uint32_t> ring[100];
    int head = 0, tail = 1;
  };
  std::map<uint32_t, IdPool> idPools_;  // key: id & 0xFFF00000
  std::map<uint32_t, Entity*> byId_;
  uint32_t nextHandle_ = 1;
  void AddUnitToLists(Unit* u);
  void RemoveUnitFromLists(Unit* u);
  uint32_t propSerial_ = 0;
  std::vector<Entity*> destroyQueue_;
  void ProcessDestroyQueue();
  std::vector<uint32_t> lateDestroy_;  // entity references (QueueDestroyAfterBeat)
  void QueueLateDestroys();

  // Push the script class for a blueprint: bp.ScriptModule/ScriptClass, else
  // <dir>/<name>_script.lua's TypeClass, else the default module's class.
  void PushScriptClass(lua_State* L, const BlueprintInfo& bp, const char* defModule, const char* defClass);
  void InitializeArmor(lua_State* L, Unit* u);
  // The entity's skeleton: Display.MeshBlueprint -> that mesh blueprint's first LOD MeshName.
  void AttachSkeleton(lua_State* L, Entity* e);
  std::unique_ptr<SkeletonCache> skeletons_;
  bool armorLoaded_ = false;
  std::map<std::string, std::map<std::string, float>> armorTypes_;  // armour type -> damage type -> multiplier

  bool CallGlobal(const char* fn, int nargs);  // args pushed; logs errors
  void SetModsGlobal(ScriptState& st);
  bool CreateArmies(const ReplayHeader& replay);
  void CreateMapProps();
};

void RegisterSimBindings(lua_State* L);

}  // namespace moho
