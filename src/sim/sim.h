// The simulation: its Lua state, armies and their brains, the world, and the tick loop.
//
// Start-up follows the original (see lua/simInit.lua's header): blueprints are copied in,
// simInit.lua runs, ScenarioInfo is filled from the session, SetupSession() runs, the armies
// and their brains are created (OnCreateArmyBrain), then BeginSession().
#pragma once
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace moho {
class WorkerPool;
template <class Out>
class Speculation;
struct SurfaceContacts;
struct AimAhead;
struct NavAhead;
}
#include "sim/blueprints.h"
#include "sim/economy.h"
#include "sim/entity.h"
#include "sim/replay.h"
#include "sim/sim_cache.h"
#include "sim/sim_view.h"
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
class SyncSnapshots;
struct UnitCommand;
struct CommandIdPool;
struct SessionDescription;

class Army {
 public:
  static void* operator new(size_t n);  // (MOHO64_PAR_WRITETRAP, sim/write_trap.h: pages of its own while on)
  static void operator delete(void* p);
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
  // The unit statistics' values per blueprint (CArmyStatItem+0xa4, a map blueprint -> float): what GetCurrentUnits,
  // GetBlueprintStat and the category stat triggers sum (CArmyStatItem::SumCategory 0x70b580). Written by the Unit
  // ctor, Unit::Materialize, ~Unit (Units_Active, Units_History, Units_BeingBuilt), Unit::Kill (Units_Killed,
  // Enemies_Killed) and SIM_DoDamagePoint (Units_TotalDamageDealt, Units_TotalDamageReceive). The original keys them by
  // blueprint pointer; here by the blueprint's category bit, so the sum's order does not depend on addresses.
  std::map<std::string, std::map<int, float>> bpStats;
  void AddBpStat(const char* name, const BlueprintInfo* bp, float v);
  float SumBpStat(const std::string& name, const uint64_t* category) const;
  // stat triggers (engine-ref army_stats.md): SetArmyStatsTrigger appends, CArmyStats::Update fires and removes
  struct StatCond {
    std::string stat;
    int op = 0;  // 0 >, 1 >=, 2 <, 3 <=
    float thr = 0;
    std::vector<uint64_t> category;  // a 6th argument: the condition sums the stat's per-blueprint values in it
    bool hasCategory = false;
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

// The beat's order (docs/parallel-mode.md): the original's, bit for bit (every replay of the original, the gate), or
// parallel order, our own (new games: the same result at any thread count). Sim::order, decided by Start.
enum class SimOrder { kOriginal, kParallel };

class Sim {
 public:
  Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids);
  ~Sim();

  // Rules state: run the blueprint loader and keep the blueprints for the sim.
  bool LoadRules();
  // The engine's script fixes (docs/deviations.md), joined to a script state as one more hook folder (read from
  // data/fixes/hook at each call). For a host's own states that should see what the sim's see, e.g. the interface
  // host's blueprint state when `fixes` and RulesFixesWanted() (ui REQUESTS 35).
  static void JoinFixesHooks(ScriptState& st);
  // A fix in the blueprints is for one of the game's mods (today the stock campaign mod, deviations.md 4): with
  // `fixes`, Start(SessionDescription) loads the rules again with the fix files.
  bool RulesFixesWanted() const;
  // The rules state for a host's own state (ui REQUESTS 46). The original makes one RRuleGameRules per session
  // (func_WorldSessionUserLoad 0x885de0, at 0x885e53) and exports it to the sim's state and to the interface's
  // (0x885fe6). Set keepRules before Start: Start then leaves the rules state alive after copying the blueprints
  // into the sim's state, and the host takes it with TakeRules once Start is done (nullptr when not kept or
  // already taken), copies what it needs into its own state and frees it. Nothing of the sim's state is in it.
  bool keepRules = false;
  std::unique_ptr<ScriptState> TakeRules();  // (out of line: ScriptState is incomplete here)
  // Start the session described by a replay header (no commands are played yet).
  bool Start(const ReplayHeader& replay);
  // Start the session a lobby launch describes (sim/session.h): the same start, with the launch's focus army.
  // As with a replay, sessionMods must hold the session's mods (description.header.mods) before LoadRules.
  bool Start(const SessionDescription& session);
  // Whether a session plays with the engine's script fixes, as Start(SessionDescription) decides it (session.fixes,
  // else on for a new game and off for a replay unless its scenario carries the mark; MOHO64_FIXES overrides). For a
  // host that needs the answer before Start, e.g. to load its interface rules first (ui REQUESTS 38).
  static bool FixesFor(const SessionDescription& session);
  // Advance one tick (runs the script threads that are due).
  void Tick();
  // A paused beat (AdvanceBeat steps 15-19 + Sync): destroy queue and id pools only; the game tick stays.
  void PausedBeat();
  // The scripts' Sync table of each beat, as bytes (SerializeLua), oldest first, for a host that shows the
  // game: set collectSync before the beats and take the list after every Tick / PausedBeat (each entry is one
  // beat; the host makes it the user state's Sync and calls OnSync()). With or without a host the table is
  // emptied after every beat (the scripts' ResetSyncTable), as the original does.
  // The setup's own Sync comes first and apart (ui REQUESTS 33): Start ends with the original's "beat 0" Sync
  // (Sim::SetupSync; CSimDriver::ThreadCreateSim at 0x73d68d), so TakeSync right after Start returns ONE table: what
  // the setup's scripts wrote (simInit.lua's BeginSession with the map's OnPopulate and OnStart: Sync.CampaignMode,
  // Sync.NewPlayableArea, Sync.StartPositions, Sync.Restrictions ...), made at tick() 0 with focusArmy as it reads
  // after Start; beat 1's TakeSync then returns only beat 1's own. The original's interface runs it as its beat 0,
  // BEFORE the game's screen exists (CWldSession::DoInitializing 0x88c3f0 -> DoBeat 0x88c507, then
  // StopLoadingDialog 0x88c577 and CreateGameInterface 0x88c5cd; beat 1's and on come after). That one table is
  // kept whatever collectSync says, so a host that sets collectSync only after Start still gets it, first, at its
  // first TakeSync (followed by the beats' tables, oldest first); a host that never calls TakeSync keeps it unread.
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
  //   army in focus before the Sync: nothing happens. (Asked after Start and before the first beat: applied with
  //   beat 1's Sync; Start's own "beat 0" Sync has already been made.)
  // - The sim's own scripts change it the same way, with FAF's sim global SetFocusArmy(index0) (simInit.lua's
  //   MergeArmyPair): applied at the end of the beat that called it; called during the setup (BeginSession):
  //   applied by the setup's own Sync at the end of Start, before beat 1, as by the original's beat 0.
  // - Read focusArmy after Start and after each Tick / PausedBeat, with that Sync's TakeSync (not while a beat runs
  //   on another thread: its Sync writes it): it is the focus that Sync was made with (the original's sync packet +4,
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
  // The scripts' print(...) lines, for the host's console (ui REQUESTS 41). The original's sim print (0x74b570)
  // builds the lines (SCR_ConcatArgsAndCall 0x4cd740, below) and keeps each with Sim::Printf (0x7462a0) in a list
  // (sim+0xa18) that every Sync hands to its packet (Sim::Sync, 0x7480aa); the user side gives each line to
  // CON_Printf (CWldSession::DoBeat 0x895260). Nothing of it reaches the log there.
  // - Set collectPrints before Start: the lines printed while the session starts are kept too (the original's
  //   setup Sync carries them). Take them after Start and after every Tick / PausedBeat with TakePrints: every line
  //   since the last take, oldest first (not while a beat runs on another thread). Lines are kept until taken.
  // - A line as the original builds it: the arguments one after the other, each as tostring makes it (the
  //   original: a string or number as it is, anything else through the global tostring; the same text) and up to
  //   its first NUL; before each argument after the first, ((the line's length - 1) & 7) + 1 spaces (8 on an
  //   empty line; not a tab stop); in the text a newline ends the line (an empty one too), a tab is those spaces,
  //   any other character below 0x20 becomes a tab character; what is left at the end is the last line (an empty
  //   rest is no line, so print() gives none).
  // - Kept lines are not logged (the original's way); with collectPrints off, print logs its arguments joined by
  //   tabs, as LOG does (ours: a headless run has no console). The Lua calls are the same either way.
  bool collectPrints = false;
  std::vector<std::string> TakePrints();
  // Hits, impacts and health changes, a list each per beat in the order they happened (HitEvent, ImpactEvent,
  // HealthEvent in sim/sim_view.h; render REQUESTS 36), as the print lines are kept: set the flags before the first
  // beat, take each list after every Tick / PausedBeat with TakeHits / TakeImpacts / TakeHealth (everything since
  // the last take, oldest first; on the thread that runs the beats, between beats; a list not taken keeps growing).
  // Off: nothing is kept, the cost is a flag test. On: recording reads the simulation only (no script call, no Lua
  // object, no random draw), so the game plays the same either way.
  bool collectHits = false;
  bool collectImpacts = false;
  bool collectHealth = false;
  std::vector<HitEvent> TakeHits() const;
  std::vector<ImpactEvent> TakeImpacts() const;
  std::vector<HealthEvent> TakeHealth() const;
  // (the recording side: sim/damage.cpp, sim/projectile.cpp, sim/entities.cpp, sim/air.cpp, sim/transport.cpp)
  mutable std::vector<HitEvent> hitEvents;
  mutable std::vector<ImpactEvent> impactEvents;
  mutable std::vector<HealthEvent> healthEvents;
  // While collectHits: the shell or collision beam whose OnImpact runs now (null otherwise): what the damage its
  // script deals is credited to (weapon, projectile, direction)
  const Entity* impactShot = nullptr;
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
  // The engine's draws in the stages parallel order runs on the worker pool (docs/parallel-mode.md 4.1): in the
  // original's order each is the stream's next draw, exactly as before; in parallel order a keyed draw
  // (core/keyed_rng.h) of (the session's seed, the entity's creation handle, the beat, the purpose, the entity's n-th
  // keyed draw in the beat), whoever draws first. The draws of serial stages (creation, commands, recon, transports,
  // Lua) stay on the stream in both orders: their order is canonical.
  enum class RandPurpose : uint32_t { kAirCircle = 1, kAirTactics = 2, kAirCrash = 3, kHoverWobble = 4, kPush = 5, kZigZag = 6,
                                    kTargetPick = 7 };
  uint32_t ERand(Entity* e, RandPurpose p);
  double EFRand(Entity* e, RandPurpose p, float lo, float hi);  // FRand's formula on ERand
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
  std::vector<std::pair<Entity*, bool>> gridMoved;  // (the same, parallel order: the moved entities and whether widened)
  // A manipulator's place among those of equal precedence (0x63ace0 puts it behind them): stamped when it is
  // made, when its precedence is set, and when an animator is given an animation (it re-adds its bones).
  uint32_t manipSeq = 0;
  uint32_t NextManipSeq() { return ++manipSeq; }
  // an engine event (a manipulator) has just been signalled: its waiting threads go to the run order's end now
  // (ThreadScheduler::Signal, CTaskEvent::EventSetSignaled 0x406d90)
  void SignalEvent(const ScriptObject* o);
  // PropEvents (sim/sim_view.h): while set, every prop that goes is noted (its id, its place in the map file)
  bool collectProps = false;
  std::vector<std::pair<uint32_t, int>> propsGone;
  // The sync point's copies for an interface (sim/sim_snapshot.h): while set, the end of Start, of every Tick and of
  // every PausedBeat (after the Sync) brings snapshots() up to date, on the thread that runs the beat. snapshots()
  // is made with the Sim and never moves: a host may keep its address and read it from any thread at any time. The
  // copies only read the simulation (no script call, no Lua object made, no random draw): the game plays the same
  // with them on or off. snapPropsGone: the props destroyed since the last copy whose reclaim entry it holds.
  bool keepSnapshots = false;
  SyncSnapshots& snapshots() { return *snapshots_; }
  const SyncSnapshots& snapshots() const { return *snapshots_; }
  std::vector<std::pair<uint32_t, int>> snapPropsGone;  // (id, its cell in the copy)
  // The props whose place, size or state changed since the last copy (their handles): made, moved, rescaled, warped,
  // attached, destroy-queued. Every place that changes one of those of a prop tells it here (only while
  // keepSnapshots); the copy also looks at a window of the others every beat (sim/sim_snapshot.cpp).
  std::vector<uint32_t> snapPropsTouched;
  std::vector<uint32_t> snapUnitsMade;  // the units made since the last copy (their handles; AddUnitToLists)
  void NotePropTouched(const Entity* e) {
    if (keepSnapshots && e && e->kind == Entity::Kind::Prop) snapPropsTouched.push_back(e->handle);
  }
  // The weapons' target category sets (TargetRestrictDisallow / TargetRestrictOnlyAllow as parsed), each distinct
  // non-empty set once, in the order first seen: UnitWeapon::disallowSet / onlyAllowSet index it, and so do the
  // views (WeaponView in sim/sim_view.h, ui request 37). It only grows; an index keeps its set. The simulation
  // itself tests the weapon's own copies.
  std::vector<std::vector<uint64_t>> categorySets;
  int InternCategorySet(const std::vector<uint64_t>& set) {
    bool any = false;
    for (uint64_t w : set) any = any || w != 0;
    if (!any) return -1;
    auto it = categorySetIndex_.find(set);
    if (it != categorySetIndex_.end()) return it->second;
    const int i = static_cast<int>(categorySets.size());
    categorySets.push_back(set);
    categorySetIndex_.emplace(set, i);
    return i;
  }
  std::map<std::vector<uint64_t>, int> categorySetIndex_;
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
    if (e->kind != Entity::Kind::Unit && e->kind != Entity::Kind::Prop && e->kind != Entity::Kind::Projectile)
      others_[e->handle] = e;
    if (byHandle_.size() <= e->handle) byHandle_.resize(static_cast<size_t>(e->handle) + 1, nullptr);
    byHandle_[e->handle] = e;
    byId_[e->id] = e;
  }
  Entity* FindEntityById(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
  }
  // The game id an entity handle (EntityRef) had, also after the entity is gone (units, props and the like; not
  // projectiles); 0xf0000000 if unknown. (A command keeps its target's id encoded: EncodeToSSTITarget 0x5e27d0.)
  uint32_t IdOfRef(uint32_t handle) const {
    if (Entity* e = FindEntity(handle)) return e->id;
    auto it = goneIds_.find(handle);
    return it == goneIds_.end() ? 0xf0000000u : it->second;
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
  // By handle (EntityRef): byHandle_, which holds every registered entity at its handle's index.
  Entity* FindEntity(uint32_t id) const { return id < byHandle_.size() ? byHandle_[id] : nullptr; }
  void QueueDestroy(Entity* e);  // Entity:Destroy(): OnDestroy and removal happen at the end of the tick
  // While the destroy drain runs the units' ~Unit work (EntityDb::Purge order): an entity of the drain whose ~Unit has
  // not run yet, by handle (a weak pointer to it is still set then); null otherwise.
  Entity* PurgePending(uint32_t handle) const {
    if (!purging_ || !handle) return nullptr;
    for (size_t i = purgeAt_ + 1; i < purging_->size(); ++i)
      if ((*purging_)[i]->handle == handle) return (*purging_)[i];
    return nullptr;
  }
  void RequestRefreshUI(Entity* e);  // Entity:RequestRefreshUI / Entity::Kill (entity.h refreshUI; ui REQUESTS 47)
  // For what dies with a command object (a ferry beacon): the original deletes an emptied command only after
  // the beat's destroy queue has run, so the entity is queued then and goes at the end of the beat after.
  void QueueDestroyAfterBeat(Entity* e);
  // The entities that are no unit, prop or projectile (shields, beams, blips, the scripts' own), by handle. (An
  // entity's kind is set before it is registered and never changes.)
  const std::map<uint32_t, Entity*>& otherEntities() const { return others_; }
  // Live units of all armies, by entity id (kept up to date on creation and destruction).
  const std::vector<Unit*>& units() const { return units_; }
  // How many entities QueueDestroy has queued so far (it only counts up). Destroy is the only way an entity
  // leaves the world, and destroyQueued is never cleared, so while this number stands no entity has become
  // destroy-queued or gone (prop_motor.cpp MotorsTick).
  uint64_t DestroysQueued() const { return destroysQueued_; }
  // MOHO64_PROFILE: the order latency of docs/benchmarks.md B8. From the next beat on, each beat's end looks at these
  // units until every one has a path and moves (or 600 beats have passed), then logs how many beats and ms that took
  // (sim.cpp OrderLatency). A host calls it right after it gave them one order (src/app/main.cpp, the --orders verb
  // issuegroup). It only reads the units.
  void ProfileTrackOrder(const std::vector<uint32_t>& ids, const std::string& what);
  // The worker pool every parallel stage runs through (core/workers.h; MOHO64_THREADS, MOHO64_PAR_CHECK).
  WorkerPool& workers() { return *workers_; }
  // The entity pass's contact searches made ahead (MOHO64_PAR_MOTION, multicore step 3; sim/steering.h
  // SurfaceContactsAhead, core/speculate.h).
  Speculation<SurfaceContacts>& surfaceSpec() { return *surfaceSpec_; }
  // ... and the aim controllers' muzzles (MOHO64_PAR_AIM; sim/combat.h AimMuzzlesAhead)
  AimAhead& aimAhead() { return *aimAhead_; }
  // The navigators' turns played ahead (MOHO64_PAR_NAV; sim/landnav.cpp).
  Speculation<NavAhead>& navSpec() { return *navSpec_; }
  // The multicore switches' default (MOHO64_PAR_MOTION, AIM, NAV, PATH, RECON, BLIPS, INTELCOORDS): on in a game played with the
  // engine's fixes (a new game; `fixes`, decided by Start(SessionDescription)) whose worker pool has more than one
  // thread, off otherwise: the original's replays play as they always did, and at one thread the work made ahead
  // would only be made twice. A switch given in the environment (MOHO64_PAR_<NAME>=0 / 1 / verify / ...) wins. Each
  // is exact either way (the results are the serial ones, bit for bit), so this decides only the speed.
  bool ParDefault() const;
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
  // The dirty-entity list sim+0xa5c, oldest link first (the order of AdvanceBeat's AdvanceCoords walk, 0x74a239), and
  // the move of a unit to its newest end (Entity::SetHealth 0x679a3c).
  const std::vector<Unit*>& DirtyOrder();
  void MarkDirty(Unit* u);
  void ResumeCommandThread(Unit* u);
  // A queue clear's event re-links the unit's thread BEFORE its tasks are torn down (IAiCommandDispatchImpl::OnEvent
  // 0x599030: 0x599050..0x599083, then TaskInterruptSubtasks 0x408d70). The thread is then no longer suspended
  // (+0x18 = 0), so a resume of the same unit from inside that teardown (its walk's navigator event) re-links nothing:
  // ResumeCommandThread skips the unit held here while it is torn down.
  Unit* resumeHeld = nullptr;
  uint64_t CommandSeqHigh() const { return cmdSeqNext_; }
  // The stamp of a unit that takes a new parent (Unit::attachSeq). The original keeps a parent's children in a list
  // (entity+0x17c): Entity::AttachTo 0x679550 appends the child (0x6795d5..0x67961c), Entity::DetachFrom 0x6796f0
  // takes it out and moves the rest down (0x679725..0x67974f), so the list is in attach order; Entity::SyncInterface
  // 0x67a290 sends it to the user side (SSTIEntityVariableData+0x68). Here only UnitViews sorts by the stamp.
  uint64_t NextAttachSeq() { return ++attachSeqNext_; }
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
        for (const UnitGridEntry& e : grid_[static_cast<size_t>(cz) * gridW_ + cx]) {
          Unit* u = e.u;
          const Vec3& p = SeenPos(u);  // (another unit in parallel order's phase A: its beat-start place)
          if (p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1) f(u);
        }
  }
  // One unit in that grid, with its army and the end of its run: the index after the cell's consecutive entries of the
  // same army (a cell is filled in id order, and ids are army-major, so each army's units in a cell are one run). A unit
  // keeps its army for life, so the copy is the unit's own (MOHO64_VERIFY_UNITGRID checks it at every entry looked at).
  struct UnitGridEntry {
    Unit* u;
    const Army* army;
    uint32_t runEnd;
  };
  // ForUnitsInRect for the units whose army is neither `skip`'s own nor allied to it (Relation 2) nor none: a run of such
  // an army's entries is passed over at once, without touching its units (B3-16k 2026-10-31: BlipsInRange walked a
  // packed army's own units for 4% of the beat time). The units are given cell by cell, run by run: for a caller to whom
  // the order does not matter (BlipsInRange sorts what it keeps).
  template <class F>
  void ForNonAlliedUnitsInRect(const Army* skip, float x0, float z0, float x1, float z1, F&& f) {
    if (gridDirty_) RebuildUnitGrid();
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(gridW_ - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gridH_ - 1, static_cast<int>(z1) >> 4);
    if (x1 < 0 || z1 < 0) return;
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx) {
        const std::vector<UnitGridEntry>& c = grid_[static_cast<size_t>(cz) * gridW_ + cx];
        for (uint32_t i = 0, n = static_cast<uint32_t>(c.size()); i < n;) {
          const UnitGridEntry& e = c[i];
          if (kVerifyUnitGrid) [[unlikely]] VerifyUnitGridEntry(e);
          if (!e.army || ArmyRelation(skip, e.army) == 2) {
            i = e.runEnd;
            continue;
          }
          ++i;
          Unit* u = e.u;
          const Vec3& p = SeenPos(u);  // (another unit in parallel order's phase A: its beat-start place)
          if (p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1) f(u);
        }
      }
  }
  static int ArmyRelation(const Army* a, const Army* b);  // (combat.cpp Relation)
  static inline const bool kVerifyUnitGrid = std::getenv("MOHO64_VERIFY_UNITGRID") != nullptr;
  static void VerifyUnitGridEntry(const UnitGridEntry& e);
  static void VerifyUnitGridList(bool same);  // BlipsInRange's two lists agreed or not (counts at exit)
  void MarkUnitsMoved() { gridDirty_ = true; }
  bool UnitGridDue() const { return gridDirty_; }  // (the next ForUnitsInRect / EnsureUnitGrid rebuilds it)
  // The unit grid behind ForUnitsInRect made now if it is due (the searches' pre-pass, sim/hpath.h).
  void EnsureUnitGrid() {
    if (gridDirty_) RebuildUnitGrid();
  }
  // The entity grid behind GetUnitsInRect / GetReclaimablesInRect (sim/entity_grid.h).
  EntityGrid& entityGrid();
  const EntityGrid* EntityGridIfMade() const { return egrid_.get(); }  // (the snapshots: never makes it)
  bool anyAttached = false;  // some unit rides a transport (sim/transport.cpp)
  // FAF's exe global 0x1292b54 (SetNavigatorPersonalPosMaxDistance): the land path navigator's look-ahead limit in
  // cells (TryAdvanceTargetPoint 0x5af8d5); 50 until a script sets it (FAF's simInit: 9999). One per sim here.
  float navPersonalPosMaxDistance = 50.0f;
  uint32_t cargoFollowTick = 0;  // the beat whose cargo has taken its attach bones' transforms (AttachedUnitsTick)
  // Call obj:method(args...) for the nargs values on L's stack; logs script errors.
  // (Every function taking a lua_State works on the caller's state: it may be a thread.)
  bool CallMethod(lua_State* L, ScriptObject* obj, const char* method, int nargs);
  // Parallel order's deferred script calls (docs/parallel-mode.md 3.3 / 3.6, step 4.1). While the entity pass runs
  // its phases A (motion), C (pose and aim) and D (economy), the engine's callbacks of those phases (group A of 3.6:
  // motion events, layer and terrain changes, fuel, a prop's OnCollision, a crash's OnImpact, tracking, footfalls,
  // the economy's events) are not called at once: DeferScriptCall keeps the object, the method (a string that lives
  // as long as the program) and the arguments, and the phase's commit runs them in the order of their keys (the turn's
  // place in the pass, then the call's place in the turn). In the original's order nothing is ever deferred: each site
  // calls as before. Outside those phases (shells, commands, Lua) every call is immediate too.
  struct ScriptArg {
    enum Kind : uint8_t { kString, kNumber, kObject, kTerrainType } kind = kNumber;
    double number = 0;  // (kNumber: a float or int as the site pushed it; kTerrainType: the terrain type's code)
    ScriptObject* object = nullptr;
    std::string string;
    static ScriptArg Str(const char* s) { ScriptArg a; a.kind = kString; a.string = s; return a; }
    static ScriptArg Num(double v) { ScriptArg a; a.kind = kNumber; a.number = v; return a; }
    static ScriptArg Obj(ScriptObject* o) { ScriptArg a; a.kind = kObject; a.object = o; return a; }
    static ScriptArg Terrain(int code) { ScriptArg a; a.kind = kTerrainType; a.number = code; return a; }
  };
  bool DeferringScripts() const { return deferring_; }
  void DeferScriptCall(ScriptObject* obj, const char* method, std::vector<ScriptArg> args);
  // ... and an engine action that runs scripts (a kill), in the same order
  void DeferAction(std::function<void()> action);
  // (DeferScriptCall: the calls of the phase running now, in the order made; the turn's key is deferKey_)
  struct DeferredCall {
    uint64_t key = 0;  // (the turn's place in the pass << 32) | the call's place in the turn
    ScriptObject* object = nullptr;
    const char* method = nullptr;
    std::vector<ScriptArg> args;
    std::function<void()> action;  // (DeferAction: run instead of a script call)
  };
  std::vector<DeferredCall> deferred_;
  bool deferring_ = false;
  // parallel order's phase D (step 4.5): the units whose economy step added to their army this beat (Unit::econAdd),
  // and the sum of those additions into the armies, unit by unit in creation order (rule 7: a fixed order of float sums)
  std::vector<Unit*> econAdders_;
  void SumUnitEconomy();
  uint32_t deferKey_ = 0, deferSeq_ = 0;
  void DeferTurn(uint32_t key) {
    deferKey_ = key;
    deferSeq_ = 0;
  }
  // (step 4.6) a phase's turns on the worker pool: each worker's deferred calls go to a list of its own, with the key
  // of the turn it runs (DeferSink, set per thread by the driver); after the phase the lists are merged into deferred_
  // and the commit sorts them by key as before, so the order is the turns' and not the workers'.
  struct DeferSink {
    std::vector<DeferredCall> calls;
    uint32_t key = 0, seq = 0;
  };
  std::vector<DeferSink> deferSinks_;
  void MergeDeferSinks();
  // A stage's turns in parallel order (step 5.2): turn(i) for i in [0, n), with deferring on and the turn's key i; on
  // the worker pool when it has more than one thread (or its order check is on) and the write trap is off, each
  // worker's deferred calls in its own list, merged after; else one after another. The caller runs the commit
  // (RunDeferredScriptCalls). The path grids' pending terrain changes are taken first (a grid's first reader would).
  void RunTurns(const char* stage, size_t n, const std::function<void(size_t)>& turn);
  void RunDeferredScriptCalls();
  // script calls still made at once inside a deferring phase (by method; logged at the end): what step 4.6 must
  // still defer before the phases go to the worker pool
  std::map<std::string, uint64_t> immediateInPhase_;
  // The army the session looks through (sim+0xa88): 0-based, -1 = none (an observer, a replay). Hosts read it;
  // they change it with RequestFocusArmy (applied at the next Sync), never by writing it.
  int focusArmy = -1;
  bool isReplay = true;  // false for a session launched from a game configuration (Start(SessionDescription))
  // Deliberate departures from the original that repair something broken in it or in the game's scripts
  // (docs/deviations.md lists each one). On in a new game, off while a replay is played back, so that a
  // recording of the original still plays as the original did; on again for the replay of a game this engine
  // played with them (its mark: replay.h ScenarioMarksFixes). MOHO64_FIXES=0 / 1 overrides. Those made in the
  // game's Lua are the hook files of the repository's data/fixes/hook, given to the sim state only with `fixes`
  // (and to the rules state, loaded again, in a game with a mod a blueprint fix is for: RulesFixesWanted).
  bool fixes = false;
  // The beat's order (docs/parallel-mode.md 5), decided by Start(SessionDescription) after `fixes`: the session's
  // simOrder, else the scenario's Options.RemasterSimOrder (missing: the original's); MOHO64_SIM_ORDER=original |
  // parallel overrides it. Parallel order needs the fixes (a session asking for it without them does not start).
  // orderVersion: the parallel order's version the game was made with (Options.RemasterSimOrderVersion; 0 for the
  // original's order); kSimOrderVersion is this engine's. Raise it whenever a change makes a parallel-order game
  // play differently (docs/parallel-mode.md 7, risk 4); the original's order never reads it.
  //   1: the first parallel order (docs/parallel-mode.md 8, steps 1-3 and 4.4-4.6: canonical grid buckets, keyed
  //      draws, the entity pass's phases, economy and snapshots on the pool).
  //   2: steps 5.1-5.5 (armies' stages without the interleave; steering, navigators and weapons' acquire as turns
  //      on the pool with their effects committed in turn order; grid spans): a version-1 game plays differently.
  SimOrder order = SimOrder::kOriginal;
  int orderVersion = 0;
  static constexpr int kSimOrderVersion = 2;
  bool ParallelOrder() const { return order == SimOrder::kParallel; }
  // The FAF game version whose rules the session plays by (replay.h kGameVersion3839 / 3840; engine-ref/specs/
  // faf3840.md), decided by Start(SessionDescription) from the header's version line: a replay plays by its own
  // version's rules, a new game by the one its header was made with (replay.h kNewGameVersion). A version this
  // engine does not know plays by the nearest one it knows. MOHO64_GAME_VERSION=3839 | 3840 overrides it.
  int gameVersion = 3839;
  bool Rules3840() const { return gameVersion >= 3840; }
  // The session's mod list as the lobby sent it (a replay's serialized mods): becomes
  // __active_mods in the rules and sim states. Empty: built from the mod uids instead.
  std::string sessionMods;
  std::string recordSuffix;  // MOHO64_RECORD's file is <file><suffix> (moho64 --together: ".2" for the second copy)
  // The session's scenario info as it was launched (serialized Lua; what the interface's SessionGetScenarioInfo
  // returns). The scripts' own ScenarioInfo grows engine objects during the game and is not for handing out.
  std::string sessionScenario;
  std::string mapFile;  // the session's map (the replay header's / the launch's map path), as Start loaded it
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
  // From the entity stage to step 13 (Entity::AdvanceCoords): a unit that has had its turn of the motion loop
  // holds its new transform as pending only; its committed one is lastPosition / lastOrientation.
  bool coordsPending = false;
  bool endGame = false;               // sim+0x8dc (EndGame message)
  bool frozen = false;                // sim+0x8dd (= endGame after the next Sync): beats do nothing
  bool Paused() const { return frozen || (pauseOwner != -1 && !singleStep); }
  bool OkayToMessWith(const Army* a) const;  // 0x747320
  // Sim::CheatsEnabled 0x747180: every cheat test of the sim goes through it (the stream's CreateUnit, CreateProp,
  // WarpEntity, ExecuteLuaInSim, a DestroySelf order, a debug command that is not safe; OkayToMessWith when the
  // source does not command the army; the scripts' CheatsEnabled() and OkayToMessWithArmy). With cheats off it
  // warns "%s is trying to cheat!" (0x7471ea..0x74721f; with cheats on "%s is cheating!" only under the convar
  // sim_ReportCheats, 0x10a63ed, off by default: not built), the name being the current command source's; then
  // that source (when there is one, 0x747282) joins the cheaters (sim+0x8e8, each once), whom the next Sync hands
  // to the interface as Sync.Cheaters (SyncStep). Returns the flag (sim+0x8e6, the launch's CheatsEnabled).
  bool CheatsEnabled() const;
  // A DebugCommand message (stream type 20; Sim::ExecuteDebugCommand 0x749da0): the interface's "DoSimCommand
  // <text>" with its cursor's world point, its focus army (an index; out of range = none) and its selection.
  void ExecuteDebugCommand(lua_State* L, const std::string& text, Vec3 mouse, int army,
                           const std::vector<Unit*>& units);
  // Sim::Printf 0x7462a0: one line for the interface's console (sim+0xa18, as print's lines; logged when no host
  // takes them).
  void Print(const std::string& line);
  // Stream-issued commands by their stream id (source << 24 | n); Lua-issued ones are in commandsById.
  std::map<uint32_t, std::weak_ptr<UnitCommand>> streamCommands;
  // Commands whose last unit let them go since the last Sync. The original deletes such a command in the Sync
  // (CCommandDb::PublishSyncData 0x6e0f50 -> CUnitCommand::RefreshPublishedCommandEvent 0x6e8dc0: dirty, +0x141, which
  // RemoveUnit 0x6e8c20 sets, and no units, +0xf8 == +0xfc -> the deleting dtor, vt+8), before the id pool's turn
  // (0x6e0fff); until then the command database still finds it by its id. Seen: the interface's coordinated attack
  // (MP1 3915) takes the hovered order off every unit and then names it as coordWith in the same message batch.
  // Held here until UpdateIdPools.
  std::vector<std::shared_ptr<UnitCommand>> commandsEmptied;

  static Sim* From(lua_State* L);
  bool PushImport(lua_State* L, const std::string& module);  // import(module) -> table, or false (logged)

  // Movement (M3): pathfinding, path searches waiting their turn, issued commands.
  Navigation& navigation();
  bool HasNavigation() const { return nav_ != nullptr; }  // made already (navigation() reads the terrain types from Lua)
  const Navigation* NavigationIfMade() const { return nav_.get(); }  // (the snapshots: never makes it)
  std::deque<Unit*> pathQueue;
  uint64_t walkSeqNext = 0;  // Unit::walkSeq: the order task walks were made in (their listeners' link order)
  std::vector<ResourceDeposit> deposits;
  std::map<uint32_t, std::weak_ptr<UnitCommand>> commandsById;  // by UnitCommand::serial (the scripts' handles)
  uint32_t nextCommandId = 1;  // the next serial
  std::map<uint32_t, std::weak_ptr<UnitCommand>> engineCommands;  // by engine id (0x80000000 | index), for the stream
  std::shared_ptr<CommandIdPool> commandIds;  // (made by the Sim's ctor, as CCommandDb's pool)
  // Each system's own state, made by that system when it first needs it (multicore step 1: these were
  // process-wide variables in the systems' files, so two sims in one process shared them; docs/multicore.md)
  std::shared_ptr<struct IntelState> intelState;  // sim/intel.cpp: the armies' grids, the sources, the blips
  std::shared_ptr<struct ThreatState> threatState;  // sim/ai_threat.cpp: the armies' threat maps
  std::shared_ptr<struct BrainStates> brainState;   // sim/ai_brain.cpp: the brains' own state, by army
  std::shared_ptr<struct MotorState> motorState;     // sim/prop_motor.cpp: the props' fall / sink motors
  std::shared_ptr<struct AnimState> animState;       // sim/anim.cpp: the animators and the .sca files they read
  std::shared_ptr<struct EffectsState> effectsState;  // sim/effects.cpp: effect objects, the host's effect,
                                                      // sound and shake lists
  std::shared_ptr<struct LandNavState> landNavState;  // sim/landnav.cpp: the path tables, the navigators' profile
  std::shared_ptr<struct FormationState> formationState;  // sim/formation.cpp: the formations
  std::shared_ptr<struct AirState> airState;          // sim/air.cpp: the landing-spot reservations
  // sim/steering.cpp: the steering stage runs (UnitMotion::colLateTick); the army whose navigators run, 1-based,
  // 0 outside that stage (SteeringNavStageArmy)
  bool inSteeringStage = false;
  int navStageArmy = 0;
  // src/ai/map_analysis.cpp: our own AI's map analysis (its MapState), made only in a session with our AI or for
  // MOHO64_AI_MAPDUMP; the game itself never reads it
  std::shared_ptr<void> aiMapState;
  // src/ai/unit_index.cpp: its unit ratings and its unit / threat index, likewise only with our AI (the ratings also
  // for MOHO64_AI_RATINGS); the game itself never reads them
  std::shared_ptr<void> aiRatingsState, aiIndexState;
  std::shared_ptr<struct PhaseTimer> phaseTimer;  // sim/sim.cpp: MOHO64_PROFILE's phase times (measuring only)
  std::shared_ptr<struct OrderLatency> orderLatency;  // sim/sim.cpp: MOHO64_PROFILE's order latency (ProfileTrackOrder)
  // Counts for the debug and stats lines (MOHO64_DEBUG_COMBAT, MOHO64_STATS); nothing in the simulation reads them
  uint64_t firesSeen = 0, projectilesMade = 0, impactsSeen = 0;
  int unitsCreated = 0;
  mutable std::shared_ptr<void> caches[kSimCacheSlots];  // sim/sim_cache.h: each SimCache site's container
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
  std::unique_ptr<SyncSnapshots> snapshots_;  // (snapshots(); made in the ctor)
  SimBlueprints bps_;
  std::vector<std::unique_ptr<Army>> armies_;
  std::mt19937 rng_;
  uint32_t sessionSeed_ = 0;  // what rng_ was seeded with (the replay header's / the lobby's seed)
  float gauss_ = 0;
  bool hasGauss_ = false;
  uint32_t tick_ = 0;
  std::vector<std::unique_ptr<ScriptObject>> owned_;
  std::map<uint32_t, Entity*> others_;  // (otherEntities(): the fingerprint walks it)
  // The same by handle as an array (handles are given out one after another from 1 and never again): FindEntity
  // without the map's search (265 M calls in B1, ~280 ns each in the map). A removed entity's slot is null.
  std::vector<Entity*> byHandle_;
  std::vector<Unit*> units_;
  std::vector<std::string> pendingSync_;
  std::vector<std::string> prints_;  // print's lines until TakePrints (the original's sim+0xa18)
  mutable std::vector<int> cheaters_;  // sim+0x8e8: the sources that met CheatsEnabled since the last Sync
  void KeepPrint(const std::vector<std::string>& args);  // one print(...) call's lines onto prints_
  void SyncStep(bool keep);  // the script part of Sim::Sync; keep: its bytes go to pendingSync_
  void CollectorStep();  // the Lua collector's work at the beat's end (AdvanceBeat step 19; sim.cpp)
  std::atomic<int> requestedFocus_{-1};  // the pending focus army (SimDriver+0xb0); Start sets it to focusArmy
  void ApplyFocusArmy();  // the start of Sim::Sync: a pending focus army takes effect
  void SetupSync();  // the end of Start: the setup's own Sync, the original's "beat 0" (always kept)
  std::vector<Unit*> cmdOrder_;
  uint64_t cmdSeqNext_ = 0;
  bool cmdOrderDirty_ = true;
  std::vector<Unit*> motionOrder_;
  uint64_t entSeqNext_ = 0;
  uint64_t attachSeqNext_ = 0;
  bool motionOrderDirty_ = true;
  std::vector<Unit*> armyOrder_;
  bool armyOrderDirty_ = true;
  std::vector<Unit*> dirtyOrder_;
  uint64_t dirtySeqNext_ = 0;
  bool dirtyOrderDirty_ = true;
  std::vector<std::vector<UnitGridEntry>> grid_;
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
  std::unordered_map<uint32_t, Entity*> byId_;  // (only looked up, set and erased, never walked: a hash)
  std::unordered_map<uint32_t, uint32_t> goneIds_;  // handle -> id of a removed entity (IdOfRef)
  uint32_t nextHandle_ = 1;
  void AddUnitToLists(Unit* u);
  void RemoveUnitFromLists(Unit* u);
  uint32_t propSerial_ = 0;
  std::vector<Entity*> destroyQueue_;
  const std::vector<Entity*>* purging_ = nullptr;  // (PurgePending: the drain's list and the entity at work)
  size_t purgeAt_ = 0;
  uint64_t destroysQueued_ = 0;  // (DestroysQueued)
  void ProcessDestroyQueue();
  std::vector<uint32_t> refreshPending_, refreshSynced_;  // entity handles (EntityRef) with refreshUI / refreshUISynced set
  std::vector<uint32_t> lateDestroy_;  // entity references (QueueDestroyAfterBeat)
  void QueueLateDestroys();

  // Push the script class for a blueprint: bp.ScriptModule/ScriptClass, else
  // <dir>/<name>_script.lua's TypeClass, else the default module's class.
  void PushScriptClass(lua_State* L, const BlueprintInfo& bp, const char* defModule, const char* defClass);
  void InitializeArmor(lua_State* L, Unit* u);
  // The entity's skeleton: Display.MeshBlueprint -> that mesh blueprint's first LOD MeshName.
  void AttachSkeleton(lua_State* L, Entity* e);
  std::unique_ptr<SkeletonCache> skeletons_;
  std::unique_ptr<WorkerPool> workers_;  // MOHO64_THREADS (core/workers.h): made once per Sim
  std::unique_ptr<Speculation<SurfaceContacts>> surfaceSpec_;  // MOHO64_PAR_MOTION
  std::unique_ptr<AimAhead> aimAhead_;                         // MOHO64_PAR_AIM
  std::unique_ptr<Speculation<NavAhead>> navSpec_;  // MOHO64_PAR_NAV
  bool armorLoaded_ = false;
  std::map<std::string, std::map<std::string, float>> armorTypes_;  // armour type -> damage type -> multiplier

  bool CallGlobal(const char* fn, int nargs);  // args pushed; logs errors
  void SetModsGlobal(ScriptState& st);
  // The rules state (LoadRules), with the engine's script fixes joined when withFixes (Start(SessionDescription)
  // loads them again so when RulesFixesWanted: a fix in the blueprints is for one of the game's mods).
  bool LoadRulesState(bool withFixes);
  bool CreateArmies(const ReplayHeader& replay);
  void CreateMapProps();
};

void RegisterSimBindings(lua_State* L);

}  // namespace moho
