// Unit commands and their Lua bindings (see commands.h).
#include "sim/air.h"
#include "sim/commands.h"
#include "sim/entity_grid.h"
#include "sim/formation.h"
#include "sim/landnav.h"
#include "sim/combat.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

#include "core/backtrace.h"
#include "core/crash.h"
#include "core/dmath.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/collision.h"
#include "sim/economy.h"
#include "sim/enum_arg.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/replay.h"
#include "sim/sim.h"
#include "sim/steering.h"
#include "sim/terrain.h"
#include "sim/transport.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

namespace combat {
bool IsEnemy(const Army* a, const Army* b);
int CallMethodBool(Sim& sim, lua_State* L, ScriptObject* obj, const char* method, int nargs);
}
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

const char* const kCommandNames[] = {
    "None", "Stop", "Move", "Dive", "FormMove", "BuildSiloTactical", "BuildSiloNuke", "BuildFactory",
    "BuildMobile", "BuildAssist", "Attack", "FormAttack", "Nuke", "Tactical", "Teleport", "Guard",
    "Patrol", "Ferry", "FormPatrol", "Reclaim", "Repair", "Capture", "TransportLoadUnits",
    "TransportReverseLoadUnits", "TransportUnloadUnits", "TransportUnloadSpecificUnits",
    "DetachFromTransport", "Upgrade", "Script", "AssistCommander", "KillSelf", "DestroySelf",
    "Sacrifice", "Pause", "OverCharge", "AggressiveMove", "FormAggressiveMove", "AssistMove",
    "SpecialAction", "Dock"};

bool MoveLike(CommandType t) {
  switch (t) {
    case CommandType::Move:
    case CommandType::FormMove:
    case CommandType::AggressiveMove:
    case CommandType::FormAggressiveMove:
    case CommandType::Patrol:
    case CommandType::FormPatrol:
    case CommandType::AssistMove:
      return true;
    default:
      return false;
  }
}
// Commands that go to an entity target and end there (until M4 carries out the action itself).
bool ApproachLike(CommandType t) {
  switch (t) {
    case CommandType::Attack:
    case CommandType::FormAttack:
    case CommandType::Reclaim:
    case CommandType::Repair:
    case CommandType::Capture:
    case CommandType::Guard:
    case CommandType::BuildAssist:
    case CommandType::AssistCommander:
      return true;
    default:
      return false;
  }
}

// Per-unit progress of the head command (Unit::headState).
enum HeadState : int { kNotStarted = 0, kWaitingPath = 1, kRunning = 2 };

// The type the head command had when it was dispatched: the running task's kind. DispatchTask 0x608ef0 picks the task
// by cmd+0x98 at that moment, and the stream's SetCommandType (Sim::SetCommandType 0x749800) writes only cmd+0x98 and
// the publish flag +0x141 (read by RefreshPublishedCommandEvent 0x6e8dc0 alone): nothing is dispatched again. A Move
// turned into a Patrol keeps its CUnitMoveTask: the task's command listener 0x618c40 takes a dragged target
// (CUnitCommand::SetTarget 0x6e8820 -> 0x6e94a0), its ctor suspended the thread until the navigator's event (0x6186d6,
// the listener 0x618bb0 re-links it) and its dtor 0x618700 aborts the move. Only what the dispatcher does once the
// task has ended reads the type as it is then (TaskTick 0x598ebb / 0x598f47: ArrivalStep). Seen: MP1 6026, army 5's
// player turns the three Moves of its air scout 0x50008a into Patrols and drags the first (the one under way) to
// (551.38, 808.94) at 6028: the original's target at 6029 is (551, 809); ours took no listener for a Patrol without a
// patrol task and kept (555, 827).
CommandType HeadTaskType(const Unit* u) {
  return u->headState != kNotStarted ? static_cast<CommandType>(u->headType) : u->commands.front()->type;
}
// Path search work per tick (cells walked or expanded): searches start, in the order they were
// asked for, while the tick's work is below this. Fitted to the probe: 17 moves issued in one
// tick start in two batches (the second after a long search).
constexpr uint64_t kPathWorkPerTick = 1000;

bool IsAlive(const Unit* u) { return u && !u->dead && !u->destroyQueued; }

Vec3 TargetPos(Sim& sim, const UnitCommand& c, Unit* u = nullptr) {
  if (u && c.form) {  // 0x6e8a30: the unit's formation cell when the command holds more than one unit
    Vec3 g;
    if (FormationGoal(sim, const_cast<UnitCommand&>(c), u, &g)) return g;
  }
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) return e->position;
  return c.pos;
}

void SetMoving(Unit* u, bool on) {
  if (on) u->unitStates.Insert(US("Moving"));
  else u->unitStates.Erase(US("Moving"));
}

// A unit drops a command: when no unit holds it any more the command object goes, and with it
// the ferry beacon it created (CUnitCommand::DestroyInternal 0x6e8500).
void ReleaseCommandImpl(Unit* u, UnitCommand& c) {
  FormationRemoveUnit(c, u);
  c.units.erase(u);
  if (!c.units.empty()) return;
  // (the command database keeps it until the Sync: Sim::commandsEmptied)
  if (Sim* s = Sim::From(u->luaState()))
    if (auto keep = c.weak_from_this().lock()) s->commandsEmptied.push_back(std::move(keep));
  if (!c.beaconRef) return;
  Sim& sim = *Sim::From(u->luaState());
  if (Entity* b = sim.FindEntity(c.beaconRef)) {
    static const bool dbg = getenv("MOHO64_DEBUG_TASKS") != nullptr;
    if (dbg)
      Logf(LogLevel::Info, "beacon %u of command %u (type %d) goes at tick %u: unit %u let the command go (in a beat: %d)", b->id,
           c.id, static_cast<int>(c.type), sim.tick(), u->id, sim.RecorderTick() != 0xffffffffu ? 1 : 0);
    // The beacon dies in the command object's destructor (0x6e8500), and the original deletes an emptied
    // command a beat's end later than its last holder lets go (seen twice in the Transports replay: a player's
    // order clears the transport's queue after tick T, the beacon is still listed after tick T + 1 and gone
    // after T + 2). (The deletion itself is the Sync's, RefreshPublishedCommandEvent 0x6e8dc0: Sim::commandsEmptied;
    // the beacon's own end there was not read: this is the observed timing.)
    if (!b->destroyQueued) sim.QueueDestroyAfterBeat(b);
  }
  c.beaconRef = 0;
}

// CUnitMoveTask's dtor 0x618700: the o-grid rect its ctor reserved (+0x92) is freed (FreeOgridRect 0x6ab760)
void EndMoveTaskRect(Unit* u) {
  if (!u->moveTaskReserved) return;
  u->moveTaskReserved = false;
  GroundFreeRect(*Sim::From(u->luaState()), u);
}

// Remove the head command of u (it is done for this unit).
void PopHead(Unit* u) {
  EndMoveTaskRect(u);
  if (u->engageId) ClearEngagement(*Sim::From(u->luaState()), u);
  if (u->task) {
    EndBuildTask(*Sim::From(u->luaState()), u, *u->task, true);
    u->task = nullptr;
  }
  if (u->commands.empty()) return;
  auto head = u->commands.front();
  u->commands.pop_front();
  ReleaseCommandImpl(u, *head);
  u->headState = kNotStarted;
}

bool NextIsMove(const Unit* u) { return u->commands.size() > 1 && MoveLike(u->commands[1]->type); }

// IAiCommandDispatchImpl::TaskTick 0x598e80, once DispatchTask made no task for the head order (0x598eea..0x598f67):
// the order counts as carried out. A count above 1 goes down by one and the order is dispatched again (0x598eee..
// 0x598f09, on to 0x598f6d); with the repeat switch (unit+0x2a2) a spent BuildFactory order gets its count back
// (+0xe0 -> +0xe4), goes to the back of the queue (0x6edfc0) and the dispatcher waits a beat (0x598f0b..0x598f3b);
// otherwise it is removed (0x6ede70 at 0x598f67) and the next one dispatched. The count is the command's own, shared
// with every unit that holds it. Returns true when the dispatcher goes on in this beat. (TaskTick's own cases for
// Attack, Patrol and FormPatrol at 0x598f47 are not here: only BuildFactory and Upgrade orders come this way.)
bool OrderTakenWithoutTask(Unit* u) {
  std::shared_ptr<UnitCommand> head = u->commands.front();
  if (head->count > 1) {
    --head->count;
    return true;
  }
  if (u->repeatQueue && head->type == CommandType::BuildFactory) {
    head->count = head->maxCount;
    if (u->commands.size() > 1) {
      PopHead(u);
      u->commands.push_back(head);
      head->units.insert(u);
    }
    u->headState = kNotStarted;
    return false;
  }
  PopHead(u);
  return true;
}

// a unit blueprint's General.<key> string ("" when it has none)
std::string GeneralString(Sim& sim, const BlueprintInfo& bp, const char* key) {
  lua_State* L = sim.L();
  const int top = lua_gettop(L);
  std::string r;
  sim.blueprints().PushTable(L, bp);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "General");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, key);
      lua_rawget(L, -2);
      if (lua_type(L, -1) == LUA_TSTRING) r = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return r;
}

bool SameIdNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// DispatchTask's Upgrade case (0x6098bd): no blueprint, no task. When the order's blueprint names a General.SeedUnit
// (string at bp+0x1d8, its size +0x1ec), that must be the unit's own blueprint (bp+8); otherwise the unit's
// General.UpgradesTo (bp+0x184) must be the order's blueprint, or the order's General.UpgradesFrom (bp+0x1a0) the
// unit's. The compares ignore case (0xaa549e). Only then is the upgrade task made (0x5f8b80); else no task.
// (General sits at RUnitBlueprint+0x17c, 0x5258cf; UpgradesTo +8, UpgradesFrom +0x24, SeedUnit +0x5c, 0x520660..)
bool UpgradeOrderFits(Sim& sim, const Unit* u, const UnitCommand& c) {
  const BlueprintInfo* to = sim.blueprints().Find(c.blueprintId);
  if (!to || !u->blueprint) return false;
  const std::string seed = GeneralString(sim, *to, "SeedUnit");
  if (!seed.empty()) return SameIdNoCase(seed, u->blueprint->id);
  if (SameIdNoCase(GeneralString(sim, *u->blueprint, "UpgradesTo"), to->id)) return true;
  return SameIdNoCase(GeneralString(sim, *to, "UpgradesFrom"), u->blueprint->id);
}

// ProcessUnitCommand 0x6ef9e0, the Upgrade case (0x6f00a9..0x6f02c1; the type switch 0x6efdc2), its end
// (0x6f010c..0x6f02c1, read): when the blueprint tests pass (SeedUnit, UpgradesTo / UpgradesFrom, UpgradesFromBase;
// not modelled here, DispatchTask makes its own: UpgradeOrderFits), an Upgrade is refused when the LAST order of the
// unit's queue (GetLastCommand 0x6edc50) is an Upgrade (+0x98) to the same blueprint (cmd+0x60, ids stricmp'd
// 0xaa549e), whether the new one clears the queue or not (the clear comes after, 0x6f1405). Seen: MP3 7769, army 1's
// T1 extractor 0x10006f, upgrading since 5984, gets the same Upgrade from the command stream again (clear 0): the
// original's queue stays at one order; ours queued the second and showed two.
bool RefusesUpgradeAgain(Sim& sim, const Unit* u, const std::string& bpId) {
  if (u->commands.empty()) return false;
  const UnitCommand& last = *u->commands.back();
  if (last.type != CommandType::Upgrade) return false;
  const BlueprintInfo* a = sim.blueprints().Find(last.blueprintId);
  return a && a == sim.blueprints().Find(bpId);
}

}  // namespace

void EndMoveTaskReservation(Unit* u) { EndMoveTaskRect(u); }

uint32_t AssignEngineCommandId(Sim& sim, const std::shared_ptr<UnitCommand>& c) {
  // MOHO64_DEBUG_CMDID=1: every engine id taken ("cmdid alloc <tick> <index> slot <s> from <frames>") and given back
  // ("cmdid free <tick> <index> slot <s>"), to set beside a probe of CCommandDb::AddIssueData 0x6e0db0 /
  // RemoveCommand 0x6e0ec0. The slot is the pool's ring end: it tells a stream message's work (before the beat, after
  // the last Update) from the beat's own, which both carry the previous tick.
  static const bool dbg = getenv("MOHO64_DEBUG_CMDID") != nullptr;
  std::shared_ptr<CommandIdPool> pool = sim.commandIds;
  const uint32_t index = pool->Allocate();
  c->id = 0x80000000u | index;
  c->serial = sim.nextCommandId++;
  sim.commandsById[c->serial] = c;
  sim.engineCommands[c->id] = c;
  c->idLease = std::shared_ptr<void>(nullptr, [pool, index](void*) {
    if (dbg) Logf(LogLevel::Info, "cmdid free %lld %u slot %d", static_cast<long long>(g_crashTick), index, pool->tail);
    pool->Release(index);
  });
  if (dbg) {
    void* fr[8];
    const int n = CaptureFrames(fr, 8);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
      s += b;
    }
    Logf(LogLevel::Info, "cmdid alloc %u %u slot %d from%s", sim.tick(), index, pool->tail, s.c_str());
  }
  return c->id;
}

// CUnitMoveTask::OnEvent 0x618c40 (read): a move task listens to its command, and on any event of it (CUnitCommand::
// SetTarget 0x6e8820 broadcasts event 0 through 0x6e94a0 after storing the new target) it hands its navigator the
// command's position again: the unit's footprint cell at the target (CUnitCommand::GetPosition 0x6e8a30), as an
// SNavGoal, through the navigator's SetGoal (vt+8). The PrepareMove variant (task+0x95, which the ctor set for a
// factory's rally Move it put through PrepareMove; ours: moveTaskReserved) takes that cell's centre (COORDS_ToWorldPos
// 0x50b1a0), not the rounded target the ctor took, puts it through PrepareMove (spacing 0, 0x618d20), hands the
// navigator the cell found and moves its o-grid reservation there (+0x92: FreeOgridRect 0x6ab760, ReserveOgridRect
// 0x6ab6f0). Only the head's task exists, so a queued command just keeps its new target for later.
// Seen: MP3 758, a player drags the waypoint of army 0's commander's running Move: the original's navigator plans
// again from the commander's place; ours drove on to the old path's waypoint. MP1 804, the rally Move of army 5's
// 0x500008 is dragged to (331.77, 854.30): cell (331, 854), taken; the original's goal is (333, 856), the ring two
// footprints out (ours, from the rounded target, looked around (332, 854)).
// The listeners are told in the command's listener order; ours goes by unit id (inferred: the same for one unit).
// A command with a formation: CUnitCommand::SetTarget 0x6e8820 has moved the formation's centre to the new target
// first (0x6e887d..0x6e88bf, the stream handler: FormationSetCentre), and each unit's task takes its cell in the
// moved formation: the move task's listener asks GetPosition 0x6e8a30 (the formation cell while the command holds
// more units than one), CUnitFormAndMoveTask's 0x6196f0 the formation's vt+0x1c (its cell, no count test), and
// either gives the navigator that one cell (vt+0x08). Seen: MP1 beat 4149, army 2's player drags the formation Move
// 0x02000033 of nine aircraft to (160.80, 198.81): the original's 0x200043 turns at 4150 for (163, 195), its cell
// by the new centre; ours left the formation where it was and kept (150, 196).
void RetargetMoveTasks(Sim& sim, UnitCommand& c) {
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* u : holders) {
    if (!IsAlive(u) || u->commands.empty() || u->commands.front().get() != &c) continue;
    if (u->headState != kRunning || u->task || !u->motion.bp || !u->motion.bp->mobile()) continue;
    // (the task the head was dispatched as, not the command's type now: HeadTaskType)
    const CommandType task = HeadTaskType(u);
    if (task != CommandType::Move && !(task == CommandType::FormMove && c.form)) continue;
    Vec3 tp = TargetPos(sim, c, u);
    if (task == CommandType::FormMove && !FormationGoalOfForm(c, u, &tp)) continue;
    const NamedFootprint& fp = UnitFootprint(u);
    int cx = static_cast<int>(std::nearbyint(tp.x - fp.sizeX * 0.5f));
    int cz = static_cast<int>(std::nearbyint(tp.z - fp.sizeZ * 0.5f));
    if (u->moveTaskReserved) {
      Vec3 p{cx + fp.sizeX * 0.5f, tp.y, cz + fp.sizeZ * 0.5f};
      const float none[4] = {0, 0, 0, 0};
      if (u->motion.bp->motionType == kMotionAir) AirPrepareMove(sim, u, &p);
      else GroundPrepareMove(sim, u, &p, none, 0);
      cx = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
      cz = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
    }
    const Vec3 goal{cx + fp.sizeX * 0.5f, tp.y, cz + fp.sizeZ * 0.5f};
    u->motion.arrived = u->motion.failed = false;
    // (the listener calls the navigator's SetGoal only, no UpdateSpeedThroughStatus 0x6ac940: a land navigator keeps
    // the speed-through request its steering has, +0x9d. Seen (MP1 825): army 2's engineer 0x20000a on a Move with a
    // Guard queued behind it (speed-through 1 since the Guard came, both in 0x552c10's set) has its waypoint dragged;
    // the original's goal waypoint at 827 still has 1; NextIsMove, which leaves Guard out, gave 0 until the per-beat
    // refresh put it back.)
    const bool through = u->motion.nav ? u->motion.nav->speedThroughGoal : NextIsMove(u) && MoveLike(c.type);
    MotionSetGoal(sim, u, std::vector<Vec3>{goal}, through, sim.tick());
    if (u->moveTaskReserved) {
      const int r[4] = {cx, cz, cx + fp.sizeX, cz + fp.sizeZ};
      GroundReserveRect(sim, u, r);
    }
  }
}

namespace {

void RunPathSearch(Sim& sim, Unit* u, bool /*continuing*/) {
  if (u->commands.empty()) return;
  UnitCommand& c = *u->commands.front();
  Vec3 goal = TargetPos(sim, c, u);
  // CUnitMoveTask's constructor 0x6180e0 (the test 0x618a00, then 0x618344): a Move that came from a factory (a
  // rally point: cmd+0x142), is the unit's last command, the unit not TransportLoading, Teleporting or Guarding,
  // has its target rounded to whole numbers and put through PrepareMove (0x62b780, spacing 0, no exclusion). So
  // the second unit out of a factory does not head for the cell the first one stands on: it gets the nearest free
  // spot of the ring two footprints out.
  if (c.type == CommandType::Move && c.factoryIssued && u->commands.size() == 1 && u->motion.bp &&
      !u->unitStates.Has(US("TransportLoading")) && !u->unitStates.Has(US("Teleporting")) && !u->unitStates.Has(US("Guarding"))) {
    Vec3 p{std::nearbyint(goal.x), goal.y, std::nearbyint(goal.z)};
    if (p.x != 0.0f || p.y != 0.0f || p.z != 0.0f) {
      const float none[4] = {0, 0, 0, 0};
      if (u->motion.bp->motionType == kMotionAir) AirPrepareMove(sim, u, &p);
      else GroundPrepareMove(sim, u, &p, none, 0);
      goal = {p.x, goal.y, p.z};
      // ... and the ctor reserves the footprint rect there (0x618613..0x618631: 0x50b010, ReserveOgridRect 0x6ab6f0,
      // +0x92 = 1) until the task ends. So the next unit out of the factory with the same rally point finds the
      // spot taken and goes to the ring two footprints out. Seen (MP3 1508): army 0's engineer 0x17 takes (188, 418)
      // in the original's navigator lines, as 0x12 (rallied at 1359, still on its way) holds (190, 416).
      const NamedFootprint& fp = UnitFootprint(u);
      const int x0 = static_cast<int>(std::nearbyint(p.x - fp.sizeX * 0.5f));
      const int z0 = static_cast<int>(std::nearbyint(p.z - fp.sizeZ * 0.5f));
      const int r[4] = {x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ};
      GroundReserveRect(sim, u, r);
      u->moveTaskReserved = true;
    }
  }
  std::vector<Vec3> path;
  bool ok = true;
  path.push_back(goal);  // land units: the navigator plans (sim/landnav.cpp)
  if (!ok || path.empty()) {
    u->motion.failed = true;
    u->headState = kRunning;
    return;
  }
  // air units steer in this beat's motion; land units' navigators plan from the next beat on
  uint32_t drive = sim.tick();
  bool through = NextIsMove(u) && MoveLike(c.type);
  MotionSetGoal(sim, u, path, through, drive);
  u->headState = kRunning;
}

}  // namespace

namespace combat {
bool IsAlly(const Army* a, const Army* b);
bool IsEnemy(const Army* a, const Army* b);
}  // namespace combat

std::vector<Unit*> ScriptUnitList(lua_State* L, int idx) {
  if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = lua_gettop(L) + idx + 1;
  if (!lua_istable(L, idx)) {
    lua_Debug ar;  // the binding's name, as the original passes it (e.g. "IssueMove")
    const char* fn = (lua_getstack(L, 0, &ar) && lua_getinfo(L, "n", &ar) && ar.name) ? ar.name : "?";
    luaL_error(L, "Invalid unit set in %s; expected a table but got a %s", fn, lua_typename(L, lua_type(L, idx)));
  }
  std::vector<Unit*> out;
  const int count = ObjectCount(L, idx);
  for (int i = 1; i <= count; ++i) {
    lua_rawgeti(L, idx, i);
    Unit* u = OptObject<Unit>(L, -1);
    lua_pop(L, 1);
    if (!u || u->dead) continue;
    auto at = std::lower_bound(out.begin(), out.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
    if (at == out.end() || *at != u) out.insert(at, u);
  }
  return out;
}

void CheckScriptTarget(lua_State* L, int idx) {
  if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = lua_gettop(L) + idx + 1;
  if (lua_type(L, idx) == LUA_TNIL) return;
  if (ToObject<Entity>(L, idx)) return;
  if (lua_istable(L, idx) && ObjectCount(L, idx) == 3) return;
  lua_Debug ar;
  const char* fn = (lua_getstack(L, 0, &ar) && lua_getinfo(L, "n", &ar) && ar.name) ? ar.name : "?";
  const int t = lua_type(L, idx);
  luaL_error(L, "Invalid target set in %s; expected an entity or a Vec3 but got a %s", fn,
             t == LUA_TNONE ? "nil" : lua_typename(L, t));
}

// ---- CUnitPatrolTask: Patrol 16, FormPatrol 18, AggressiveMove 35, FormAggressiveMove 36 ----------------
// (engine-ref attack_move.md 2). Runs every 6 ticks; finds enemies in a box along its leg and pushes an
// attack child; otherwise (re-)issues the navigator goal; ends when the unit is within ceil(speed) cells of
// the goal cell (from the 4th run on).
struct PatrolData {
  bool goalIssued = false, atGoal = false, aggressive = false;
  // task+0x5c: the goal (SNavGoal) the dispatcher worked out when it made the task (CUnitCommand::GetPosition at
  // 0x6097cb / 0x609809 / 0x60985f, copied by the ctor 0x61aed2) and that only the command's event changes (OnEvent
  // 0x61c3e0: GetPosition again, 0x61c43c). TaskTick hands this one to the navigator (0x61c3aa) and the arrival test
  // 0x61b610 reads it (0x61b61c), so it does not follow the command's later state: a formation command whose other
  // units have left keeps giving the unit its formation cell. Seen: MP2 6057, army 1's 0x100031, the last of six
  // aircraft on a formation AggressiveMove, ends it at its formation cell's rect (247, 215) +- 15; ours, asking the
  // command again, took the order's point (only one unit left: 0x6e8a42) and flew on to 6063.
  Vec3 goal{};
  int ticks = 0;
  PatrolBox box;
  BuildTask* child = nullptr;
  uint32_t childFrom = 0;
  std::vector<uint32_t> tried;  // +0xc8: what the reclaim helper sent the unit to reclaim (never again)
};
const BuildTask* PatrolChildTask(const BuildTask& t) { return t.pdata ? t.pdata->child : nullptr; }

namespace {

bool IsPatrolType(CommandType t) {
  return t == CommandType::Patrol || t == CommandType::FormPatrol || t == CommandType::AggressiveMove ||
         t == CommandType::FormAggressiveMove;
}

// RecomputePatrolSearchBox 0x61b2f0: the leg from the previous patrol point (or the unit) to the goal,
// widened by GuardScanRadius
// (0x61b397 / 0x61b406: a patrol point is CUnitCommand::GetPosition 0x6e8a30, the unit's footprint-origin cell at the
// order's target, and back through COORDS_ToWorldPos 0x50b0f0: the cell's footprint centre at the height of the ground,
// or of the water unless the footprint may stand on the seabed. Only the unit's own place is taken as it is. Seen
// (MP3 1567): army 0's engineer 2 on a three-point patrol; a cactus 0.25 outside the box built from the raw points is
// inside the original's, which sends the engineer to it.)
Vec3 PatrolPoint(Sim& sim, const UnitCommand& c, Unit* u) {
  const Vec3 t = TargetPos(sim, c, u);
  const NamedFootprint& f = UnitFootprint(u);
  const int cx = static_cast<int>(std::nearbyint(t.x - f.sizeX * 0.5f));
  const int cz = static_cast<int>(std::nearbyint(t.z - f.sizeZ * 0.5f));
  const float x = static_cast<float>(cx) + f.sizeX * 0.5f, z = static_cast<float>(cz) + f.sizeZ * 0.5f;
  const TerrainMap* map = sim.map();
  float y = map ? map->TerrainHeight(x, z) : 0.0f;
  if (map && map->hasWater && map->waterElevation > y && !(f.caps & kLayerSeabed)) y = map->waterElevation;
  return {x, y, z};
}

void PatrolSearchBox(Sim& sim, Unit* u, PatrolData& p) {
  if (u->commands.empty()) return;
  const UnitCommand& cur = *u->commands.front();
  const UnitCommand& last = *u->commands.back();
  Vec3 E = PatrolPoint(sim, cur, u);
  Vec3 S = u->position;
  if (&last != &cur && (last.type == CommandType::Patrol || last.type == CommandType::FormPatrol))
    S = PatrolPoint(sim, last, u);
  float dx = E.x - S.x, dz = E.z - S.z, len = std::sqrt(dx * dx + dz * dz);
  float r = GuardScanRadiusOf(u);
  p.box.cx = (E.x + S.x) * 0.5f;
  p.box.cz = (E.z + S.z) * 0.5f;
  p.box.cy = (S.y * 2.0f) * 0.5f;
  // 0.001 > len: (0, 0, 1); otherwise Normalize 0x452af0 (0x61b4e1: times 1 / length; a NaN gives zero)
  if (0.001f > len) {
    p.box.dx = 0.0f;
    p.box.dz = 1.0f;
  } else {
    Vec3 dir{dx, 0.0f, dz};
    vm::NormalizeExe(dir);
    p.box.dx = dir.x;
    p.box.dz = dir.z;
  }
  p.box.side = r;
  p.box.along = len * 0.5f + r;
}

// u+0x594 as Unit::UpdateInfoCache last wrote it: for an aircraft Air.MaxAirspeed x the speed multiplier, not the
// blueprint's Physics.MaxSpeed. Seen: Skirmish 1930, army 7's UEF interceptor 0x700013 (MaxAirspeed 15, Physics.MaxSpeed
// 0.5) on patrol: at its 4th patrol TaskTick the original finds it within ceil(15) cells of the patrol point
// (AtPatrolGoal 0x61b610) and ends the leg; ours measured 1 cell and sent it round the point again.
float UnitTopSpeed(const Unit* u) {
  const UnitMotion& m = u->motion;
  if (m.maxSpeedU594 > 0) return m.maxSpeedU594;
  if (m.speedCap > 0) return m.speedCap;
  return m.bp ? m.bp->maxSpeed * m.speedMult : 0;
}

// AtPatrolGoal 0x61b610
bool AtPatrolGoal(Sim& sim, Unit* u, const PatrolData& p) {
  if (u->commands.empty() || !u->motion.bp) return false;
  const NamedFootprint& f = UnitFootprint(u);  // (0x61b671: Entity::GetFootprint)
  const Vec3& g = p.goal;  // (task+0x5c, 0x61b61c)
  int gx = static_cast<int>(std::nearbyint(g.x - f.sizeX * 0.5f));
  int gz = static_cast<int>(std::nearbyint(g.z - f.sizeZ * 0.5f));
  int x0 = gx, z0 = gz, x1 = gx + 1, z1 = gz + 1;
  if (p.goalIssued) {
    int m = static_cast<int>(std::ceil(UnitTopSpeed(u)));
    x0 -= m;
    z0 -= m;
    x1 += m;
    z1 += m;
  }
  int cx = static_cast<int>(std::nearbyint(u->position.x - f.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(u->position.z - f.sizeZ * 0.5f));
  return x0 <= cx && cx <= x1 && z0 <= cz && cz <= z1;
}

// CUnitPatrolTask::EvaluatePatrolReclaimAttack 0x61b9c0 (read 2026-10-16): what a PATROLHELPER on patrol stops for.
// Nothing for an ACU / SACU on AggressiveMove (the aggressive byte +0x82 is read only here). The candidates are the
// units and props whose primitive meets the patrol's search box (COGrid::CollectEntitiesInBox 0x721dc0, mask 0x300:
// the box's cells, the entity's AABB, then the primitive against the box), in the grid's order:
// - a unit: not dead, not U, standing still (its position the same as last beat's: memcmp 0x61bd4b), not on the Air
//   layer (unit+0x120 == 0x10); an enemy (IArmy::GetAllianceWith 0x579430 == 2) in RECLAIMABLE, weight 0.5; an ally
//   or U's own (== 1) only when both resources are stored above 3/4 of their room (army economy +0x18 / +0x1c
//   against 0.75 x the int64 storage +0x40 / +0x48, energy then mass), below 0.9 of its health and not
//   BeingReclaimed (unit state 15), weight 2.0; a neutral (0) never;
// - a prop: U in RECLAIM, the prop in RECLAIMABLE; with one resource stored above 3/4 it must give the other
//   (blueprint ReclaimEnergyMax bp+0x1a8 / ReclaimMassMax bp+0x1a4 > 0); and not one this patrol already sent the
//   unit to reclaim (the set +0xc8, 0x61c540); weight 1.
// Each must stand within the playable rect with a margin of 1 (STIMap::IsWithin 0x62ca60, 0x61bfd3; U's army's
// whole-map flag, vt+0xb8) and, while the no-rush timer (army+0x1cc) runs, within its circle (not modelled: no replay
// has no-rush). The least weight x 3D distance squared from U wins (strictly less, from +inf 0x10b19a4).
// Seen: Skirmish 1268, engineer 0x700007 on AggressiveMove reclaims a prop 1 cell away first (its navigator idle
// until the reclaim child moves it at 1274); ours walked off toward the goal.
Entity* PatrolReclaimTarget(Sim& sim, Unit* u, PatrolData& p) {
  if (!BpInCategory(sim, u->blueprint, "PATROLHELPER")) return nullptr;
  if (p.aggressive && (BpInCategory(sim, u->blueprint, "COMMAND") || BpInCategory(sim, u->blueprint, "SACU_BEHAVIOR")))
    return nullptr;
  if (!u->army) return nullptr;
  const ArmyEconomy& eco = u->army->econ;
  const bool energyHigh = eco.stored[kEnergy] > static_cast<float>(static_cast<double>(eco.maxStorage[kEnergy]) * 0.75);
  const bool massHigh = eco.stored[kMass] > static_cast<float>(static_cast<double>(eco.maxStorage[kMass]) * 0.75);
  // the search box (RecomputePatrolSearchBox 0x61b2f0): side = dir turned 90 degrees about +Y, up, dir
  const PatrolBox& b = p.box;
  const Vec3 centre{b.cx, b.cy, b.cz};
  // (the side: dir turned by the quarter-turn quaternion 0x10b1a44 through its matrix, 0x61b4f0..0x61b4f5; the
  // initialiser 0xbd11c0 makes it with fcos / fsin of pi / 4, as 0x10b965c: not exactly (dz, 0, -dx))
  const Vec3 axes[3] = {RotateQuarterTurnY(Vec3{b.dx, 0.0f, b.dz}), {0.0f, 1.0f, 0.0f}, {b.dx, 0.0f, b.dz}};
  const float half[3] = {b.side, 100.0f, b.along};
  const Vec3 ext{std::fabs(axes[0].x) * half[0] + std::fabs(axes[2].x) * half[2], half[1],
                 std::fabs(axes[0].z) * half[0] + std::fabs(axes[2].z) * half[2]};
  const Vec3 boxMn{centre.x - ext.x, centre.y - ext.y, centre.z - ext.z};
  const Vec3 boxMx{centre.x + ext.x, centre.y + ext.y, centre.z + ext.z};
  std::vector<Entity*> cand;
  sim.entityGrid().GatherBox(boxMn.x, boxMn.z, boxMx.x, boxMx.z, 3, &cand);
  const bool wholeMap = Sim::UseWholeMap(u->army);
  Entity* best = nullptr;
  float bestD = std::numeric_limits<float>::infinity();
  for (Entity* e : cand) {
    WorldShape ws;
    if (!GetWorldShape(e, &ws)) continue;
    Vec3 mn, mx;
    ShapeBounds(ws, &mn, &mx);
    if (boxMn.x > mx.x || mn.x > boxMx.x || boxMn.y > mx.y || mn.y > boxMx.y || boxMn.z > mx.z || mn.z > boxMx.z) continue;
    if (!ShapeOverlapsOBox(ws, centre, axes, half)) continue;
    float w = 1.0f;
    if (e->kind == Entity::Kind::Unit) {
      Unit* c = static_cast<Unit*>(e);
      if (c->dead || c == u) continue;
      if (std::memcmp(&c->position, &c->lastPosition, sizeof(Vec3)) != 0) continue;  // it moved
      if (c->layer == "Air") continue;
      const bool ally = c->army == u->army || combat::IsAlly(u->army, c->army);
      if (ally) {
        if (!(energyHigh && massHigh)) continue;
        if (c->health >= c->maxHealth * 0.9f) continue;
        if (c->unitStates.Has(US("BeingReclaimed"))) continue;
        w = 2.0f;
      } else if (c->army && combat::IsEnemy(u->army, c->army)) {
        if (!BpInCategory(sim, c->blueprint, "RECLAIMABLE")) continue;
        w = 0.5f;
      } else {
        continue;  // neutral
      }
    } else if (e->kind == Entity::Kind::Prop) {
      const Prop* pr = static_cast<const Prop*>(e);
      if (!BpInCategory(sim, u->blueprint, "RECLAIM")) continue;
      if (!pr->blueprint || !pr->blueprint->reclaimable) continue;
      if (energyHigh || massHigh) {
        const bool wantE = !energyHigh && pr->bpReclaimEnergy > 0.0f;
        const bool wantM = !massHigh && pr->bpReclaimMass > 0.0f;
        if (!wantE && !wantM) continue;
      }
      if (std::find(p.tried.begin(), p.tried.end(), EntityRef(e)) != p.tried.end()) continue;
    } else {
      continue;
    }
    const Vec3& q = e->position;
    if (!sim.map() || !sim.IsWithin(q.x, q.z, 1.0f, wholeMap)) continue;  // IsWithin(1, the army's flag)
    const float dx = u->position.x - q.x, dy = u->position.y - q.y, dz = u->position.z - q.z;
    const float d = (dx * dx + dy * dy + dz * dz) * w;
    if (bestD > d) {
      bestD = d;
      best = e;
    }
  }
  return best;
}

void PatrolSetGoal(Sim& sim, Unit* u, const PatrolData& p) {
  if (u->commands.empty()) return;
  std::vector<Vec3> path{p.goal};  // (task+0x5c, 0x61c3aa)
  u->motion.arrived = u->motion.failed = false;
  MotionSetGoal(sim, u, path, NextIsMove(u), sim.tick());
  // (no "Moving": that is the move task's state; a patrolling unit on its way only has "Patrolling", as seen in
  // the original's unit records)
  AirSpeedThroughEvent(sim, u);  // the patrol's TaskTick
}

}  // namespace

BuildTask* StartPatrolTask(Sim& sim, Unit* u, const UnitCommand& c) {
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->order = "Patrol";
  t->pdata = std::make_shared<PatrolData>();
  t->pdata->aggressive = c.type == CommandType::AggressiveMove;
  t->pdata->goal = TargetPos(sim, c, u);  // (the dispatcher's GetPosition, into the ctor's +0x5c)
  u->unitStates.Insert(US("Patrolling"));
  PatrolSearchBox(sim, u, *t->pdata);
  AirSpeedThroughEvent(sim, u);  // the patrol task ctor
  AttackerUnpackStop(sim, u);    // ctor 0x61b009..0x61b067: standing unpacked with AI.NeedUnpack: SetDesiredTarget(none)
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int TickPatrol(Sim& sim, Unit* u, BuildTask& t) {
  PatrolData& p = *t.pdata;
  if (p.child) {  // the child runs; when it ends the patrol runs again in the same tick
    if (sim.tick() < p.childFrom) return kTaskRunning;
    BuildTask& ch = *p.child;
    const CommandType chType = ch.type;
    const uint32_t chGoal = ch.goalId;
    int r = TickBuildTask(sim, u, ch);
    if (r == kTaskRunning) return kTaskRunning;
    // the child's result into the dispatcher's +0x2c (its +0x28 points there: the patrol makes its children with the
    // dispatcher, patrol+0x50, as parent, 0x61c232 / 0x61c2b6 / 0x61c306); the attack child writes none
    if (chType == CommandType::Repair) u->dispatchResult = r == kTaskDone ? 1 : 2;
    else if (chType == CommandType::Reclaim) u->dispatchResult = sim.FindEntity(chGoal) ? 2 : 1;
    p.child = nullptr;
  } else if (sim.tick() < t.waitUntil) {
    return kTaskRunning;
  }
  const uint32_t next = sim.tick() + 6;  // return 7
  if (!u->motion.bp || !u->motion.bp->mobile() || (p.goalIssued && p.atGoal)) return kTaskDone;
  ++p.ticks;
  // (b) enemies
  if (Unit* e = PatrolFindTarget(sim, u, p.box)) {
    u->leashPos = u->position;
    if (BuildTask* c = MakeAttackTaskOn(sim, u, e)) {
      u->dispatchResult = 0;  // (its CCommandTask ctor clears the parent's result, 0x5f095f)
      p.child = c;
      p.childFrom = next;
      p.goalIssued = false;
      t.waitUntil = next;
      return kTaskRunning;
    }
  }
  // (c) the reclaim / repair helper (0x61c268): an ally's unit is repaired (CUnitRepairTask::new 0x5f9e50), anything
  // else reclaimed (IssueReclaimTask 0x61ef60), and the latter is kept in +0xc8 (0x5c3a90) so it is not picked again
  if (Entity* r = PatrolReclaimTarget(sim, u, p)) {
    Unit* ru = r->kind == Entity::Kind::Unit ? static_cast<Unit*>(r) : nullptr;
    BuildTask* c = nullptr;
    if (ru && (ru->army == u->army || combat::IsAlly(u->army, ru->army))) {
      c = MakeRepairChild(sim, u, ru, false);
    } else {
      p.tried.push_back(EntityRef(r));
      // the reclaim branch builds a CAiTarget of the entity first (0x61c2e1..0x61c30b: 0x5c3a90 into +0xc8, then
      // UpdateTarget 0x5d55b0 from 0x61c2f9, then IssueReclaimTask 0x61ef60 with it): one Unit::PickTargetPoint draw
      // for a unit with AI.TargetBones (none for a prop), the value unused; the repair branch makes none (0x61c2ac..
      // 0x61c2bb: CUnitRepairTask::new 0x5f9e50 with the unit). Seen (probe mp1_9304): MP1 9304, the original draws
      // 0x6aaf94 for army 1's ship 0x1000d2 from the patrol task's UpdateTarget (caller 0x61c2fe) in the command
      // stage; ours drew nothing there and the stream ran one draw behind.
      TargetPointDraw(sim, r);
      c = MakeReclaimChild(sim, u, r);
    }
    u->dispatchResult = 0;  // (its CCommandTask ctor clears the parent's result, 0x5f095f)
    p.child = c;
    p.childFrom = next;
    p.goalIssued = false;
    t.waitUntil = next;
    return kTaskRunning;
  }
  // (d) move / arrival
  if (u->unitStates.Has(US("NeedToTerminateTask"))) return kTaskDone;
  UnitCommand* c = u->commands.empty() ? nullptr : u->commands.front().get();
  if (p.goalIssued && p.ticks > 3 && AtPatrolGoal(sim, u, p)) {
    // the task's formation (+0x58): DispatchTask gives one only to FormPatrol / FormAggressiveMove (0x60984e ->
    // 0x61c4e0 with GetFormation 0x6e8a00); Patrol and AggressiveMove make their task with none (0x6097c2 /
    // 0x6097f8 -> 0x61c480, push 0), so they end on arrival with no wait for the others. Seen: MP2 3132, army 1's
    // interceptor 0x100042, one of three on an AggressiveMove, ends it 15 cells from the point while the other two
    // are still 50 away; ours waited for the formation's groups.
    if (!c || !c->form || (t.type != CommandType::FormPatrol && t.type != CommandType::FormAggressiveMove)) return kTaskDone;
    if (u->formAllAtGoal || FormationGroupAtGoal(sim, *c, u)) return kTaskDone;
    t.waitUntil = next;
    return kTaskRunning;
  }
  if (!u->motion.hasGoal || !p.goalIssued) {
    PatrolSetGoal(sim, u, *t.pdata);
    p.goalIssued = true;
    p.atGoal = false;
  }
  t.waitUntil = next;
  return kTaskRunning;
}

// CUnitPatrolTask dtor 0x61b140
void EndPatrol(Sim& sim, Unit* u, BuildTask& t) {
  PatrolData& p = *t.pdata;
  if (p.child) {
    EndBuildTask(sim, u, *p.child, false);
    p.child = nullptr;
  }
  if (!IsAlive(u)) return;
  u->navIgnoreFormation = false;
  if (!vm::SameBits(u->position, u->lastPosition))  // (0x61b1a7..0x61b1c2, memcmp 0x4f0a50)
    MotionStop(u);  // AbortMove only when it moved this tick
  u->unitStates.Erase(US("NeedToTerminateTask"));
  u->unitStates.Erase(US("Patrolling"));
  u->leashPos = {};
}

// The patrol task's command listener 0x61c3e0 (attack_move.md 2.8, read): the goal at GetPosition(cmd, unit); the
// navigator's SetGoal only while the patrol's own goal is issued (not while a child runs); RecomputePatrolSearchBox
// 0x61b2f0 (S from where the unit stands now). A factory's rally orders are the factory's own command objects,
// shared with every unit that inherited them (InheritCommandsTo 0x5fa550), so dragging a rally point reaches the
// units still on it. The patrol's reclaim child listens to the same command (ReclaimTaskCommandEvent, build.cpp).
// NOT MODELLED: the attack and repair children's listeners (0x5f4000, 0x5f9d60). (A reclaim child's walk under way
// ends with the wake: TickPropReclaim.)
// The reclaim child's listener wakes the thread (0x620238..0x62026d, read): the frame count (+0x14) 0 and, when the
// thread is suspended (+0x18, set by its walking child's move task: CUnitMoveTask's ctor), re-linked at the stage's
// end, as the move task's navigator listener 0x618bb0 does (NavEventWakesCommandThread). The move task's own command
// listener 0x618c40 and the patrol's 0x61c3e0 only re-goal and re-link nothing (read). The command tells its
// listeners from the oldest linked on (0x6e94cf starts at the list head's +4; a link as in the move task's ctor,
// 0x6182f6..0x618315, puts a new listener at the head's +0); ours goes by unit id (the same order here).
// Seen (probe mp1_3150): MP1 beat 3149, army 7's player drags its engineers' AggressiveMove waypoint; at 3150
// 0x70003d and 0x70004f (reclaiming, awake) run first, then 0x700027, 0x700042 and 0x70004a, whose reclaim walks the
// event ended, in that order; at 3156 their patrol legs take the free spots by PrepareMove in that thread order.
// Ours kept 0x700027 first (last re-linked 3041) and the five took each other's cells (MP1 3158).
// A command with a formation tells the same listeners: CUnitCommand::SetTarget 0x6e8820 only moves the formation's
// centre first (0x6e887d..0x6e88bf) and then calls 0x6e94a0 either way (0x6e88c1), and neither listener (0x61c3e0,
// 0x620160) asks for a formation; the patrol's goal is GetPosition's, the formation cell while the command holds more
// units than one (TargetPos). Seen (probe mp1_5381): MP1 beat 5381, army 0's player drags the AggressiveMove 0x11
// (issued with a formation at 4311, only 0x1a left on it) while 0x1a reclaims the tree group 0x2ff00d29 for it: at
// 5382 the original's reclaim task has no target (+0x44 null, +0x4c set, state 0, not started), ends there and the
// patrol walks to (737, 270) (SetGoal from NewMoveTask 0x6190cb); ours left formation commands out and reclaimed on.
void RetargetAttackTasks(Sim& sim, UnitCommand& c) {
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  for (Unit* u : holders) {
    if (!IsAlive(u) || u->commands.empty() || u->commands.front().get() != &c) continue;
    if (u->headState != kRunning || !u->task) continue;
    AttackTaskCommandEvent(sim, u, *u->task, c);
  }
}

void RetargetPatrolTasks(Sim& sim, UnitCommand& c) {
  // (whatever the command's type is now: a patrol task under way keeps its listener when the type is changed,
  // HeadTaskType; read, not seen)
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  std::vector<Unit*> walkers;  // reclaim children whose walk is under way
  for (Unit* u : holders) {
    if (!IsAlive(u) || u->commands.empty() || u->commands.front().get() != &c) continue;
    if (u->headState != kRunning || !u->task || !u->task->pdata || !u->motion.bp || !u->motion.bp->mobile()) continue;
    PatrolData& p = *u->task->pdata;
    p.goal = TargetPos(sim, c, u);  // (OnEvent 0x61c3e0 stores it, +0x5c)
    if (p.goalIssued) {
      const Vec3 goal = p.goal;
      if (u->motion.bp->motionType == kMotionAir) MotionSetGoal(sim, u, std::vector<Vec3>{goal}, u->motion.passThrough, sim.tick());
      else if (u->motion.nav) LandNavSetGoal(sim, u, goal, u->motion.nav->speedThroughGoal);
    }
    PatrolSearchBox(sim, u, p);
    BuildTask* ch = p.child;
    if (ch && ch->type == CommandType::Reclaim && !ch->pdata && !ch->gdata && !ch->adata) {
      const bool asleep = TaskThreadAsleepOnMove(u, ch);  // (asked before the event changes the child)
      ReclaimTaskCommandEvent(sim, u, *ch, c);
      p.childFrom = 0;  // (the listener wakes the thread: counter 0)
      if (asleep) walkers.push_back(u);
    }
  }
  // The walk's own listener (linked after the reclaim task's): its navigator takes the command's new position at once.
  // Seen (probe mp1_3341): MP1 beat 3340, army 7's player drags the AggressiveMove again; the walks of 0x700055 and
  // 0x70005a (rally overridden: free spots by PrepareMove) get the goals (360, 584) and (362, 584) from 0x618d76 in the
  // stream, their navigators plan at 3341 before the command stage and the drive-through byte is 0 after 3341; ours
  // set the goal only in the command stage and the byte followed a beat later.
  // Both listeners are told in the command's listener order, the oldest linked first (0x6e94cf), which for the walks
  // is the order they were made in, not the unit ids: their re-links (the reclaim task's) and their PrepareMove spots
  // (the walk's) follow it. Seen (probe mp1_3632): MP1 beat 3623, army 0's player drags its rally AggressiveMove; the
  // walks are re-goaled 0x49, 0x3c, 0x42, 0x46, 0x3a, 0x44, the first taking the waypoint's own cell (787, 173), and
  // the threads run in that order at 3624; ours went by id (0x3a first took (787, 173)), and from 3630 the walks of
  // 0x3a, 0x44 and 0x49 headed for each other's spots (MP1 3632).
  std::stable_sort(walkers.begin(), walkers.end(), [](const Unit* a, const Unit* b) { return a->walkSeq < b->walkSeq; });
  for (Unit* u : walkers) {
    TaskWalkCommandEvent(sim, c, u);
    sim.ResumeCommandThread(u);
  }
}

// The mobile build tasks listening to a command whose target was set (CUnitCommand::SetTarget 0x6e8820 -> 0x6e94a0):
// CUnitMobileBuildTask::OnEvent 0x5f80c0 for each unit that runs it on this command (the queue's head), in unit id
// order (inferred, as for the patrol's listeners); a thread asleep on the task's walk is re-linked at the end. The
// walk's own listener (CUnitMoveTask::OnEvent 0x618c40, linked after the build task's) hands its navigator the
// command's position first; the woken thread then ends the walk (TickMobileBuild state 0).
void RetargetMobileBuildTasks(Sim& sim, UnitCommand& c) {
  if (c.type != CommandType::BuildMobile) return;
  std::vector<Unit*> holders(c.units.begin(), c.units.end());
  std::sort(holders.begin(), holders.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  std::vector<Unit*> walkers;
  for (Unit* u : holders) {
    if (!IsAlive(u) || u->commands.empty() || u->commands.front().get() != &c) continue;
    if (u->headState != kRunning || !u->task || u->task->type != CommandType::BuildMobile) continue;
    const bool asleep = TaskThreadAsleepOnMove(u, u->task);  // (asked before the event changes the task)
    MobileBuildCommandEvent(sim, u, *u->task, c);
    if (asleep) walkers.push_back(u);
  }
  std::stable_sort(walkers.begin(), walkers.end(), [](const Unit* a, const Unit* b) { return a->walkSeq < b->walkSeq; });
  for (Unit* u : walkers) {
    if (!IsAlive(u)) continue;
    TaskWalkCommandEvent(sim, c, u);
    sim.ResumeCommandThread(u);
  }
}

namespace {

void StartHead(Sim& sim, Unit* u) {
  // Carried by a transport: nothing is dispatched until it is let go. Then the queue runs again from the NEXT
  // beat, also while the unit still falls (seen: a tank dropped by "unload these units" has that order in its own
  // queue, three times; it cannot carry them out and all three are gone the beat after the drop, long before it
  // lands). Not in the beat of the drop itself: the transport's thread, woken by its arrival, has its turn after
  // the cargo's. What blocks is unit state Attached alone (IAiCommandDispatchImpl::TaskTick 0x598f89, IsUnitState
  // 14), not the attach link: cargo a script took out of that state runs its orders (a DetachFromTransport, say).
  if (u->unitStates.Has(US("Attached")) || u->detachedAt == sim.tick()) {
    u->headState = kNotStarted;
    return;
  }
  // IAiCommandDispatchImpl::TaskTick 0x598f6d: nothing is dispatched while the unit is being built (IUnit vtable
  // +0x34 = the being-built flag, entity +0x98). Orders given to a factory under construction wait in its queue
  // until it stands. (Seen in a live game: the unfinished factory began the unit ordered from it, and its
  // builder with it.)
  if (u->beingBuilt) {
    u->headState = kNotStarted;
    return;
  }
  // ... nor while the unit is in state BlockCommandQueue (TaskTick 0x598f99: IsUnitState 42 through IUnit vt+0x3c
  // 0x6a4af0; Unit:SetBlockCommandQueue 0x6c65f0 sets that bit, +0x4a4 0x400). FAF sets it on a HUMAN army's
  // commander from the start until its warp-in ends (ScenarioUtilities CommanderWarpDelay; WarpInEffectThread clears
  // it in a Lua thread, after the command stage), and on a factory during its roll-off. Seen (MP3, army 2's ACU):
  // a build order given during the warp-in waits until the beat after the clear; ours had started it at once and
  // built two beats early.
  if (u->unitStates.Has(US("BlockCommandQueue"))) {
    u->headState = kNotStarted;
    return;
  }
  u->dispatchResult = 0;  // (0x598fc4, before DispatchTask)
  while (!u->commands.empty()) {
    UnitCommand& c = *u->commands.front();
    u->headType = static_cast<int>(c.type);  // (HeadTaskType)
    if (c.type == CommandType::Stop) {
      // DispatchTask's case for Stop (0x608f98 -> 0x60a490) does not touch the motion: it clears the attacker's
      // desired target (0x5d56f0), calls unit+0x558's vt+0x2c and sets unit+0xa2. What stops the unit is the
      // queue being cleared when the order ARRIVES: the running move task is torn down and its navigator makes
      // the brake spline and hands out its first point there and then. By the next beat the steering hands out
      // the second, so a unit told to stop skips the first brake point (seen: thirteen hovercraft, each one
      // brake step ahead of ours from the beat after the order). A second stop here would make the brake spline
      // afresh from where the unit stands. (A Stop a SCRIPT queued has not cleared anything when it gets here:
      // not read what the original does with one behind other orders; it stops the motion as before.)
      const bool fromStream = c.fromStream;
      // The attacker's Stop (0x60a49e, when unit+0x544 has one) clears the desired target and tells every weapon
      // SetTarget(none), so the acquire tasks look again, an AutoInitiateAttackCommand weapon two beats on. Seen: MP2
      // 7325, army 0's interceptors (ura0102) given a Stop by the player while their AutoCannons hold an air target:
      // the original's weapons look again and auto-attack it (an Attack order at 7328); ours kept the targets and
      // never ordered the attack.
      AttackerStop(sim, u);
      PopHead(u);
      if (!fromStream) MotionStop(u);
      SetMoving(u, false);
      continue;
    }
    if (SiloCommand(sim, u, c)) {
      PopHead(u);
      continue;
    }
    if (c.type == CommandType::Dive) {
      // DispatchTask 0x60971d: a Dive order toggles between the Water and the Sub layer and is done at once
      SetNewTargetLayer(sim, u, u->layer == "Sub" ? kLayerWater : kLayerSub);
      PopHead(u);
      continue;
    }
    // Orders the dispatcher carries out on the spot, without a task:
    if (c.type == CommandType::KillSelf) {  // 0x60b850: the unit kills itself (instigator itself, type "Damage")
      PopHead(u);
      KillUnit(sim, sim.L(), u, u, "Damage", 0.0f);
      return;
    }
    if (c.type == CommandType::DestroySelf) {  // 0x60a3c4: Entity::Destroy
      PopHead(u);
      sim.QueueDestroy(u);
      return;
    }
    if (c.type == CommandType::Pause) {  // 0x608fa4: Unit::SetPaused(!paused)
      u->paused = !u->paused;
      sim.CallMethod(sim.L(), u, u->paused ? "OnPaused" : "OnUnpaused", 0);
      PopHead(u);
      continue;
    }
    {
      // DetachFromTransport, a teleporter's new destination, a carrier docking on itself (transport.cpp): no task, so
      // the dispatcher pops the order and goes on with the next one in the same beat (TaskTick answers 0)
      std::shared_ptr<UnitCommand> keep = u->commands.front();
      if (TransportOrderOnTheSpot(sim, u, c)) {
        if (u->dead || u->destroyQueued) return;
        if (!u->commands.empty() && u->commands.front() == keep) PopHead(u);
        continue;
      }
    }
    if (c.type == CommandType::SpecialAction) {  // 0x6095d0: the script's OnSpecialAction(target position)
      Entity* te = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
      const Vec3 p = te ? te->position : c.pos;
      PopHead(u);
      lua_State* L = sim.L();
      PushVector(L, p.x, p.y, p.z);
      sim.CallMethod(L, u, "OnSpecialAction", 1);
      if (u->dead || u->destroyQueued) return;
      continue;
    }
    switch (c.type) {  // DispatchTask 0x608ef0: CUnitCommand::GenerateFormation (formations.md 1.2)
      case CommandType::Move:
      case CommandType::FormMove:
      case CommandType::Attack:
      case CommandType::FormAttack:
      case CommandType::FormPatrol:
      case CommandType::FormAggressiveMove:
      case CommandType::AggressiveMove:
      case CommandType::AssistMove:
        GenerateFormation(sim, c, u);
        break;
      case CommandType::Guard:
        if (!c.targetId) GenerateFormation(sim, c, u);  // (entity guards use the guard formation)
        break;
      default:
        break;
    }
    // (no Immobile test anywhere in DispatchTask 0x608ef0: a unit standing unpacked (Unit:SetImmobile) is given its
    // move or patrol task as any other; the task's ctor packs it up (AttackerUnpackStop) and the steering holds
    // still while the state lasts. Seen: LAND 1679, two heavy artillery (url0304) unpacked and firing were given a
    // player's Move: the original's navigators took the goal at 1680 and the units drove off; ours dropped the
    // order ("not carried out ... immobile").)
    if (IsPatrolType(c.type) && u->motion.bp && u->motion.bp->mobile()) {
      u->task = StartPatrolTask(sim, u, c);
      u->headState = kRunning;
      return;
    }
    // DispatchTask's Reclaim case (0x609894: IssueReclaimTask 0x61ef60(parent, &cmd+0x11c)) makes the task only for
    // a live entity: the order's CAiTarget weak entity (+4, 0x61ef7e..0x61ef8c) null -> parent+0x2c = 2 (0x61efdc)
    // and no task, so the dispatcher pops the order in the same beat. That is every order that names a place
    // (DecodeFromSSTITarget 0x5e2620: type 2 = {2, no entity, place}) and every entity order whose entity was gone
    // when the command was made (type 1: looked up by id, null if gone). Seen (Skirmish 681): the player's Reclaim
    // at 680 names tree group 0x2ff004d0, which broke up at 679 (TreeGroup.OnCollision -> Breakup -> Destroy); the
    // original's queue is empty at 681 and the unit just brakes; ours sent it to the old approach fallback below
    // (Moving, a path search) until the player's next Reclaim at 683.
    if (c.type == CommandType::Reclaim && !(c.targetId && sim.FindEntity(c.targetId))) {
      PopHead(u);
      continue;
    }
    if (c.type == CommandType::BuildFactory) {
      // DispatchTask's BuildFactory case (0x6093bc): no blueprint, or a unit whose script says no to it
      // (CScriptObject::RunScriptBool 0x60c2c0, "CheckBuildRestriction" with the blueprint's Lua table, 0x6093c4..
      // 0x609419), gets no task, and TaskTick takes the order as carried out (OrderTakenWithoutTask). Seen: MP3
      // 4460, army 0's T1 land factory 0x34 has
      // finished its upgrade into the T2 0x39 (which got its five BuildFactory orders, NotifyUpgrade) and is
      // destroyed; in that beat its dispatcher goes on with those orders, FAF's CheckBuildRestriction (CanBuild)
      // refuses the T2 engineers to the T1 factory, and its queue is emptied (probe mp3_site: empty when 0x39's
      // site test meets it). With no order at its head the old factory then counts as standing on the site
      // (LocationIsFree, flag 1), and 0x39 waits 50 beats before its first unit.
      const BlueprintInfo* fbp = sim.blueprints().Find(c.blueprintId);
      bool allowed = fbp != nullptr;
      if (allowed) {
        lua_State* L = sim.L();
        sim.blueprints().PushTable(L, *fbp);
        allowed = ::moho::combat::CallMethodBool(sim, L, u, "CheckBuildRestriction", 1) == 1;
      }
      if (u->dead) return;
      if (!allowed) {
        if (OrderTakenWithoutTask(u)) continue;
        return;
      }
    }
    if (c.type == CommandType::Upgrade && !UpgradeOrderFits(sim, u, c)) {
      // (UpgradeOrderFits.) Seen: Economy 3079, a T1 mex (0xf) finishes its upgrade into the T2 0x3b and is
      // destroyed; NotifyUpgrade has handed 0x3b the next order, the upgrade to T3. In that beat the T1's dispatcher
      // goes on with its own copy of that order: the T3 does not upgrade from the T1, so no task, and the order is
      // dropped (only 0x3b starts the T3, 0x4b).
      if (OrderTakenWithoutTask(u)) continue;
      return;
    }
    if (BuildTask* bt = StartBuildTask(sim, u, c)) {
      u->task = bt;
      u->headState = kRunning;
      return;
    }
    if (c.type == CommandType::Guard) {
      // DispatchTask's Guard case (0x609775): an order whose target is none (CAiTarget::HasTarget 0x5e2a10: no live
      // entity and no place) makes no task and goes the way of Stop (0x6097b8 -> 0x60a490): the attacker's Stop
      // (0x60a49e), the order is done (dispatcher +0x2c = 1), the motion is left alone. Seen: MP3 4473, six of army
      // 1's engineers given a Guard with no target left (no entity, no place): the original's queues are empty at
      // 4474; ours walked them off toward the order's empty place.
      AttackerStop(sim, u);
      PopHead(u);
      continue;
    }
    bool mobile = u->motion.bp && u->motion.bp->mobile();
    if ((MoveLike(c.type) || ApproachLike(c.type)) && mobile) {
      SetMoving(u, true);
      bool driving = u->motion.hasGoal || (u->motion.vel.x * u->motion.vel.x + u->motion.vel.z * u->motion.vel.z) > 1e-6f;
      u->motion.arrived = false;
      u->motion.failed = false;
      (void)driving;
      RunPathSearch(sim, u, true);  // land units queue their search in the navigator
      // NewMoveTask 0x6190a0 sets the navigator's goal first, then the ctor (CUnitMoveTask 0x618638, the
      // form-and-move task's 0x619401) packs up a unit standing unpacked with AI.NeedUnpack
      if (MoveLike(c.type)) AttackerUnpackStop(sim, u);
      if (MoveLike(c.type)) AirSpeedThroughEvent(sim, u);  // the move task ctor
      return;
    }
    // Not carried out yet: finish it at once so the scripts see an idle unit.
    // MOHO64_DEBUG_TASKS=1: every (command type, blueprint) this happens to, once, with the tick and the count
    // so far for the type
    {  // (the process's lists, to log each case once; the lock: two sims may run at once)
      static std::mutex logMutex;
      static std::map<int, int> notDone;
      static std::set<std::pair<int, std::string>> notDoneFor;
      static std::set<int> logged;
      static const bool dbgTasks = getenv("MOHO64_DEBUG_TASKS") != nullptr;
      std::lock_guard<std::mutex> logLock(logMutex);
      if (dbgTasks) {
        const int n = ++notDone[static_cast<int>(c.type)];
        const std::string bp = u->blueprint ? u->blueprint->id : "?";
        if (notDoneFor.insert({static_cast<int>(c.type), bp}).second)
          Logf(LogLevel::Info, "moho64: tick %u: command %s not carried out for %s (unit %#x, %s%s; x%d for the type)",
               sim.tick(), kCommandNames[static_cast<int>(c.type)], bp.c_str(), u->id, mobile ? "mobile" : "immobile",
               c.targetId ? ", entity target" : "", n);
      }
      if (logged.insert(static_cast<int>(c.type)).second)
        Logf(LogLevel::Debug, "moho64: command %s is not carried out yet (finished at once)",
             kCommandNames[static_cast<int>(c.type)]);
    }
    PopHead(u);
  }
  SetMoving(u, false);
}

}  // namespace

// The head task's teardown is itself the motion's last call: a pod's CUnitPodAssist. ClearCommandQueue 0x6ee2d0
// tells the dispatcher (event 2, IAiCommandDispatchImpl::OnEvent 0x599030), which deletes the tasks above it top
// down (CTask::TaskInterruptSubtasks 0x408d70): the assist child first, the pod task last, whose dtor 0x61d4f0 ->
// Kill 0x61d820 ends with CUnitMotion::Stop 0x6b8460(motion, 0) (0x61d988: the target where the drone is; ours
// EndPod -> PodLeave). Nothing after it aborts the move: cfunc_IssueClearCommandsL 0x6f1950 calls only
// ClearCommandQueue (0x6f19e6) and CAiAttackerImpl::Stop (0x6f19f5). Seen (Upgrades 3697): a support commander's
// OnStopBuild -> RefreshPodFocus clears its drone's queue while the drone assists; the original's target is the
// drone's own place, ours (the navigator's AbortMove after the teardown) was the point a second ahead.
static bool HeadTaskStopsMotion(const Unit* u) {
  return u->isPod && u->task && u->task->type == CommandType::AssistCommander;
}

// The unit's command thread sleeps on its navigator: a dispatched Move task, or a task's walking child (the
// CUnitMoveTask a repair, reclaim, capture ... task pushes on the thread: its ctor 0x6180e0 suspends the thread until
// the navigator's event, item 111). Clearing the queue destroys the command, which wakes the thread, and a woken
// thread is linked in at the command stage's new end (beat_order.md 2.6). Seen (Skirmish 809): M28 re-issues the
// commander's Repair (IssueClearCommands + IssueRepair, m28orders.lua:791) at 716 while its walking child is under
// way; the original's commander then works on the structure after both engineers every beat (the structure's
// fraction, summed in that order, matches the original's to the bit from 742; ours added the commander's share
// between the two and differed in the last bits), so an engineer finishes it at 809, not the commander, and M28's
// OnConstructed (first finisher only) gives the commander no new order.
// A patrol or attack-move under way (CUnitPatrolTask, land units): its dtor 0x61b140 makes the only abort of the move
// when its task is torn down, and only when the unit moved this beat (EndPatrol).
static bool PatrolTaskDecidesAbort(const Unit* u) {
  return !u->motion.air && !u->commands.empty() && u->headState == kRunning &&
         IsPatrolType(HeadTaskType(u)) && u->task && u->task->pdata;
}

static bool ThreadAsleepOnMove(const Unit* u) {
  if (u->commands.empty() || u->headState == kNotStarted) return false;
  if (HeadTaskType(u) == CommandType::Move) return true;
  // an attack task with its target in reach suspends the thread until its weapon reports (TaskTick's in-range branch
  // returns -2, 0x5f3a63; ours marks it waitUntil = ~0); the queue's events re-link a suspended thread whatever
  // suspended it (OnEvent 0x599030). Seen (probe mp1_5234): MP1 5232, eleven of army 1's ships given a formation
  // Attack with clear; the frigate 0x100056, standing in reach of its target since 5220, runs after the other
  // members at 5233 in the original (their slots reserved before its own: (498.98, 200.55), its square's corner
  // (482,184)); ours kept it fifth and its slot was the free raw place.
  // The same for a guard's attack child in reach (ThreadAttackTask). Seen (probe mp1_7061): MP1 7055, army 2's four
  // subs and the ship 0x200044 on a Guard of an enemy blip, attacking from it since 7037 (GuardBusy; the subs' attacks
  // end at 7044 and start again at 7050), get a formation Move with clear; at 7056 the original dispatches it in the
  // thread order 0x20003a, 0x20007d, 0x200007, 0x200076, 0x200044 (CUnitCommand::GetPosition 0x6e8a5d ->
  // GetAdjustedFormationPosition -> FindSlotFor 0x59aa20), so the sub 0x20007d takes its raw slot (563.18, 273.71) and
  // the ship rings out to (562.12, 268.42). Ours had never re-linked a guard's suspended attack and went by id: the
  // ship's ring took (563.12, 273.42), which refused the sub's slot and its rings out to 3, and the sub drove to
  // (562.19, 277.71) (MP1 7061).
  if (const BuildTask* a = ThreadAttackTask(u); a && a->waitUntil == 0xffffffffu) return true;
  // a patrol whose repair / reclaim child walks: the child's CUnitMoveTask suspended the thread (ctor 0x6186d6, the
  // same listener NavEventWakesCommandThread has for it), and the clear wakes it to the run list's end. Seen: MP3
  // 3109, army 0's engineer 0xf, its patrol's child walking, gets a BuildMobile with clear together with the idle 0x2e
  // and 0x2f, and the three reclaim the site prop 0x2ff010e3. In the original 0xf leaves Reclaiming at 3117 and 0x2e
  // at 3118: 0x2e takes the last step and 0xf, after it in the run list, ends at once (the order is inferred from
  // those states). Ours kept 0xf first (its last re-link 2875): 0xf took the last step and 0x2e ended at once.
  if (IsPatrolType(HeadTaskType(u))) {
    const BuildTask* c = u->task && u->task->pdata ? u->task->pdata->child : nullptr;
    return c && TaskThreadAsleepOnMove(u, c);
  }
  return TaskThreadAsleepOnMove(u);  // (the head task's walking child, or its guard child's: build.cpp)
}

// Whether the navigator's event (Succeeded / Failed) re-links the unit's command thread. Only a move task listens
// to the navigator: CUnitMoveTask (a Move order's own task, or a task's walking child, NewMoveTask 0x6190a0) and
// CUnitFormAndMoveTask suspend the thread in their ctors (0x6186d6, 0x6194b8: +0x18 = 1, off the run list), and
// their OnEvent (0x618c11, 0x6196db) puts a SUSPENDED thread back at the stage's new end (every inlined resume in
// the exe tests +0x18 first). A goal set on the navigator directly wakes nobody:
// - SIM_TryToBuild 0x62dd40 sends a standing unit away with navigator vt+8 SetGoal (no command, no task), whether
//   the unit is idle or busy with a task of its own (build.cpp TryToBuild);
// - the patrol / attack-move task (CUnitPatrolTask::TaskTick 0x61c130) calls nav.SetGoal and returns 7: no
//   navigator listener (attack_move.md 2.3), so a patrol leg's arrival re-links nothing, and neither does a clear
//   (its thread is not asleep);
// - the attack task (CUnitAttackTargetTask) sets the navigator's goal itself and listens to its attacker and its
//   command only (attack_move.md 3: in range it suspends with the navigator still going; the weapon codes wake it);
// - a build-like task (build, repair, reclaim, capture ...) or a guard: only while its walking child runs.
// Seen: Economy 4865, fourteen units given a Capture at 4864 reserve their spots beside the target in thread order.
// Ours had re-linked the commander 0x0 at 3091 (the end of a SIM_TryToBuild walk of 3027) and the engineer 0x4 at
// each patrol leg (4494-4506), so the engineer 0x3 (last re-linked 2095, its walk to a build site) came second and
// took (671, 179), whose path runs west of the T3 generator 0x17; in the original 0x4 and 0x0 come before it, take
// (671, 179) and (672, 179), and 0x3 takes (673, 179): its path and every beat of its walk until 5708 now match.
// (The attack and guard cases are read, not seen: no gate replay changes with them. A plain Move / FormMove head and
// the transport tasks keep re-linking at their navigator's end as before.)
bool NavEventWakesCommandThread(const Unit* u) {
  if (u->commands.empty() || u->headState == kNotStarted) return false;  // idle: a SIM_TryToBuild goal
  const BuildTask* t = u->task;
  if (IsPatrolType(HeadTaskType(u))) {  // only the patrol's child's walk listens
    const BuildTask* c = t && t->pdata ? t->pdata->child : nullptr;
    return c && (c->moving || (c->sub && c->sub->moving));
  }
  if (t && t->adata) return false;
  if (t && !t->tdata && !t->pdata) return TaskThreadAsleepOnMove(u);  // (its walking child, or its guard child's)
  return true;
}

void ForgetUnitCommands(Unit* u) {
  EndMoveTaskRect(u);
  if (u->engageId) ClearEngagement(*Sim::From(u->luaState()), u);
  if (u->task) {
    EndBuildTask(*Sim::From(u->luaState()), u, *u->task, false);
    u->task = nullptr;
  }
  auto drop = std::move(u->commands);
  u->commands.clear();
  for (auto& c : drop) ReleaseCommandImpl(u, *c);
  u->headState = kNotStarted;
  auto& q = Sim::From(u->luaState())->pathQueue;
  q.erase(std::remove(q.begin(), q.end(), u), q.end());
}

namespace {
// A finished move ends its command (the move task returns -1 and the dispatcher starts the next one). Answers true
// when the dispatcher waits a beat before the next command (TaskTick 0x598e80 answers 1).
bool ArrivalStep(Sim& sim, Unit* u) {
  if (!IsAlive(u) || u->commands.empty()) return false;
  if (u->headState != kRunning) return false;
  UnitMotion& m = u->motion;
  if (!m.arrived && !m.failed) return false;
  if (u->task) return false;  // a task command (build, guard, attack, ...) handles its own moves
  if (u->engageId) {  // arrived at the enemy it stopped for, not at the patrol point
    m.arrived = m.failed = false;
    return false;
  }
  UnitCommand& c = *u->commands.front();
  const CommandType task = HeadTaskType(u);  // (the move task's kind: the type the head was dispatched as)
  // CUnitFormAndMoveTask: arriving does not end it; it waits for its group to be at goal (formations.md 11)
  if (task == CommandType::FormMove && !m.failed && FormationHas(c, u)) {
    m.arrived = false;
    return false;
  }
  // The dispatcher reads the command's type as it is NOW (cmd+0x98, TaskTick 0x598ebb / 0x598f47): a Patrol or
  // FormPatrol (16 / 18) goes to the back of the queue, at once when the task's result word is 2 (the navigator's
  // failed event: the move task's listener 0x618bb0 writes 2, a success 1) and the next command is dispatched in the
  // same beat (0x598ee0 -> 0x598f6d); otherwise to the back when the queue holds another order (0x6edc20, 0x6edfc0),
  // else removed (0x6ede70), and the next command waits a beat (answer 1). A move task under a Patrol order is one
  // whose Move was turned into a Patrol after its dispatch (HeadTaskType); a patrol dispatched as one has its patrol
  // task.
  const bool patrol = c.type == CommandType::Patrol || c.type == CommandType::FormPatrol;
  const bool failed = m.failed;
  std::shared_ptr<UnitCommand> keep = u->commands.front();
  PopHead(u);
  m.arrived = false;
  if (patrol && (failed || !u->commands.empty())) {  // patrols cycle through their points
    u->commands.push_back(keep);
    keep->units.insert(u);
  }
  m.failed = false;
  SetMoving(u, false);  // the move task's dtor clears it; a following move sets it again
  // ... and the same destructor (0x618700) calls the navigator's AbortMove when the task ends by itself
  // (queue+0x1c is 0 then, and its table 0x618998 says "abort" for 0): an aircraft at the end of a Move order is
  // stopped at the point one second ahead (CAiNavigatorAir::AbortMove 0x5a4f00), not held at its goal cell
  if (task == CommandType::Move && u->motion.air) AirAbort(sim, u);
  // A land navigator's AbortMove 0x5a3750 is SetSpeedThroughGoal(0) (vt+0x18), MakeIdle (vt+0x4c 0x5a3e00:
  // SetWaypoints(0, 0) 0x5d29c0) and event 1 (the move task has already unlinked itself, 0x61885f). The navigator's
  // own end of the path (Execute 0x5a43dd: Stop, then MakeIdle) came first, in the army stage, and the steering of
  // the same beat may have made a new batch since: SetWaypoints(0, 0) then finds a spline (+0x80) and turns it into
  // a brake (CAiPathSpline::Update mode 3, 0x5d2b63), predicts collisions with it and hands out its first point. A
  // unit that stands makes a brake of one point, which is used up the next beat: Stop, no point, and the hull is
  // free to turn. Seen (Naval 605/606): battlecruiser 0x21 gives up its Move (stuck, 0x5aebb9) while a destroyer
  // passing it keeps putting a collision record on it; the navigator's Stop re-predicts, the record is resolved in
  // the steering stage into a seven-point hard brake (mode 4). The original stood on that beat and made its
  // broadside turn (AttackAngle 60) at 606: the move task's abort had cut the hard brake down to a one-point brake.
  // Ours ended the order without the abort, kept the hard brake and turned at 611.
  else if (task == CommandType::Move && u->motion.nav) MotionStop(u);
  if (u->commands.empty()) return false;
  if (patrol && !failed) return true;  // (the next command a beat later)
  StartHead(sim, u);
  return false;
}

void CommandStep(Sim& sim, Unit* u) {
  if (!u || u->dead) return;
  // Entity::Destroy 0x679af0 only flags and queues the entity; its command thread stays in the stage and the queue
  // lives until ~Unit (Purge, after the beat's drain). A task already running therefore still runs in the beat its
  // unit was destroyed in (CUnitUpgradeTask::TaskTick 0x5f8890 asks nothing of its unit's end; seen: Economy 3952, T2
  // mex 0x3a reclaimed by 0xa while upgrading, the original's upgrade 0x57 still gains its step in that beat), and
  // the dispatcher goes on with the queue: its tests (IAiCommandDispatchImpl::TaskTick 0x598f6d..0x598fb5) are
  // IsBeingBuilt (+0xa0), IsDead (+0xa1, which a Destroy sets only in the drain, OnDestroy 0x6a8a31), states Attached
  // and BlockCommandQueue, and an empty queue; nothing asks for the destroy (seen: MP3 4460, sim/commands.cpp's
  // BuildFactory dispatch).
  if (u->commands.empty()) return;
  int& st = u->headState;
  if (st == kNotStarted) StartHead(sim, u);
  if (u->task && st == kRunning) {
    // a finished task is popped and the dispatcher starts the next command at once; a new task's
    // first Execute is in the same pass (beat_order.md 2.6)
    for (int guard = 0; guard < 8 && u->task && st == kRunning; ++guard) {
      BuildTask* task = u->task;
      int r = TickBuildTask(sim, u, *task);
      if (r == kTaskRunning) break;
      u->task = nullptr;
      // a finished BuildFactory with a count builds again (CUnitCommand::DecreaseCount 0x6f16a0)
      if (r == kTaskDone && task->type == CommandType::BuildFactory && !u->commands.empty() &&
          u->commands.front()->type == CommandType::BuildFactory && u->commands.front()->count > 1) {
        --u->commands.front()->count;
        u->headState = kNotStarted;
      } else if (r == kTaskDone && u->repeatQueue && !u->commands.empty() &&
                 u->commands.front()->type == CommandType::BuildFactory) {
        // The repeat queue (IAiCommandDispatchImpl::TaskTick 0x598f0b): a factory order (type 7) whose count has
        // run out, on a unit whose repeat switch is on (unit+0x2a2), gets its count back (cmd+0xe4 = cmd+0xe0) and
        // goes to the END of the queue (0x6edfc0; alone in the queue it stays where it is). The dispatcher answers
        // 1: the next order starts a beat later.
        std::shared_ptr<UnitCommand> keep = u->commands.front();
        keep->count = keep->maxCount;
        if (u->commands.size() > 1) {
          PopHead(u);
          u->commands.push_back(keep);
          keep->units.insert(u);
        }
        if (!u->dead && !u->commands.empty()) u->headState = kNotStarted;
        break;
      } else if (task->pdata && (task->type == CommandType::Patrol || task->type == CommandType::FormPatrol) &&
                 !u->commands.empty()) {
        // a patrol point goes to the back of the queue (the loop); attack-moves are removed
        std::shared_ptr<UnitCommand> keep = u->commands.front();
        PopHead(u);
        u->commands.push_back(keep);
        keep->units.insert(u);
        // IAiCommandDispatchImpl::TaskTick 0x598ebb: with its result word at 2 (the patrol's last child failed) the
        // point goes to the back (0x598ee0) and the next one is dispatched at once (0x598f6d); otherwise (0x598fed)
        // the next patrol leg starts a beat later (seen: one beat with neither "Patrolling" nor the drive-through
        // byte between two legs, the new goal the beat after). Seen (MP3 3035): army 0's engineer 0x2, whose repair
        // child failed at 2945, ends its leg at 3035 and keeps Patrolling; ours had a beat without it and set the
        // next goal a beat late.
        if (u->dispatchResult == 2 && !u->dead && !u->commands.empty()) {
          StartHead(sim, u);
          continue;
        }
        if (!u->dead && !u->commands.empty()) u->headState = kNotStarted;
        break;
      } else if (!u->commands.empty() && u->commands.front()->type == CommandType::Attack &&
                 !u->commands.front()->targetId && !u->commands.front()->targetFakeId) {
        // IAiCommandDispatchImpl::TaskTick 0x598f47 / 0x598fdc..0x599023 (read): an Attack order (type 10, not
        // FormAttack) whose CAiTarget (cmd+0x11c) is a ground position (type 2) is not simply removed when its task
        // ends: with a second order in the queue (0x6edc20) the unit leaves the order's formation (FormRemoveUnit
        // 0x6e8d70) and the order goes to the BACK of the queue (MoveFirstCommandToBackOfQueue 0x6edfc0); alone it is
        // removed (0x6ede70). Either way the dispatcher answers 1: the next order starts a beat later. (The attack
        // task never writes the dispatcher's result word +0x2c, so the "failed" branch 0x598ebb is not taken.)
        // Seen: MP1 beat 3917, army 1's DistributeOrders gives six interceptors (uaa0102, anti-air only) an Attack on
        // the ground point (426.16, 147.69), then an Attack on the blip of army 2's 0x200037 (probe mp1_3917b: the
        // same six IssueAttack calls as ours). The ground attack ends in its first tick (no weapon for it, 0x5f3611);
        // the original's queue stays at two orders through 4060, the blip attack starts at 3919 (Attacking, the
        // target 0x200037 where it stood after 3918). Ours removed the ground order and started the blip attack in
        // 3918 (one order, a beat early: MP1 3918).
        std::shared_ptr<UnitCommand> keep = u->commands.front();
        PopHead(u);
        if (!u->commands.empty()) {
          u->commands.push_back(keep);
          keep->units.insert(u);
        }
        if (!u->dead && !u->commands.empty()) u->headState = kNotStarted;
        break;
      } else {
        PopHead(u);
      }
      if (u->dead || u->commands.empty()) break;
      StartHead(sim, u);
    }
    return;
  }
  // a form move ends when its group (or every group) is at goal: FormationAtGoal / u+0x590
  if (st == kRunning && !u->commands.empty() && HeadTaskType(u) == CommandType::FormMove &&
      FormationHas(*u->commands.front(), u) &&
      (u->formAllAtGoal || FormationGroupAtGoal(sim, *u->commands.front(), u))) {
    PopHead(u);
    u->motion.arrived = u->motion.failed = false;
    SetMoving(u, false);
    // CUnitFormAndMoveTask's destructor 0x6194e0 (read): after clearing Moving (0x619529) and unlinking its listeners
    // it calls the navigator's AbortMove (unit+0x54c, vt+0x10 = 0x5a3750, call at 0x6195c2) whatever the navigator is
    // doing (no command-type table, unlike CUnitMoveTask's 0x618998): a member still rolling toward its own cell when
    // its group is at goal gets SetSpeedThroughGoal(0) and MakeIdle's SetWaypoints(0, 0), a brake straight along its
    // hull (CAiPathSpline::Update mode 3). Seen: MP2 4593, army 1's Striker 0x100090 (FormMove ended, 0.27 per beat,
    // its goal cell three beats ahead) brakes straight to 209.47 255.65 in the original; ours drove on, reached its
    // cell at 4596, where the navigator's Stop dropped the spline, and coasted to 209.53 255.36.
    MotionStop(u);
    if (!u->commands.empty()) StartHead(sim, u);
    return;
  }
  // (No per-beat refresh of "drive through": Unit::UpdateSpeedThroughStatus 0x6ac940 runs on events only (the queue's
  // event 0, the move and patrol tasks' ctors, ...), and a land navigator's SetGoal keeps the steering's request (+0x9d)
  // in between. The refresh that stood here hid two places that overwrote the request: the Move task's listener
  // (RetargetMoveTasks) and Lua's navigator:SetGoal. With both keeping it, the 13 replays, MP2 and MP3 come out the
  // same without it, Skirmish 1225 (army 4's engineer 0x400010) included.)
  // guards and attacks follow a moving target
  if (st == kRunning && !u->commands.empty() && ApproachLike(u->commands.front()->type)) {
    UnitCommand& c = *u->commands.front();
    Entity* t = c.targetId ? sim.FindEntity(c.targetId) : nullptr;
    if (c.targetId && (!t || t->dead || t->destroyQueued)) {
      PopHead(u);
      MotionStop(u);
      StartHead(sim, u);
    }
  }
}
}  // namespace

// The command stage (sim+0x958): every unit's command thread in thread order (Sim::CommandOrder).
// Threads created during the pass run in it, after every older thread.
void CommandStage(Sim& sim) {
  auto step = [&](Unit* u) {
    if (!ArrivalStep(sim, u)) CommandStep(sim, u);
  };
  std::vector<Unit*> order = sim.CommandOrder();
  uint64_t hi = sim.CommandSeqHigh();
  for (Unit* u : order) step(u);
  for (;;) {
    std::vector<Unit*> more;
    for (Unit* u : sim.units())
      if (u->cmdSeq > hi) more.push_back(u);
    if (more.empty()) break;
    std::sort(more.begin(), more.end(), [](const Unit* x, const Unit* y) { return x->cmdSeq < y->cmdSeq; });
    hi = sim.CommandSeqHigh();
    for (Unit* u : more) step(u);
  }
  // path searches, first come first served
  auto& q = sim.pathQueue;
  uint64_t work = 0;
  while (work < kPathWorkPerTick && !q.empty()) {
    Unit* u = q.front();
    q.pop_front();
    if (!IsAlive(u) || u->commands.empty() || u->headState != kWaitingPath) continue;
    sim.navigation().lastWork = 0;
    RunPathSearch(sim, u, false);
    work += sim.navigation().lastWork;
  }
}

namespace {

// ---- argument helpers ------------------------------------------------------------------------

std::vector<Unit*> UnitsArg(lua_State* L, int idx) { return ScriptUnitList(L, idx); }

bool PosArg(lua_State* L, int idx, Vec3* p) {
  if (!lua_istable(L, idx)) return false;
  float v[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    lua_rawgeti(L, idx, i + 1);
    if (!lua_isnumber(L, -1)) {
      lua_pop(L, 1);
      // a Vector (x/y/z keys)
      const char* k[3] = {"x", "y", "z"};
      for (int j = 0; j < 3; ++j) {
        lua_pushstring(L, k[j]);
        lua_gettable(L, idx);
        v[j] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
      }
      *p = {v[0], v[1], v[2]};
      return true;
    }
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  *p = {v[0], v[1], v[2]};
  return true;
}

// Push the Lua handle of a command (a plain table remembering the command's id).
void PushCommand(lua_State* L, const std::shared_ptr<UnitCommand>& c) {
  lua_newtable(L);
  lua_pushstring(L, "moho64_cmd");
  lua_pushnumber(L, c->serial);
  lua_rawset(L, -3);
}

// Commands a factory passes on to what it builds (its rally point, patrols, ...).
bool FactoryCommand(CommandType t) {
  return MoveLike(t) || t == CommandType::Attack || t == CommandType::FormAttack || t == CommandType::Guard ||
         t == CommandType::Ferry;
}

// ProcessUnitCommand (ValidateIssue) 0x6ef9e0, before its type switch: a unit being built (vt+0x34, 0x6efabe) takes
// an order only when it is in category FACTORY (0x6efac9 -> FAF's hook 0x128dae5, IsInCategory 0x67b050) or in the
// category named at 0x12931fc, "CQUEMOV" (the hook's second pass, 0x128daec); any other is refused (0x6eface). Seen:
// Skirmish 2109, M28 gives army 1's land unit 0x100036, begun by its factory this beat, an IssueMove: the original's
// queue stays empty while it is built (2 commands when it stands, at 2140); ours took the Move and had 3.
bool RefusesOrderWhileBuilt(Sim& sim, const Unit* u) {
  return u->beingBuilt && !BpInCategory(sim, u->blueprint, "FACTORY") && !BpInCategory(sim, u->blueprint, "CQUEMOV");
}

// ProcessUnitCommand 0x6efc8e: an order with a target (the target's type +0x10 not 0: an entity, or a point) is
// refused by a unit when the target's position (the entity's +0xac, else the order's point +0x18) is not inside the
// playable rect (STIMap::IsWithin 0x6efcf9, margin 0, the unit's army's whole-map flag): 0x6efe44, the unit does not
// take the order. Every way an order reaches a unit goes through it (UNIT_IssueCommand 0x6f12c0: the command
// stream, the scripts' Issue* functions, the platoons, the factories, the auto-attack).
bool RefusesTargetOutside(Sim& sim, const Unit* u, const Vec3* target) {
  return target && sim.map() && !sim.IsWithin(target->x, target->z, 0.0f, Sim::UseWholeMap(u->army));
}

// ProcessUnitCommand, the Guard case (0x6f08a7: the type switch 0x6efdc2, index type - 2 = 13; IssueGuard, the
// stream's Guard and IssueFactoryAssist come here alike). The unit does not take the order (0x6efe44)
// - when it rides a transport that is no CARRIER and it is no POD (0x6f08a9..0x6f097c);
// - without the Guard command cap (0x6f098b: caps +0x60 bit 8);
// with a unit as the target (0x6f099d, IsUnit):
// - the unit itself (0x6f09b2);
// - both immobile (IUnit vt+0x30) FACTORY units whose buildable categories (bp+0x528, Economy.BuildableCategory) have
//   nothing in common (EntityCategory::Mul 0x5577b0 comes back empty: 0x6f09ba..0x6f0ae7);
// - a FERRYBEACON that is the unit's own ferry beacon (GetTransportFerryBeacon 0x6a8890, 0x6f0af1..0x6f0b60);
// - the unit a FACTORY and the target not (0x6f0b66..0x6f0ba7);
// - the target guarding the unit already (GetGuardedUnit 0x59a430 == the unit, 0x6f0baf): two units never guard
//   each other. (FAF's FactoryUnit.CalculateRollOffPoint walks the guard chain until it ends or meets the factory:
//   a pair guarding each other would hold it in that loop for ever.)
// Without a target unit (0x6f0bcf) only a mobile unit takes it (vt+0x30 IsMobile).
bool RefusesGuard(Sim& sim, lua_State* L, const Unit* u, const Entity* target) {
  if (u->transportedBy) {
    const Entity* t = sim.FindEntity(u->transportedBy);
    const Unit* tu = t && t->kind == Entity::Kind::Unit ? static_cast<const Unit*>(t) : nullptr;
    if (tu && !BpInCategory(sim, tu->blueprint, "CARRIER") && !BpInCategory(sim, u->blueprint, "POD")) return true;
  }
  if (!(LiveCommandCaps(L, u) & 0x8u)) return true;  // RULEUCC_Guard
  const Unit* g = target && target->kind == Entity::Kind::Unit ? static_cast<const Unit*>(target) : nullptr;
  if (!g) return !(u->motion.bp && u->motion.bp->mobile());
  if (g == u) return true;
  auto mobile = [](const Unit* x) { return x->motion.bp && x->motion.bp->mobile(); };
  if (!mobile(g) && !mobile(u) && BpInCategory(sim, g->blueprint, "FACTORY") &&
      BpInCategory(sim, u->blueprint, "FACTORY") && u->bpData && g->bpData) {
    const std::vector<uint64_t>& a = u->bpData->buildable;
    const std::vector<uint64_t>& b = g->bpData->buildable;
    bool meet = false;
    for (size_t w = 0; w < a.size() && w < b.size() && !meet; ++w) meet = (a[w] & b[w]) != 0;
    if (!meet) return true;
  }
  if (BpInCategory(sim, g->blueprint, "FERRYBEACON") && UnitFerryBeacon(sim, u) == g) return true;
  if (BpInCategory(sim, u->blueprint, "FACTORY") && !BpInCategory(sim, g->blueprint, "FACTORY")) return true;
  if (g->guardedId && g->guardedId == EntityRef(u)) return true;
  return false;
}

// ProcessUnitCommand, the Move case (0x6efed4: index 0 of the type switch 0x6efdc2 (table 0x6f128c), taken by Move 2,
// FormMove 4, Patrol 16, FormPatrol 18, AggressiveMove 35 and FormAggressiveMove 36). The unit does not take the order
// (0x6efe44)
// - when it rides a transport that is no CARRIER and it is no POD (GetTransportedBy 0x5e3c30, 0x6efed6..0x6eff87);
// - when it has no motion (IUnit::IsMobile 0x6a4a10, vt+0x30 at 0x6eff8d): a structure (a factory's rally orders go
//   to its own rally queue, Sim::IssueFactoryCommand, not through here);
// - when the order's point has a NaN (IsValid_Vector3f 0x5657f0 on the target's point +0x18, 0x6eff9e..0x6effaf);
// - without a target (the target's type +0x10 is 0, 0x6effb5).
// Seen: MP3 9430, army 1's hydrocarbon plants 0x100056 and 0x1001a7 (xsb1102) in a player's Move for a group: the
// original's queues stay empty; ours queued the Move and dropped it at the next dispatch ("not carried out").
bool MoveCaseType(int t) { return t == 2 || t == 4 || t == 16 || t == 18 || t == 35 || t == 36; }
bool RefusesMoveOrder(Sim& sim, const Unit* u, bool hasTarget, const Vec3* point) {
  if (u->transportedBy) {
    const Entity* t = sim.FindEntity(u->transportedBy);
    const Unit* tu = t && t->kind == Entity::Kind::Unit ? static_cast<const Unit*>(t) : nullptr;
    if (tu && !BpInCategory(sim, tu->blueprint, "CARRIER") && !BpInCategory(sim, u->blueprint, "POD")) return true;
  }
  if (!(u->motion.bp && u->motion.bp->mobile())) return true;
  if (point && !(point->x == point->x && point->y == point->y && point->z == point->z)) return true;
  return !hasTarget;
}

// ProcessUnitCommand, the Repair case (0x6f075e: type 20, index 8 of the type switch 0x6efdc2). The unit does not take
// the order (0x6efe44)
// - without a target (0x6f0760);
// - when it rides a transport and is no POD (GetTransportedBy 0x5e3c30, 0x6f0766..0x6f07d5);
// - when it is not in REPAIR (0x6f07db..0x6f081e, and again with the rules' category 0x6f0862..0x6f088e);
// - when the target's army is its enemy (GetAllianceWith 0x579430 == 2, 0x6f0824..0x6f0846);
// - when the target is the unit itself (0x6f084c..0x6f085c).
// Seen: MP3 3152 and 3155, the player orders army 0's commander, busy reclaiming, to repair itself: the original's
// queue does not take it, and the order's target decode (a unit target's draw) is not made; ours queued it and drew.
bool RefusesRepair(Sim& sim, const Unit* u, const Entity* target) {
  if (!target) return true;
  if (u->transportedBy && !BpInCategory(sim, u->blueprint, "POD")) return true;
  if (!BpInCategory(sim, u->blueprint, "REPAIR")) return true;
  if (target->army && ::moho::combat::IsEnemy(u->army, target->army)) return true;
  return target == u;
}

std::shared_ptr<UnitCommand> Issue(lua_State* L, const std::vector<Unit*>& units, CommandType type,
                                   const Vec3* target = nullptr, const Entity* targetEntity = nullptr,
                                   const std::string* upgradeTo = nullptr) {
  static const long dbgIssue = getenv("MOHO64_DEBUG_ISSUE") ? atol(getenv("MOHO64_DEBUG_ISSUE")) : -1;
  if (dbgIssue >= 0)
    for (Unit* u : units)
      if (static_cast<long>(u->id) == dbgIssue) {
        lua_getglobal(L, "debug");
        lua_pushstring(L, "traceback");
        lua_gettable(L, -2);
        lua_pushstring(L, "");
        lua_call(L, 1, 1);
        Logf(LogLevel::Info, "issue %u type %d unit %u: %s", S(L)->tick(), static_cast<int>(type), u->id,
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 2);
      }
  auto c = std::make_shared<UnitCommand>();
  Sim* sim = S(L);
  c->type = type;
  for (Unit* u : units) {
    // FilterByCommandCap 0x6eecf0 drops immobile factories (their orders go to the rally queue only
    // through IssueFactoryRallyPoint / IssueFactoryCommand, factory_handoff.md 3.2)
    if (FactoryCommand(type) && IsImmobileFactory(*sim, u)) continue;
    if (RefusesOrderWhileBuilt(*sim, u)) continue;
    if (type == CommandType::Guard && RefusesGuard(*sim, L, u, targetEntity)) continue;
    if (type == CommandType::Repair && RefusesRepair(*sim, u, targetEntity)) continue;
    if (RefusesTargetOutside(*sim, u, target)) continue;
    if (MoveCaseType(static_cast<int>(type)) && RefusesMoveOrder(*sim, u, target || targetEntity, target)) continue;
    if (upgradeTo && RefusesUpgradeAgain(*sim, u, *upgradeTo)) continue;
    // IssueToUnits 0x6f12c0 makes the command for the first unit that takes it (0x6f13ca..0x6f13fb): an order no
    // unit takes uses no id of the command database's pool (CommandIdPool)
    if (c->units.empty()) {
      AssignEngineCommandId(*sim, c);
    }
    u->commands.push_back(c);
    c->units.insert(u);
    AirSpeedThroughEvent(*sim, u);  // queue event 0
  }
  return c;
}

// ---- the command stream's orders (engine-ref command_stream.md 5) ----------------------------------------------

// A command's entity target. The player's orders on an enemy name its recon blip (the UI only knows blips), and so
// do scripts that pass a blip (Unit:GetBlip(army), a weapon's GetCurrentTarget): SCR_GetTarget 0x6eef60 takes any
// entity (SCR_FromLuaNoError_Entity 0x5e3800) and CAiTarget::UpdateTarget 0x5d55b0 keeps it as given, so the
// original's order holds the blip either way. Our tasks and weapons work on units, so a real blip stands for its
// unit, and a jammer's fake blip for its unit with the fake named beside it (the target then stands where the fake
// stands and lasts as long as it).
void SetCommandEntityTarget(UnitCommand& c, Entity* e) {
  c.targetViaBlip = false;
  c.targetFakeId = 0;
  c.targetBlipRef = 0;
  Vec3 at = e->position;
  if (e->kind == Entity::Kind::Blip) {
    auto* b = static_cast<ReconBlip*>(e);
    if (b->source && !b->source->destroyQueued) {
      if (b->fake) c.targetFakeId = EntityRef(b);
      c.targetBlipRef = EntityRef(b);
      e = b->source;
      c.targetViaBlip = true;
      if (!c.targetFakeId) at = e->position;
    }
  }
  c.targetId = EntityRef(e);
  c.pos = at;
  c.hasPos = true;
}

}  // namespace

void SetCommandTargetFromStream(Sim& sim, UnitCommand& c, const StreamTarget& t) {
  static const bool dbgBlip = getenv("MOHO64_DEBUG_BLIP") != nullptr;  // (read once)
  c.targetId = 0;
  c.targetPoint = -1;
  c.targetViaBlip = false;
  c.targetFakeId = 0;
  c.targetBlipRef = 0;
  if (t.type == 1) {
    if (Entity* e = sim.FindEntityById(t.entity)) {
      c.targetPoint = TargetPointDraw(sim, e);  // CAiTarget::UpdateTarget: a unit / unit blip draws
      SetCommandEntityTarget(c, e);
    } else if (dbgBlip) {
      Logf(LogLevel::Info, "command %u: target entity %#x does not exist", sim.tick(), t.entity);
    }
  } else if (t.type == 2) {
    c.pos = t.pos;
    c.hasPos = true;
  }
  // CUnitCommand::SetTarget 0x6e8820 writes the new target into cmd+0x9c..+0xac too (0x6e883a: 0x5e27d0 from the
  // CAiTarget, 0x6e884d..0x6e8875), so a dragged order is compared as it now stands
  c.issueTargetType = t.type;
  c.issueTargetEntity = t.entity;
  c.issueTargetPos = t.pos;
}

bool RemoveCommandFromUnit(Sim& sim, Unit* u, UnitCommand& c) {
  auto& q = u->commands;
  size_t i = 0;
  while (i < q.size() && q[i].get() != &c) ++i;
  if (i == q.size()) return false;
  std::shared_ptr<UnitCommand> keep = q[i];
  if (i == 0) {  // the executing head: its task is interrupted (destructor abort -> AbortMove)
    const bool stopped = HeadTaskStopsMotion(u);  // a pod's task: its dtor's Stop is the last word
    const bool asleep = ThreadAsleepOnMove(u);    // (asked before the teardown changes the head)
    // A head not dispatched yet has no task: event 3's interrupt (IAiCommandDispatchImpl::OnEvent 0x599030 ->
    // 0x408d70) tears nothing down, so nothing aborts the move (as the clear of an undispatched head, IssueFromStream).
    // Seen: MP1 15137, army 1's player gives five ships a Move with clear (0x01000044) and takes it off their queues
    // in the same message batch; the Move it replaced kept its move (CUnitMoveTask dtor, a Move for a Move): the
    // original's ships drive on at full speed, speed-through 1, an empty queue; ours stopped them (braking from 15138).
    const bool undispatched = u->headState == kNotStarted && !u->task;
    EndMoveTaskRect(u);
    if (u->engageId) ClearEngagement(sim, u);
    if (u->task) {
      EndBuildTask(sim, u, *u->task, false);
      u->task = nullptr;
    }
    q.pop_front();
    ReleaseCommandImpl(u, c);
    u->headState = kNotStarted;
    auto& pq = sim.pathQueue;
    pq.erase(std::remove(pq.begin(), pq.end(), u), pq.end());
    if (!stopped && !undispatched) MotionStop(u);
    if (!undispatched) SetMoving(u, false);
    // CUnitCommandQueue::RemoveCommandFromQueue 0x6edef0, index 0: the queue tells its listeners event 3
    // (0x6edf11..0x6edf1c) before the command goes; IAiCommandDispatchImpl::OnEvent 0x599030 takes events 2 and 3:
    // the thread's frame count to 0 and, when it is suspended (a move task's ctor), re-linked at the stage's end
    // (0x599050..0x599083), then the tasks above it are interrupted (0x408d70). A later order's removal (index > 0)
    // sends event 1 only, which the dispatcher ignores. Seen: MP1 beat 4043, army 5's player takes the Guard off the
    // heads of three UEF engineers whose guard walks were under way (0x500018, 0x500035, 0x50003c); in the original
    // their threads then run after army 2's Cybran engineers (last re-linked 3743-3752), so at 4049 the three Cybran
    // engineers' OnStartBuild forks (cconstructionunit.lua:94) come before the being-built effects of 0x50003c's new
    // structure 0x50005d (effectutilitiesuef.lua:163): their target-point draws and Lua Random before its
    // projectiles'. Ours kept 0x50003c where it was linked at 3436 (the random stream went apart at draw 240733).
    if (asleep) sim.ResumeCommandThread(u);
  } else {
    q.erase(q.begin() + static_cast<long>(i));
    ReleaseCommandImpl(u, c);
  }
  return true;
}

namespace {
// CUnitCommand::CoordinateWith 0x6e9000 (ecx: other, edx: self): self's list takes other (a WeakPtr, 0x6e9680), when
// the two are of one type ([cmd+0x98], 0x6e9015..0x6e9025)
void CoordinateWith(UnitCommand& self, UnitCommand& other) {
  if (self.type != other.type) return;
  self.coordinatedWith.push_back(other.weak_from_this());
}

// CommandDatabase::AddIssueData: the CUnitCommand of a stream message (one per message, made for the first unit
// that takes it)
std::shared_ptr<UnitCommand> MakeStreamCommand(Sim& sim, const StreamIssueData& d) {
  auto c = std::make_shared<UnitCommand>();
  if ((d.id >> 24) == 0xffu) {  // AddIssueData 0x6e0db0: an engine-made order (the CPlatoon movers) gets a sim id
    AssignEngineCommandId(sim, c);
  } else {
    c->id = d.id;
    c->fromStream = true;
  }
  c->type = static_cast<CommandType>(d.type);
  c->blueprintId = d.blueprint;
  for (auto& ch : c->blueprintId) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  c->count = d.count;
  c->maxCount = d.maxCount;
  c->cells = d.cells;
  c->formIndex = d.formIndex;
  if (d.formIndex != -1) {
    c->formQw = d.qw;
    c->formQx = d.qx;
    c->formQy = d.qy;
    c->formQz = d.qz;
    c->formScale = d.scale;
  }
  SetCommandTargetFromStream(sim, *c, d.target);  // the ctor's decode (the only draw on this path)
  c->issueTargetType = d.target.type;
  c->issueTargetEntity = d.target.entity;
  c->issueTargetPos = d.target.pos;
  // The order's Lua value: a Script order (type 28) carries its task's table there ({TaskName = 'EnhanceTask',
  // Enhancement = ...}, FAF's {TaskName = 'AttackMove', Location = ...}). It was read off the stream and then
  // dropped, so every scripted order of a player ended in the bare ScriptTask ("Did you forget to add TaskName
  // to your command lua param table?"): no enhancement was ever built from an order.
  if (!d.lua.empty()) {
    lua_State* L = sim.L();
    const int top = lua_gettop(L);
    if (PushSerializedLua(L, d.lua) && lua_istable(L, -1)) c->scriptRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
  }
  // A coordinated order (the ctor, 0x6e840d..0x6e84c2): the command the issue data names (+4, -1: none) is looked up
  // in the command database (0x6e1940); when there is one, the new command is linked with each live command of a copy
  // of its list (0x6e7d40), both ways, and then with it, both ways (CoordinateWith checks the types).
  if (d.coordWith != -1)
    if (std::shared_ptr<UnitCommand> o = StreamCommand(sim, static_cast<uint32_t>(d.coordWith))) {
      const std::vector<std::weak_ptr<UnitCommand>> others = o->coordinatedWith;
      for (const auto& w : others)
        if (std::shared_ptr<UnitCommand> x = w.lock()) {
          CoordinateWith(*c, *x);
          CoordinateWith(*x, *c);
        }
      CoordinateWith(*c, *o);
      CoordinateWith(*o, *c);
    }
  if (c->fromStream) sim.streamCommands[d.id] = c;
  return c;
}
}  // namespace

// UNIT_IssueFactoryCommand 0x6f14d0 for a stream order (command_stream.md 5.5): the command goes to the rally
// queue of every factory among the units (the orders its products inherit), not to the factories' own queues.
// (Not built: the army's +0x1c4..+0x1d8 distance test on the target, meaning not traced.)
std::shared_ptr<UnitCommand> IssueFactoryFromStream(Sim& sim, const std::vector<Unit*>& units,
                                                    const StreamIssueData& d, bool clear) {
  std::shared_ptr<UnitCommand> c;
  for (Unit* u : units) {
    if (u->dead || u->transportedBy) continue;
    if (!IsFactoryBuilder(sim, u)) continue;
    if (!c) {
      c = MakeStreamCommand(sim, d);
      c->factoryIssued = true;  // cmd+0x142
    }
    if (clear) ClearFactoryCommandQueue(u);
    AddFactoryCommand(u, c);
  }
  return c;
}

std::shared_ptr<UnitCommand> IssueFromStream(Sim& sim, const std::vector<Unit*>& given, const StreamIssueData& d,
                                             bool clear) {
  // 0x6ef660 (0x6f1315), before anything else: a TransportReverseLoadUnits goes to the transport chosen for its target
  // and to the target, not to the units it was given (transport.cpp ReverseLoadIssueSet). Every test below sees that
  // set, ProcessUnitCommand's "units of this message" included (it gets a copy of it, 0x6f137b).
  const bool reverseLoad = d.type == static_cast<int>(CommandType::TransportReverseLoadUnits);
  Entity* const reverseTarget = reverseLoad ? sim.FindEntityById(d.target.entity) : nullptr;
  const std::vector<Unit*> reverseSet = reverseLoad ? ReverseLoadIssueSet(sim, given, reverseTarget) : std::vector<Unit*>{};
  const std::vector<Unit*>& units = reverseLoad ? reverseSet : given;
  // types whose move-task destructor does not abort the move when the next command is one of them (0x618998). The
  // table covers types 0..31 only: a type above 31 takes the same branch as the table's "keep" entries
  // (0x6187f4 cmp eax, 0x1f; ja 0x618807), i.e. Sacrifice 32 .. Dock 39 (AggressiveMove 35 among them) keep the move
  // unless a second queued command says otherwise. Seen: MP2 2198, army 1's two Strikers on a Move get an
  // AggressiveMove with clear: the original's tanks go on along their spline at full speed through 2200; ours made a
  // brake spline and jumped.
  auto keepsMove = [](int t) {
    if (t > 0x1f) return true;
    switch (t) {
      case 2: case 4: case 5: case 6: case 7: case 9: case 10: case 11: case 15: case 16: case 17: case 18:
      case 20: case 22: case 23: case 24: case 25: case 28: case 29:
        return true;
      default:
        return false;
    }
  };
  // ProcessUnitCommand 0x6efbb4, the "same order again" test (types whose byte in the table 0x6f1218 is 0): with
  // clear, a unit whose queue holds exactly ONE command, of this type, on the same target entity id (0x6efe1b) or
  // within 0.001 of the same point (0x6efc19), held by exactly the units of this message (0x6eec40), does not get
  // the new command: its queue is not cleared and it carries on (a player clicking the same attack twice).
  auto sameOrderAgain = [&](const Unit* u) {
    switch (d.type) {
      case 2: case 4: case 10: case 11: case 16: case 18: case 19: case 20: case 21: case 22: case 23: case 27:
      case 32: case 35: case 36: case 39:
        break;
      default:
        return false;
    }
    if (!clear || u->commands.size() != 1) return false;
    const UnitCommand& f = *u->commands[0];
    if (static_cast<int>(f.type) != d.type) return false;
    if (d.target.type == 1) {
      if (f.issueTargetEntity != d.target.entity) return false;
    } else if (d.target.type == 2) {
      const float dx = d.target.pos.x - f.issueTargetPos.x, dy = d.target.pos.y - f.issueTargetPos.y,
                  dz = d.target.pos.z - f.issueTargetPos.z;
      if (!(0.001f > std::sqrt(dx * dx + dy * dy + dz * dz))) return false;
    } else {
      return false;
    }
    if (f.units.size() != units.size()) return false;
    for (Unit* o : f.units)
      if (std::find(units.begin(), units.end(), o) == units.end()) return false;
    return true;
  };
  std::shared_ptr<UnitCommand> c;
  // the target's position for the playable-rect test (RefusesTargetOutside): the entity's, or the order's point
  Vec3 targetAt{};
  const Vec3* target = nullptr;
  const Entity* targetEntity = nullptr;
  if (d.target.type == 1) {
    const Entity* e = sim.FindEntityById(d.target.entity);
    targetEntity = e;
    targetAt = e ? e->position : d.target.pos;
    target = &targetAt;
  } else if (d.target.type == 2) {
    targetAt = d.target.pos;
    target = &targetAt;
  }
  for (Unit* u : units) {
    // ProcessUnitCommand 0x6ef9e0 (outline only, command_stream.md 5.2): dead units refuse
    if (!IsAlive(u)) continue;
    if (RefusesOrderWhileBuilt(sim, u)) continue;  // (the being-built rule, above)
    if (sameOrderAgain(u)) continue;
    if (d.type == static_cast<int>(CommandType::Guard) && RefusesGuard(sim, sim.L(), u, targetEntity)) continue;
    if (d.type == static_cast<int>(CommandType::Repair) && RefusesRepair(sim, u, targetEntity)) continue;
    if (RefusesTargetOutside(sim, u, target)) continue;
    if (MoveCaseType(d.type) && RefusesMoveOrder(sim, u, d.target.type != 0, &d.target.pos)) continue;
    if (d.type == static_cast<int>(CommandType::Upgrade) && RefusesUpgradeAgain(sim, u, d.blueprint)) continue;
    if (reverseLoad && RefusesReverseLoad(sim, u, units, reverseTarget)) continue;  // (its case, 0x6f0f53)
    if (d.type == static_cast<int>(CommandType::TransportLoadUnits) && RefusesLoadOrder(sim, u, targetEntity))
      continue;  // (its case, 0x6f0bf3)
    if (!clear && u->commands.size() > 500) continue;
    int at = -1;  // insert-before: the queued command with that stream id
    if (d.insertBefore != -1) {
      for (size_t i = 0; i < u->commands.size(); ++i)
        if (u->commands[i]->fromStream && u->commands[i]->id == static_cast<uint32_t>(d.insertBefore)) {
          at = static_cast<int>(i);
          break;
        }
    }
    if (!c) c = MakeStreamCommand(sim, d);  // CommandDatabase::AddIssueData: one CUnitCommand per message
    {
      static const bool dbg = getenv("MOHO64_DEBUG_TASKS") != nullptr;
      if (dbg)
        Logf(LogLevel::Info, "order after tick %u: type %d for unit %u, clear %d, its queue held %zu", sim.tick(), d.type, u->id,
             clear ? 1 : 0, u->commands.size());
    }
    if (clear) {  // queue+0x1c = type; ClearCommandQueue 0x6ee2d0
      // CUnitMoveTask's destructor 0x618700 (read again 2026-10-11): queue+0x1c holds the NEW command's type while
      // the queue is cleared for it. A type outside the table 0x618998 aborts the move; one inside it aborts only
      // when a SECOND queued command exists and is itself outside the table (0x618807..0x618844). A lone Move
      // replaced by another Move is therefore not aborted: the navigator keeps what it was doing until the new
      // task sets its goal (seen: a tank ordered at 68, 70 and 71 takes its first step at 71, not at 74).
      const int second = u->commands.size() > 1 ? static_cast<int>(u->commands[1]->type) : -1;
      bool keep = keepsMove(d.type) && IsAlive(u) && !(second >= 0 && second <= 0x1f && !keepsMove(second));
      // That table is CUnitMoveTask's. A FormMove dispatched with a formation runs a CUnitFormAndMoveTask instead
      // (DispatchTask 0x608ef0, type 4 at 0x609335: GetFormation 0x6e8a00, then 0x619a90; without one a plain
      // NewMoveTask 0x6190a0), whose destructor 0x6194e0 clears Moving (0x619529) and calls the navigator's AbortMove
      // (unit+0x54c vt+0x10, 0x6195c2) whatever the new command is. Seen: MP2 6437, army 1's 28 units on a FormMove
      // (formation speed 1.19) get a Move with clear from the stream: the original's brake spline is made in the
      // message (SetWaypoints(0, 0) hands out its first point), the steering of 6438 hands out the second, so the
      // motion takes two brake steps at once (0.145) and then 0.026, before the new path's batch; ours kept the old
      // batch at 0.119 per beat.
      const bool formTask = !u->commands.empty() && u->headState != kNotStarted &&
                            HeadTaskType(u) == CommandType::FormMove && FormationHas(*u->commands.front(), u);
      if (formTask) keep = false;
      // the stop comes from the running command's task being torn down (its navigator's AbortMove): a unit with an
      // empty queue has none, so nothing touches its motion (seen in the original: an idle CZAR's target layer
      // stays 0 when its first order arrives; CUnitMotion::Stop 0x6b8460 would have made it Air)
      const bool hadCommands = !u->commands.empty();
      // A unit's command thread runs its dispatcher every beat while the queue is empty (TaskTick 0x598e80 answers
      // 1), so an idle unit keeps its place in the stage's run list. A dispatched Move task sleeps on the
      // navigator's event, off the list; clearing the queue destroys its command, which wakes it, and a woken
      // thread is linked in at the list's end. So when one order reaches idle units and units under way, the
      // idle ones are dispatched first, whatever their ids. Seen: 28 ships ordered to one place, each reserving
      // its spot at dispatch (formation AdjustPosition); the ones already moving came last and had to give way.
      const bool moveAsleep = hadCommands && u->headState != kNotStarted && HeadTaskType(u) == CommandType::Move;
      // (a task's walking child too, a guard's child's included. Seen: Economy 3965, SACUs 0x26 (a build walking) and
      // 0x27 (guarding 0x26, its child walking) get a Reclaim with clear: both threads wake and go to the run list's end
      // in the message's unit order, so 0x26 runs before 0x27 from then on; at 4483 the second of the two reclaimers of
      // 0x40 takes its last step and gets the wreck: 0x27 in the original. Ours had 0x27 first (its last re-link 3263,
      // 0x26's 3886).)
      const bool asleep = ThreadAsleepOnMove(u);
      u->clearKeepsMove = keep;
      const bool stopped = HeadTaskStopsMotion(u);  // a pod's task: its dtor's Stop is the last word
      // A head command not dispatched yet has no task, so the clear tears nothing down and nothing aborts the move
      // (as IssueClearCommandsTo, below); and a patrol or attack-move under way makes the only abort itself, in its
      // dtor, and only when the unit moved this beat (CUnitPatrolTask dtor 0x61b1bd, EndPatrol). Seen: MP3 7779, army
      // 0's engineer 0xd0 ends a patrol leg at 7778 (its dtor's abort: a brake spline, first point handed out); the
      // next leg is not dispatched yet when the stream's BuildMobile with clear arrives, so the original's brake goes
      // on (7779 at its second point); ours braked again from where the unit stood, handed out that brake's first
      // point in the message and the second in the beat's steering, two steps at once.
      const bool undispatched = hadCommands && u->headState == kNotStarted;
      const bool patrolSelf = PatrolTaskDecidesAbort(u);
      ForgetUnitCommands(u);
      u->clearKeepsMove = false;
      // CUnitMoveTask's destructor takes the unit out of state Moving FIRST (0x6187bd: bit 2 of unit+0x4a0) and
      // aborts the move after it (0x618884). The abort's CheckCollisions therefore sees this unit as standing:
      // "a standing unit has priority over a mover". When one Stop reaches two neighbours, the first one torn
      // down puts its record on the neighbour that still moves; the neighbour's own teardown wipes that record
      // and, both standing now, the formation rule decides: one of the two brakes hard, not both.
      if (moveAsleep || formTask) SetMoving(u, false);
      if (!keep && hadCommands && !stopped && !undispatched && !patrolSelf) MotionStop(u);
      if (asleep) sim.ResumeCommandThread(u);
      SetMoving(u, false);
      at = -1;
    }
    if (d.insertBefore != -1 && at < 0) continue;  // not in this unit's queue: it does not get the command
    if (at < 0) {
      u->commands.push_back(c);
    } else {
      // CUnitCommandQueue::InsertCommandToQueue 0x6edcb0 (read): the command goes into the queue at that place and the
      // queue tells its listeners event 0 (0x6edd19..0x6edd21), which the dispatcher takes for its speed-through
      // refresh only (OnEvent 0x599030 -> 0x6ac940): nothing is interrupted, also when the place is the head's. The
      // running task goes on, and when it ends the dispatcher takes the queue's first command, now the inserted one
      // (0x598e95: [queue+0x10][0]). Seen: MP1 beat 4633, army 4's player puts an xsa0104 order before the head
      // (0x0400001c) of the air factory 0x400056, which is building the xsa0104 0x400071 (29 %): the original's
      // factory goes on with 0x400071; ours tore the build down and began a new one (0x400072).
      u->commands.insert(u->commands.begin() + at, c);
    }
    c->units.insert(u);
    AirSpeedThroughEvent(sim, u);  // queue event 0
  }
  return c;
}

// IssueClearCommands 0x6f1950: ClearCommandQueue 0x6ee2d0 and the attacker's reset. The motion is stopped by the
// teardown of the running command's task (a move task's destructor 0x618700 -> the navigator's AbortMove). Ours stops
// every cleared unit here, which also stands in for task teardowns that end a command just before (bench 294: the
// Cybran ACU's drones, pods whose assist ended, cleared by their IdleState, are stopped in the original); but an
// aircraft that is no pod, with an empty queue and its motion already stopped (CUnitMotion::Stop 0x6b8460 ran:
// motion+0x10), is not stopped again: nothing in it is torn down (inferred from the two seen cases). Seen: Skirmish 1395, M28 clears the idle Aeon transport 0x600018
// (its first load done at 1394, its landing move aborted at 1383) before its next IssueTransportLoad: the original's
// air target stays where that abort left it (907.168, 30.716, 70.798; target layer Land, elevation 4), ours was
// stopped again one second ahead and took the Air layer.
// Nor is an aircraft whose head command has no task yet: nothing is torn down then either. Seen: Skirmish 1615, the
// air unit 0x20 let go by its factory in this beat's Lua (FinishBuildThread DetachFrom: the target one unit behind the
// factory's back, layer 0, CUnitMotion::NotifyDetached 0x6b9570) still has its inherited rally Move undispatched (an
// attached unit's dispatcher waits, 0x598f91); M28 clears and re-orders it the same beat (m28air.lua 11759). The
// original's target stays (682.5, 324.5), layer 0; ours was stopped where it was and took the Air layer.
// That holds for a pod too (no exception: IssueClearCommands 0x6f1950 is ClearCommandQueue 0x6ee2d0 and
// CAiAttackerImpl::Stop only, and an undispatched head has no task to tear down). Seen (MP1 1063): army 2's Cybran
// build drone 0x200002 ends its AssistCommander task in the command stage (Kill: a Stop where it is); in the Lua stage
// SpawnBuildBots clears it and gives it a Guard, and a second IssueClearCommands (effectutilitiesopti.lua 125)
// finds that Guard not yet dispatched: the original's target stays the drone's place; ours stopped it again a second
// ahead. (The pod exception had been inferred for bench 294's drones, which EndGuard now stops.)
// The tasks go top down (CTask::TaskInterruptSubtasks 0x408d70): a load's landing move (its child CUnitMoveTask)
// is torn down, and aborts its navigator (dtor 0x618700 -> AbortMove 0x5a4f00 -> CUnitMotion::Stop 0x6b8460), BEFORE
// the load's own dtor 0x624cc0 clears TransportLoading (transport_tasks.md 3.4): so the Stop still finds the
// transport loading and keeps its target layer (0x6b846f). Seen: Skirmish 1615, M28 clears army 2's UEF transport
// 0x20001e hovering over its pickup point (m28orders.lua 1166) before its IssueTransportUnload: the original's
// target is the point a second ahead on the Land layer, at elevation 4 (TransportHoverHeight); ours stopped it
// after the load had ended, on the Air layer at elevation 8.
// Nor is a land unit with an empty queue and no goal for its navigator: there is no task to tear down, so nothing
// aborts its move (no AbortMove, no new brake spline for its steering, SetWaypoints(0, 0) 0x5d29c0). Seen: Skirmish
// 1650, the Aeon ACU 0x300000 coasting to a stop after its reclaim target went (its Reclaim popped this beat); M28
// re-issues a Reclaim (IssueClearCommands + IssueReclaim, m28engineer.lua 6504). The original's brake goes on
// through 1650 and is made again by the new task at 1651; ours made one at 1650 too (one beat of less braking).
// The same for an aircraft that is no pod (this takes in the rule above for a STOPPED one, which was too narrow).
// Seen: Skirmish 1695, army 2's UEF transport 0x20001e, its unload done at 1694 (the unload's last step aims it at
// its own place on the Air layer, CUnitUnloadUnits state 3, not a Stop), is cleared and given a Move by M28
// (m28air.lua 11948): the original's target stays its place of 1693; ours was stopped one second ahead.
// An empty queue tears nothing down even while the navigator has a goal that no command gave (SIM_TryToBuild
// 0x62dd40 sends a standing unit away with the navigator's SetGoal, no command, no task). Seen: Skirmish 1809, army
// 0's engineer 0x29, moved aside for a structure at 1806 (goal (674.5, 340.5)), is cleared and given a BuildMobile
// by M28 (m28engineer.lua 10713): the original goes on toward that goal until the build's walk at 1810; ours
// stopped it.
// Nor is a transport whose load waits for its cargo with no landing move of its own running: the load's dtor 0x624cc0
// does nothing to the transport's motion (transport_tasks.md 3.4). Seen: Skirmish 1755, army 5's transport 0x50001b,
// landed at its pickup point (its landing move ended and aborted at 1743), is cleared by M28 before its unload
// (m28orders.lua 1166): the original stays on the ground (layer Land, target where the abort left it) until the
// unload's own AbortMove at 1756; ours was stopped again, now not loading: the Air layer, and it took off.
void IssueClearCommandsTo(Sim& sim, Unit* u, bool stopAttacker) {
  const bool moveAsleep = ThreadAsleepOnMove(u);  // (a task's walking child too)
  // An empty queue tears nothing down, a pod's included. (Bench 294's drones, whose guard of the finished structure
  // ended in the command stage, are stopped by that guard's dtor 0x611850, EndGuard: its AbortMove has no goal test.)
  // Seen (MP3 488): Cybran build drones whose AssistCommander task ended in the command stage (its dtor's Kill: a
  // Stop where the drone is) are cleared by SpawnBuildBots in the Lua stage; the original's target stays the drone's
  // place, ours was stopped again one second ahead.
  const bool idle = u->commands.empty();
  const bool stopped = HeadTaskStopsMotion(u);
  // A head command not dispatched yet has no task, so nothing is torn down and nothing aborts the move, for a land
  // unit as for an aircraft: its navigator keeps the goal an earlier task left it. Seen: MP2 5684, army 1's 21 Strikers
  // on a Move get a formation Move with clear from the stream (the old Move's dtor keeps the move: a Move replaces it,
  // 0x618998), then FAF's DistributeOrders callback clears them (IssueClearCommands) and re-issues the orders: the
  // original's tanks drive on and turn onto the new paths; ours made brake splines and lost one or two beats.
  const bool undispatched = !u->commands.empty() && u->headState == kNotStarted;
  const bool loadMove = u->motion.air && !u->commands.empty() && u->headState == kRunning && TransportLoadMoving(u);
  const bool loadWaits = u->motion.air && !u->commands.empty() && u->headState == kRunning && TransportLoadWaiting(u);
  // Nor is a guard that steers itself (its follow goal handed to the navigator directly, no move task): its dtor
  // 0x611850 aborts the move only when the unit moved this beat (unit+0xb4 != +0xd0; EndGuard does that). Seen:
  // Skirmish 2047, army 2's engineer 0x20000a, its guard following toward (863.5, 433.5) since 2046 and not moved yet,
  // is cleared by M28: the original's navigator goes on (target (847.5, 424.5) at 2048) until the Guard M28 gives it
  // at 2050; ours stopped it. (Land units only: a guarding aircraft that flies has moved every beat, and its abort is
  // still made here. Seen: bench 251, aircraft guarding cleared in flight are stopped in the original.)
  const bool guardSelf = !u->motion.air && !u->commands.empty() && u->headState == kRunning && GuardSteersItself(u);
  // Nor is a patrol or attack-move under way (CUnitPatrolTask): its dtor 0x61b140 is the only abort, and only when the
  // unit moved this beat (0x61b1bd: unit+0xb4 != +0xd0, then vt+0x10 AbortMove at 0x61b1cd; EndPatrol does that). Seen:
  // MP2 6805, army 1's 36 units, standing since the stream's AggressiveMove of 6803 (dispatched 6804, their paths not
  // yet laid), are cleared and given the same AggressiveMove again by FAF's DistributeOrders: the original's
  // navigators go on (inferred: the new task's SetGoal finds the same goal, 0x5a3f37) and the units drive off at
  // 6806 with speed-through 1; ours stopped them and they started at 6808. (Land units, as for the guard above.)
  const bool patrolSelf = PatrolTaskDecidesAbort(u);
  // Nor is a mobile builder's own task: CUnitMobileBuildTask (dtor 0x5f6ac0), CUnitRepairTask (0x5f8e20),
  // CUnitReclaimTask (0x620280), CUnitCaptureTask (0x6050f0). Read: their dtors and what they call (RunScript 0x581aa0,
  // CBuildTaskHelper::OnStopBuild 0x5f5a20 / dtor 0x5f5790, Unit::FreeOgridRect 0x6ab760, CUnitCaptureTask::DoCallback
  // 0x604e10, 0x620110, ~CCommandTask 0x608e90, unit-state / script vt calls) reach neither the navigator (unit+0x54c)
  // nor CUnitMotion::Stop 0x6b8460. Only their walking child's CUnitMoveTask dtor aborts the move, and that abort is
  // counted below (stopCount) when the teardown makes it. (Keyed by the task's own kind, BuildTask::type, the type
  // the head was dispatched with. Every other kind keeps the stop here: a Move or FormMove head has no task of ours
  // and is unaffected; Upgrade, BuildFactory, Teleport, Sacrifice, Script, FireAt, OverCharge, Attack and the transport
  // and guard tasks keep their paths.) Seen: MP1 6966, army 4's Seraphim engineer 0x400013 (in the water), still
  // braking from the stream's BuildMobile with clear of 6963 (dispatched 6964 in build range: no walk), is cleared by
  // FAF's DistributeOrders (IssueClearCommands + IssueBuildMobile, distribute-queue.lua 148 / 257): the original's
  // brake goes on (velocity 0.1520 -> 0.1216 at 6966); ours made a new brake spline and took two of its steps at once.
  const BuildTask* ht = !u->commands.empty() && u->headState == kRunning ? u->task : nullptr;
  const bool buildSelf = ht && !ht->gdata && !ht->adata && !ht->pdata && !ht->tdata &&
                         (ht->type == CommandType::BuildMobile || ht->type == CommandType::Repair ||
                          ht->type == CommandType::Reclaim || ht->type == CommandType::Capture);
  // CUnitMoveTask's dtor clears state Moving (0x6187bd) BEFORE its AbortMove, as in the stream's clear above: the
  // abort's CheckCollisions sees this unit standing ("a standing unit has priority over a mover"). Seen: Skirmish
  // 1890, M28 clears army 6's engineer 0x60001b walking beside the engineer 0x600024 (IssueClearCommands +
  // IssueReclaim): the original puts the record on 0x600024, which side-steps at 1891 while 0x60001b brakes hard;
  // ours, still Moving at the abort, lost the right of way and 0x600024 braked.
  if (moveAsleep) SetMoving(u, false);
  if (loadMove) MotionStop(u);  // (the child move first)
  // The abort below stands for the head task's own (a Move task's dtor). When the teardown has made one already (a
  // repair's or reclaim's walking child: CUnitMoveTask::~CUnitMoveTask 0x618886 -> AbortMove), there is no second:
  // the probe saw one AbortMove -> NavigatorMakeIdle -> SetWaypoints(0, 0) -> Update(3) chain under
  // IssueClearCommands (0x6f19eb) for Skirmish 2949's engineer 0x2f, cleared on its Repair.
  // Nor is a cargo's call for its transport: CUnitCallTransport's dtor 0x5ff7f0..0x5ff961 (the two state bits, the
  // beam-up's OnStopTransportBeamUp and NotifyDetached 0x6b9570, the transport's RemovePickupUnit through vt+0x14)
  // reaches neither the navigator nor CUnitMotion::Stop; only its walk's CUnitMoveTask dtor aborts (EndCall, counted
  // by stopCount). Seen: Skirmish 2946, army 1's hover artillery 0x100049 (xsl0103), braking since M28 cleared its
  // Move at 2945 and gave it a TransportLoadUnits, is cleared and given it again (the call dispatched at 2946, no walk
  // yet): the original's brake goes on (q.w 0.0916819796 at 2946), ours made a new one (0.0913212597) and its tilt
  // and place were off from there (58 Skirmish ww lines).
  const bool callSelf = !u->commands.empty() && u->headState == kRunning && TransportCallTask(u);
  const uint32_t stops = u->motion.stopCount;
  ForgetUnitCommands(u);
  if (!stopped && !idle && !undispatched && !loadMove && !loadWaits && !guardSelf && !patrolSelf && !buildSelf &&
      !callSelf && u->motion.stopCount == stops)
    MotionStop(u);
  SetMoving(u, false);
  if (moveAsleep) sim.ResumeCommandThread(u);
  if (stopAttacker) AttackerStop(sim, u);
}

// The acquire task's auto-initiated Attack (0x5d9415: UNIT_IssueCommand(sim, {u}, Attack on the entity, clear)):
// what is left in the queue goes, the command ctor draws the target point.
std::shared_ptr<UnitCommand> IssueAutoAttack(Sim& sim, Unit* u, Entity* target, bool viaBlip, bool pastDeath,
                                             uint32_t fakeId) {
  if (!IsAlive(u) || !target) return nullptr;
  {
    const Entity* f = viaBlip && fakeId ? sim.FindEntity(fakeId) : nullptr;
    const Vec3 at = f ? f->position : target->position;
    if (RefusesTargetOutside(sim, u, &at)) return nullptr;
  }
  auto c = std::make_shared<UnitCommand>();
  AssignEngineCommandId(sim, c);
  c->type = CommandType::Attack;
  c->targetId = EntityRef(target);
  c->targetViaBlip = viaBlip;  // (what the weapon found was a jammer's blip: the order names the blip's id)
  c->targetPastDeath = pastDeath;
  c->targetFakeId = viaBlip ? fakeId : 0;
  const Entity* fake = c->targetFakeId ? sim.FindEntity(c->targetFakeId) : nullptr;
  c->pos = fake ? fake->position : target->position;
  c->hasPos = true;
  c->targetPoint = TargetPointDraw(sim, target);
  c->issueTargetType = 1;
  c->issueTargetEntity = target->id;
  static const bool dbgAutoAttack = getenv("MOHO64_DEBUG_AUTOATTACK") != nullptr;  // (read once)
  if (dbgAutoAttack)
    Logf(LogLevel::Info, "autoattack %u unit %u -> %u target point %d", sim.tick(), u->id, target->id, c->targetPoint);
  if (!u->commands.empty()) {
    ForgetUnitCommands(u);
    SetMoving(u, false);
  }
  u->commands.push_back(c);
  c->units.insert(u);
  AirSpeedThroughEvent(sim, u);  // queue event 0
  return c;
}

std::shared_ptr<UnitCommand> IssuePodAssist(Sim& sim, Unit* u) {
  if (!u || u->dead) return nullptr;  // (a destroy-queued pod too: Unit::MotionTick 0x6a921a)
  auto c = std::make_shared<UnitCommand>();
  AssignEngineCommandId(sim, c);
  c->type = CommandType::AssistCommander;
  u->commands.push_back(c);
  c->units.insert(u);
  AirSpeedThroughEvent(sim, u);  // queue event 0
  return c;
}

// A land navigator's AbortMove 0x5a3750 called on a unit from outside its own move task (Lua's navigator:AbortMove,
// a transport's load ending, a ferry calling its cargo): the stop, and the event its move task listens to.
void NavigatorAbortMove(Sim& sim, Unit* u) {
  // AbortMove 0x5a3750 (through vt+0x10 from 0x5a5cfd): MakeIdle, then broadcast(Aborted = 1); a move task listening
  // (navigator listener 0x618bb0) takes Failed and resumes its thread, re-linked at the run list's end, whichever
  // stage the call comes from. Seen (probe mp1_1958): a stream AbortNavigation aborts MP1 army 1's engineer
  // 0x10000e's guard walk at 1957. In the original its repair of the factory's unit then ends a beat after that of
  // its neighbour 0x10001e (2152 / 2151), ours the other way round without the re-link (thread order inferred).
  // Only a move task under way listens: a navigator with no goal (a UEF pod's AssistCommander aborted by
  // ConstructionUnit:OnPrepareArmToBuild, Upgrades 3216) wakes nobody.
  const bool wake = u->motion.hasGoal && !u->motion.arrived && !u->motion.failed &&
                    NavEventWakesCommandThread(u);  // (asked before the goal is gone)
  MotionStop(u);
  // The land navigator's event 1 (0x5a378e -> 0x5a6c50, sent when MakeIdle found it busy) is "failed" to a plain
  // move task: its listener 0x618bb0 maps events 0 and 1 to result 2 (0x618bc2; 2 = success, 0x618bd2), and the
  // task's TaskTick then ends the order like a failed arrival. Ours only woke the thread, so a Move aborted from Lua
  // stayed in the queue with no goal. Seen: MP1 8627, the stream's AbortNavigation (FAF SimCallbacks.lua 655,
  // abort-navigation.lua 45) on army 1's engineers 0x100010 / 0x100011 / 0x1000a6 ... under plain Moves: the
  // original's queues are empty at 8628, ours kept Move x1 [Moving]. (CAiNavigatorAir::AbortMove 0x5a4f00 sends no
  // such event.)
  if (wake && !u->task && !u->motion.air && IsAlive(u)) u->motion.failed = true;
  if (wake && IsAlive(u)) sim.ResumeCommandThread(u);
}

namespace {

// Issue<Type>(units, position or entity target)
// ValidateIssue 0x6ef9e0, the Reclaim case (0x6f059c; the type switch 0x6efdc2, index type - 2): may unit u take a
// Reclaim of entity t? u: not carried unless a POD (GetTransportedBy 0x5e3c30), in category RECLAIM. t: none or
// destroy-queued passes; one with a blueprint must be RECLAIMABLE and not on the Air layer (t+0x118 == 0x10); its unit
// (a blip's creator, 0x579670) must be reclaimable by its attributes (GetAttributes()+0x68, which Unit:SetReclaimable
// 0x6c9460 writes). Seen: Skirmish 1812, M28 gives army 6's engineer 0x600008 a Reclaim of army 1's engineer 0x100002,
// which a transport is carrying (Air layer): the original keeps the engineer's queue empty, ours took the order.
static bool ReclaimOrderValid(lua_State* L, const Unit* u, Entity* t) {
  Sim& sim = *S(L);
  if (u->transportedBy && !BpInCategory(sim, u->blueprint, "POD")) return false;
  if (!BpInCategory(sim, u->blueprint, "RECLAIM")) return false;
  if (!t || t->destroyQueued) return true;
  if (t->blueprint && !BpInCategory(sim, t->blueprint, "RECLAIMABLE")) return false;
  Entity* e = t;
  if (e->kind == Entity::Kind::Blip) e = static_cast<ReconBlip*>(e)->source;
  if (!e || e->kind != Entity::Kind::Unit) return true;
  if (static_cast<const Unit*>(e)->layer == "Air") return false;
  if (!e->HasLuaObject()) return true;
  PushObjectValue(L, e, "Reclaimable");  // (Unit:SetReclaimable's value; true until a script says otherwise)
  const bool ok = lua_isnil(L, -1) || lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return ok;
}

// The command's issue target (cmd+0x9c..+0xac, SSTITarget): an entity's raw id (the blip a script passed stays the
// blip), else the point.
void SetIssueTarget(UnitCommand& c, const Entity* e, const Vec3* p) {
  if (e) {
    c.issueTargetType = 1;
    c.issueTargetEntity = e->id;
    c.issueTargetPos = Vec3{};
  } else if (p) {
    c.issueTargetType = 2;
    c.issueTargetEntity = 0xf0000000u;
    c.issueTargetPos = *p;
  } else {
    c.issueTargetType = 0;
    c.issueTargetEntity = 0xf0000000u;
    c.issueTargetPos = Vec3{};
  }
}

template <CommandType T>
int l_IssueTarget(lua_State* L) {
  // SCR_GetTarget 0x6eef60 converts the target first: an entity target picks a target point (one draw,
  // value unused); the CUnitCommand ctor decodes it again (a second draw) when a command is created.
  // IssueGuard / IssuePatrol / IssueAggressiveMove take it before their unit list (func_GetUnitList 0x6eee40), the
  // others after it (which of the two errors a script sees). IssueReclaim / Repair / Capture / OverCharge take their
  // entity without SCR_GetTarget.
  constexpr bool kGetTarget = T != CommandType::Reclaim && T != CommandType::Repair && T != CommandType::Capture &&
                              T != CommandType::OverCharge;
  constexpr bool kTargetFirst = T == CommandType::Guard || T == CommandType::Patrol || T == CommandType::AggressiveMove;
  std::vector<Unit*> units;
  if (kGetTarget && !kTargetFirst) units = UnitsArg(L, 1);
  if (kGetTarget) CheckScriptTarget(L, 2);
  Entity* te = ToObject<Entity>(L, 2);
  TargetPointDraw(*S(L), te);
  if (!kGetTarget || kTargetFirst) units = UnitsArg(L, 1);
  if (T == CommandType::Reclaim)
    units.erase(std::remove_if(units.begin(), units.end(), [&](Unit* u) { return !ReclaimOrderValid(L, u, te); }),
                units.end());
  constexpr bool kForm = T == CommandType::FormMove || T == CommandType::FormAttack || T == CommandType::FormPatrol ||
                         T == CommandType::FormAggressiveMove;
  int formIndex = -1;
  if (kForm) {  // IssueForm*: the formation name -> script index; nothing is issued for an unknown one
    std::string name = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
    formIndex = FormationScriptIndex(L, name, units);
    if (formIndex < 0) return 0;
  }
  Vec3 targetAt{};  // (the target as ProcessUnitCommand tests it: RefusesTargetOutside)
  const bool hasTarget = te ? (targetAt = te->position, true) : PosArg(L, 2, &targetAt);
  auto c = Issue(L, units, T, hasTarget ? &targetAt : nullptr, te);
  if (Entity* e = te) {
    // a blip the script passed stands for its unit, as in a player's order (BlackOps' drone carriers:
    // IssueAttack({drone}, unit:GetBlip(army)); seen in Experimental2 at 964: the original's drones fly at the
    // enemy, ours ended the order at once - no weapon could take the blip itself)
    SetCommandEntityTarget(*c, e);
    if (!c->units.empty()) c->targetPoint = TargetPointDraw(*S(L), e);
  } else {
    c->hasPos = PosArg(L, 2, &c->pos);
  }
  // cmd+0x9c..+0xac (the SSTITarget the command was made from: type, raw entity id, point) is a script's order's as
  // well as a player's: the CUnitCommand ctor 0x6e81b0 takes it from the issue data whoever made it, and
  // ProcessUnitCommand's "same order again" test (0x6efbb4) compares a player's order with it. Seen: MP1 5943, army
  // 1's 17 ships hold one Move FAF's DistributeOrders issued at 5939 (IssueMove to the point of the order they had);
  // the player's Move with clear to that very point at 5943 is not taken by the original (no clear: they go on, Moving,
  // their navigators' goals unchanged); ours compared it with a point of (0, 0, 0), cleared their queues and planned
  // again.
  SetIssueTarget(*c, te, c->hasPos ? &c->pos : nullptr);
  if (kForm) {
    c->formation = lua_tostring(L, 3);
    c->formIndex = formIndex;
    // arg 4: heading in degrees -> a quaternion about +Y (0x570750)
    float h = static_cast<float>(lua_tonumber(L, 4)) * 0.0174532924f;
    c->formQw = dmath::Cos(h * 0.5f);
    c->formQx = 0;
    c->formQy = dmath::Sin(h * 0.5f);
    c->formQz = 0;
    c->formScale = 1.0f;
  }
  PushCommand(L, c);
  return 1;
}

// Issue<Type>(units [, ...]) without a position (blueprint ids, counts, script tables...)
template <CommandType T>
int l_IssueOther(lua_State* L) {
  auto units = UnitsArg(L, 1);
  // the target as ProcessUnitCommand tests it (RefusesTargetOutside): the entity argument's position, else the
  // first position table's (the loop below takes them in the same way)
  Vec3 targetAt{};
  bool hasTarget = false;
  for (int i = 2; i <= lua_gettop(L) && !hasTarget; ++i) {
    if (lua_type(L, i) == LUA_TSTRING || lua_type(L, i) == LUA_TNUMBER) continue;
    if (Entity* e = ToObject<Entity>(L, i)) {
      targetAt = e->position;
      hasTarget = true;
    } else if (lua_istable(L, i)) {
      lua_rawgeti(L, i, 1);
      const bool isPos = lua_isnumber(L, -1);
      lua_pop(L, 1);
      if (isPos) hasTarget = PosArg(L, i, &targetAt);
    }
  }
  std::string upgradeTo;  // (the Upgrade case of ProcessUnitCommand tests the order's blueprint: RefusesUpgradeAgain)
  if (T == CommandType::Upgrade)
    for (int i = 2; i <= lua_gettop(L) && upgradeTo.empty(); ++i)
      if (lua_type(L, i) == LUA_TSTRING) upgradeTo = lua_tostring(L, i);
  auto c = Issue(L, units, T, hasTarget ? &targetAt : nullptr, nullptr, upgradeTo.empty() ? nullptr : &upgradeTo);
  Entity* issuedAt = nullptr;  // (the entity argument, for the command's issue target)
  for (int i = 2; i <= lua_gettop(L); ++i) {
    if (lua_type(L, i) == LUA_TSTRING && c->blueprintId.empty()) c->blueprintId = lua_tostring(L, i);
    else if (lua_type(L, i) == LUA_TNUMBER) c->count = static_cast<int>(lua_tonumber(L, i));
    else if (Entity* e = ToObject<Entity>(L, i)) {
      c->targetId = EntityRef(e);
      if (!issuedAt) issuedAt = e;
      TargetPointDraw(*S(L), e);  // UpdateTarget (IssueSacrifice ...), then the command ctor's decode
      if (!c->units.empty()) c->targetPoint = TargetPointDraw(*S(L), e);
    } else if (!c->hasPos && lua_istable(L, i)) {
      Vec3 p;
      lua_rawgeti(L, i, 1);
      bool isPos = lua_isnumber(L, -1);
      lua_pop(L, 1);
      if (isPos && PosArg(L, i, &p)) {
        c->pos = p;
        c->hasPos = true;
      }
    }
  }
  SetIssueTarget(*c, issuedAt, c->hasPos ? &c->pos : nullptr);  // (cmd+0x9c, as l_IssueTarget)
  PushCommand(L, c);
  return 1;
}

// IssueFactoryAssist(factories, target) 0x6f3410: a Guard in the factories' own queues (append); this
// is what links a factory to the factory it assists (its guard chain).
int l_IssueFactoryAssist(lua_State* L) {
  Sim& sim = *S(L);
  Entity* e = ToObject<Entity>(L, 2);
  TargetPointDraw(sim, e);  // UpdateTarget 0x5d55b0 on the target arg
  std::shared_ptr<UnitCommand> c;
  Vec3 targetAt{};
  const bool hasTarget = e ? (targetAt = e->position, true) : PosArg(L, 2, &targetAt);
  for (Unit* u : UnitsArg(L, 1)) {
    if (!(LiveCommandCaps(L, u) & 0x8u) || !u->isFactoryBuilder || u->dead) continue;  // RULEUCC_Guard
    if (RefusesGuard(sim, L, u, e)) continue;  // (the Guard case of ProcessUnitCommand, above)
    if (RefusesTargetOutside(sim, u, hasTarget ? &targetAt : nullptr)) continue;
    if (!c) {
      c = std::make_shared<UnitCommand>();
      AssignEngineCommandId(sim, c);
      c->type = CommandType::Guard;
      if (e) {
        c->targetId = EntityRef(e);
        c->pos = e->position;
        c->hasPos = true;
        c->targetPoint = TargetPointDraw(sim, e);  // the CUnitCommand ctor's decode
      } else {
        c->hasPos = PosArg(L, 2, &c->pos);
      }
    }
    u->commands.push_back(c);
    c->units.insert(u);
    AirSpeedThroughEvent(*S(L), u);  // queue event 0
  }
  if (c) PushCommand(L, c);
  else lua_pushnil(L);
  return 1;
}

// IssueScript(units, { TaskName = ..., ... }): a script task (/lua/sim/tasks/<TaskName>.lua)
int l_IssueScript(lua_State* L) {
  auto units = UnitsArg(L, 1);
  auto c = Issue(L, units, CommandType::Script);
  if (lua_istable(L, 2)) {
    lua_pushvalue(L, 2);
    c->scriptRef = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  PushCommand(L, c);
  return 1;
}

// IssueBuildFactory(factories, blueprintId, count): count separate BuildFactory commands
// (ids_repair_placement.md 4: GetCommandQueue shows one entry per build).
int l_IssueBuildFactory(lua_State* L) {
  auto units = UnitsArg(L, 1);
  const char* bp = luaL_checkstring(L, 2);
  int n = std::max(1, static_cast<int>(luaL_optnumber(L, 3, 1)));
  std::shared_ptr<UnitCommand> first;
  for (int i = 0; i < n; ++i) {
    auto c = Issue(L, units, CommandType::BuildFactory);
    c->blueprintId = bp;
    c->count = 1;
    if (!first) first = c;
  }
  PushCommand(L, first);
  return 1;
}

// IssueBuildMobile(units, position, blueprintId, table)
int l_IssueBuildMobile(lua_State* L) {
  auto units = UnitsArg(L, 1);
  CheckScriptTarget(L, 2);  // SCR_GetTarget 0x6eef60, after the unit list (0x6f5bfb)
  Vec3 targetAt{};
  const bool hasTarget = PosArg(L, 2, &targetAt);
  auto c = Issue(L, units, CommandType::BuildMobile, hasTarget ? &targetAt : nullptr);
  c->hasPos = PosArg(L, 2, &c->pos);
  if (lua_isstring(L, 3)) c->blueprintId = lua_tostring(L, 3);
  PushCommand(L, c);
  return 1;
}

int l_IssueStop(lua_State* L) {
  auto units = UnitsArg(L, 1);
  Issue(L, units, CommandType::Stop);
  return 0;
}

int l_IssueClearCommands(lua_State* L) {
  // MOHO64_DEBUG_CALL=IssueClearCommands: each call with its units and the Lua frames that made it
  static const bool dbg = getenv("MOHO64_DEBUG_CALL") && strcmp(getenv("MOHO64_DEBUG_CALL"), "IssueClearCommands") == 0;
  if (dbg) {
    std::string s;
    for (Unit* u : UnitsArg(L, 1)) s += " " + std::to_string(u->id);
    s += " from";
    lua_Debug ar;
    for (int lv = 1; lv < 16 && lua_getstack(L, lv, &ar); ++lv)
      if (lua_getinfo(L, "Sl", &ar)) s += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
    Logf(LogLevel::Info, "call %u IssueClearCommands%s", S(L)->tick(), s.c_str());
  }
  // cfunc_IssueClearCommandsL 0x6f1950: ClearCommandQueue 0x6ee2d0 for each unit, the same as an order with clear
  // (above): a dispatched Move asleep on its navigator's event is woken by its command's end and its thread is
  // linked in at the stage's end. Seen in the bench at 342: Cybran build drones whose move (IdleState.Main) was
  // cleared at 301 (BuildState.Main) ran their dispatchers after their commander's (re-linked at 300) and stopped
  // repairing right after it finished the structure, before the drone script's next beam step.
  // After the queue, the unit's attacker is stopped (0x6f19eb..0x6f19f5: unit+0x544, when there is one ->
  // CAiAttackerImpl::Stop 0x5d56f0 = SetDesiredTarget(none)): its weapons' acquire tasks start their period again
  // from this beat. Seen in Experimental2: BlackOps drones are given IssueClearCommands + IssueAttack at 964, and
  // in the original their depth charges (TargetCheckInterval 2) take the enemy at 1084 = 964 + 6 x 20, ours at the
  // old phase (1095). (Both steps: IssueClearCommandsTo.)
  for (Unit* u : UnitsArg(L, 1)) IssueClearCommandsTo(*S(L), u);
  return 0;
}

int l_IsCommandDone(lua_State* L) {
  if (!lua_istable(L, 1)) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushstring(L, "moho64_cmd");
  lua_rawget(L, 1);
  uint32_t id = static_cast<uint32_t>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  auto& byId = S(L)->commandsById;
  auto it = byId.find(id);
  bool done = true;
  if (it != byId.end())
    if (auto c = it->second.lock()) done = c->units.empty();
  lua_pushboolean(L, done);
  return 1;
}

// ---- Unit ----------------------------------------------------------------------------------

Unit* U(lua_State* L) { return CheckObject<Unit>(L, 1); }

// Unit:IsMoving 0x6cc200: the unit's position differs from the one it had a beat before (12-byte compare of
// u+0xb4 and u+0xd0); nothing about orders or the navigator.
int l_IsMoving(lua_State* L) {
  Unit* u = U(L);
  lua_pushboolean(L, !vm::SameBits(u->position, u->lastPosition));  // (0x6cc288..0x6cc295)
  return 1;
}

// GetCommandQueue's entry keys commandType, x, y, z: four different main positions among 8 hash nodes (their string
// hashes, lua_gpg_strhash, & 7). A fact of the four texts.
bool CommandEntryKeysApart() {
  static const bool apart = [] {
    const char* k[4] = {"commandType", "x", "y", "z"};
    unsigned m = 0;
    for (const char* s : k) m |= 1u << (lua_gpg_strhash(s, std::strlen(s)) & 7u);
    return __builtin_popcount(m) == 4;
  }();
  return apart;
}

int l_GetCommandQueue(lua_State* L) {
  // a destroyed unit: the original's own message (probe v9)
  Unit* u = ToObject<Unit>(L, 1);
  if (!u) {
    if (lua_istable(L, 1)) luaL_error(L, "UnitScript:GetCommandQueue Passed in an invalid unit");
    u = U(L);
  }
  Sim* sim = S(L);
  lua_newtable(L);
  int n = 0;
  // Speed only (bind 2026-10-28): when no collector check in the loop could fire (lua_gpg_nocollect, from the most the
  // loop can make: a table per command, its nodes, the key and value strings), the list gets its array part and each
  // entry its 8 hash nodes at once instead of growing them through rehashes. The entry's nodes are those growth gives
  // when the first four keys (commandType, x, y, z: always there, inserted before the growth's last rehash) have four
  // different main positions among 8 nodes (CommandEntryKeysApart: their re-insertion order then changes nothing;
  // the later keys go in the same way both times). The same objects are made in the same order, the heap holds the
  // same blocks at the end, and the tables the same nodes.
  const int count = static_cast<int>(u->commands.size());
  bool sized = false;
  if (count > 0 && CommandEntryKeysApart()) {
    size_t chars = 33;  // the six keys
    for (auto& c : u->commands) chars += 10 + c->blueprintId.size();  // (targetId's digits, blueprintId)
    sized = lua_gpg_nocollect(L, count, 2 * count, 8 * count, 6 + 2 * count, chars);
    if (sized) lua_gpg_arrayfor(L, -1, count);
  }
  // Each entry is the command's own table as FAF's patch fills it (0x129181d, on the command's LuaObject at cmd+0x20):
  // commandType (cmd+0x98); x, y, z ALWAYS, from the command's encoded target (cmd+0xa4..+0xac, which
  // CAiTarget::EncodeToSSTITarget 0x5e27d0 writes at every SetTarget: an entity target encodes as 0, 0, 0, a ground
  // target as its point, the command's own and not a formation member's slot); targetId (decimal) unless the encoded
  // id is 0xf0000000 (an entity's id stays when the entity is gone); blueprintId. There is no `target`: FAF's
  // Unit.GetCommandQueue (sim/Unit.lua 5087) adds it, and the entity's position as x, y, z, only for a live entity from
  // GetEntityById. Seen (MP1): army 7's DistributeOrders (16254) got an attack order whose entity was gone; ours
  // handed the dead entity's unbound table as `target` and IssueAttack raised "Invalid target set"; ours also gave a
  // gone target's id as "0", which FAF's wrapper turns into entity 0, army 0's commander.
  for (auto& c : u->commands) {
    if (sized) lua_newtablesized(L, 0, 3);
    else lua_newtable(L);
    lua_pushstring(L, "commandType");
    lua_pushnumber(L, static_cast<int>(c->type));
    lua_rawset(L, -3);
    const Vec3 p = c->targetId || !c->hasPos ? Vec3{0, 0, 0} : c->pos;
    lua_pushstring(L, "x");
    lua_pushnumber(L, p.x);
    lua_rawset(L, -3);
    lua_pushstring(L, "y");
    lua_pushnumber(L, p.y);
    lua_rawset(L, -3);
    lua_pushstring(L, "z");
    lua_pushnumber(L, p.z);
    lua_rawset(L, -3);
    if (c->targetId) {
      uint32_t id = sim->IdOfRef(c->targetId);
      if (c->targetViaBlip) {  // (the order named the army's blip: its id is the blip's)
        Entity* te = sim->FindEntity(c->targetId);
        if (te && te->kind == Entity::Kind::Unit && static_cast<Unit*>(te)->blip) id = static_cast<Unit*>(te)->blip->id;
      }
      if (id != 0xf0000000u) {
        lua_pushstring(L, "targetId");
        char digits[16];
        const auto r = std::to_chars(digits, digits + sizeof digits, id);  // (the text std::to_string gives)
        lua_pushlstring(L, digits, static_cast<size_t>(r.ptr - digits));
        lua_rawset(L, -3);
      }
    }
    if (!c->blueprintId.empty()) {
      lua_pushstring(L, "blueprintId");
      lua_pushstring(L, c->blueprintId.c_str());
      lua_rawset(L, -3);
    }
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

int l_IsIdleState(lua_State* L) {
  lua_pushboolean(L, U(L)->commands.empty());
  return 1;
}

int l_GetVelocity(lua_State* L) {
  Unit* u = U(L);
  const Vec3& v = UnitVelocity(u);  // Unit::GetVelocity 0x6a9c90
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}

void PushVector(lua_State* L, const Vec3& v) { moho::PushVector(L, v.x, v.y, v.z); }

int l_GetCurrentMoveLocation(lua_State* L) {
  Unit* u = U(L);
  if (!u->commands.empty() && (MoveLike(u->commands.front()->type) || ApproachLike(u->commands.front()->type))) {
    PushVector(L, TargetPos(*S(L), *u->commands.front(), u));
  } else {
    PushVector(L, u->position);
  }
  return 1;
}

bool CanPath(Sim& sim, Unit* u, const Vec3& to) {
  if (!u->motion.bp || !u->motion.bp->mobile()) return false;
  if (u->motion.bp->motionType == kMotionAir) return true;
  std::vector<Vec3> path;
  const NamedFootprint& f = UnitFootprint(u);  // (Unit:CanPathTo 0x6cba42: Entity::GetFootprint)
  if (!sim.navigation().FindPath(f, u->position, to, &path) || path.empty()) return false;
  // reachable only if the path ends in the goal's cell
  int gx, gz, ex, ez;
  GoalCell(f, to.x, to.z, &gx, &gz);
  GoalCell(f, path.back().x, path.back().z, &ex, &ez);
  return gx == ex && gz == ez;
}

int l_CanPathTo(lua_State* L) {
  Vec3 p;
  if (!PosArg(L, 2, &p)) luaL_error(L, "CanPathTo: expected a position");
  lua_pushboolean(L, CanPath(*S(L), U(L), p));
  return 1;
}

int l_CanPathToRect(lua_State* L) {
  luaL_checktype(L, 2, LUA_TTABLE);
  float r[4];
  const char* k[4] = {"x0", "y0", "x1", "y1"};
  for (int i = 0; i < 4; ++i) {
    lua_pushstring(L, k[i]);
    lua_gettable(L, 2);
    r[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  Vec3 c{(r[0] + r[2]) * 0.5f, 0, (r[1] + r[3]) * 0.5f};
  lua_pushboolean(L, CanPath(*S(L), U(L), c));
  return 1;
}

template <int Which>
int l_SetMult(lua_State* L) {
  Unit* u = U(L);
  float v = static_cast<float>(luaL_checknumber(L, 2));
  {
    // MOHO64_DEBUG_MULT=<unit id>: every speed / acceleration / turn multiplier a script gives that unit, and from where
    static const long dbg = getenv("MOHO64_DEBUG_MULT") ? atol(getenv("MOHO64_DEBUG_MULT")) : -1;
    if (dbg >= 0 && static_cast<long>(u->id) == dbg) {
      lua_Debug ar;
      std::string where;
      for (int lvl = 1; lvl <= 3 && lua_getstack(L, lvl, &ar); ++lvl) {
        lua_getinfo(L, "Sl", &ar);
        where += std::string(lvl > 1 ? " < " : "") + ar.short_src + ":" + std::to_string(ar.currentline);
      }
      Logf(LogLevel::Info, "mult %u unit %u kind %d = %g from %s", S(L)->tick(), u->id, Which, v, where.c_str());
    }
  }
  if (Which == 0) u->motion.speedMult = v;
  else if (Which == 1) u->motion.accMult = v;
  else if (Which == 2) u->motion.turnMult = v;
  else if (Which == 3) u->motion.breakOffTriggerMult = v;
  else u->motion.breakOffDistanceMult = v;
  return 0;
}

// Unit:SetImmobile(flag) 0x6c6770: sets or clears the unit state Immobile (1), nothing else. (Ours also keeps a
// flag.) The move goes on underneath: the steering drops its spline while the state lasts and makes a fresh
// one after it (ProcessSplineMovement), the navigator and the order stay.
int l_SetImmobile(lua_State* L) {
  Unit* u = U(L);
  u->immobile = lua_toboolean(L, 2) != 0;
  if (u->immobile) {
    u->unitStates.Insert(US("Immobile"));
  } else {
    u->unitStates.Erase(US("Immobile"));
  }
  return 0;
}

// Unit:ForceAltFootprint(flag): FAF's binding (its code at 0x128dcxx..0x128dd7e, registered by the FAF exe): two
// arguments, unit+0x125 := lua_toboolean(2) (0x128dd70), nothing else; Entity::GetFootprint 0x678880 then gives the
// AltFootprint (FAF patch 0x128e9a6). The Salem's script calls it from OnScriptBitSet / OnScriptBitClear of its
// weapon toggle (bit 1), which OnStopBeingBuilt sets for a human army's Salem off the land; M28AI's overseer calls
// it too. MOHO64_DEBUG_ALTFP=1 logs each call.
int l_ForceAltFootprint(lua_State* L) {
  if (lua_gettop(L) != 2) luaL_error(L, "%s\n  expected %d args, but got %d", "ForceAltFootprint", 2, lua_gettop(L));
  Unit* u = U(L);
  u->altFootprintForced = lua_toboolean(L, 2) != 0;
  static const bool dbg = getenv("MOHO64_DEBUG_ALTFP") != nullptr;
  if (dbg)
    Logf(LogLevel::Info, "altfp: tick %u unit %u ForceAltFootprint(%d) layer %s", S(L)->tick(), u->id,
         u->altFootprintForced ? 1 : 0, u->layer.c_str());
  return 0;
}

int l_IsMobile(lua_State* L) {
  Unit* u = U(L);
  lua_pushboolean(L, u->motion.bp && u->motion.bp->mobile() && !u->immobile);
  return 1;
}

// ---- navigator -------------------------------------------------------------------------------

// Unit::Unit 0x6a5d9f..0x6a5e2f: a unit gets a motion (0x6b7b60, unit+0x4b0) and a navigator (unit+0x54c: the land
// one 0x5a5790, or the air one 0x5a5800 for MotionType 2) only when its blueprint's MotionType is 1..8 (bp+0x290 - 1
// above 7 jumps past both, 0x6a5da5..0x6a5dab); RULEUMT_None (a structure, a carrier's external factory, a dummy)
// has neither. Unit:GetNavigator 0x6cc350 then pushes nil (0x6cc3d8..0x6cc3ef). Ours made a navigator object for
// any unit: the probe's navigator lines of a carrier's external factory (Naval), and a script's `if nav then` took
// the other branch; FAF's abort-navigation.lua calls GetNavigator():AbortMove() on every unit it is given.
static bool UnitHasNavigator(const Unit* u) {
  return u->motion.bp && u->motion.bp->motionType >= kMotionLand && u->motion.bp->motionType <= kMotionAmphibiousFloating;
}

// cfunc_UnitGetNavigatorL 0x6cc350: unit+0x54c's Lua object, nil when there is none (0x6cc3e0) - also after
// KillCleanup / ~Unit deleted it (EndNavigatorObject).
int l_GetNavigator(lua_State* L) {
  Unit* u = U(L);
  if (!UnitHasNavigator(u) || u->navigatorGone) {
    lua_pushnil(L);
    return 1;
  }
  if (!u->navigator) {
    auto nav = std::make_unique<NavigatorObject>();
    nav->unit = u;
    u->navigator = nav.get();
    CreateObject(L, nav.get(), "CAiNavigatorImpl");
    lua_pop(L, 1);
    S(L)->Own(std::move(nav));
  }
  PushObject(L, u->navigator);
  return 1;
}

NavigatorObject* N(lua_State* L) { return CheckObject<NavigatorObject>(L, 1); }
}  // namespace

// The unit's navigator is deleted (Unit::KillCleanup 0x6a8804..0x6a881a: unit+0x54c deleted (vt[0] with 1) and set to
// null; ~Unit 0x6a6cdb for a unit destroyed without a kill): its Lua table loses its object (IsDestroyed; its methods
// error "Game object has been destroyed") and GetNavigator() answers nil from then on.
void EndNavigatorObject(Unit* u) {
  u->navigatorGone = true;
  if (u->navigator) u->navigator->UnbindLua();
}

namespace {

int l_nav_SetGoal(lua_State* L) {
  NavigatorObject* n = N(L);
  Vec3 p;
  if (!PosArg(L, 2, &p) || !IsAlive(n->unit)) return 0;
  Unit* u = n->unit;
  if (!u->motion.bp || !u->motion.bp->mobile()) return 0;
  Sim* sim = S(L);
  // CAiNavigatorLand::SetGoal 0x5a3ed0 takes the goal only: the speed-through request stays the steering's (+0x9d),
  // as the last Unit::UpdateSpeedThroughStatus 0x6ac940 or navigator:SetSpeedThroughGoal left it. Seen (Skirmish 950):
  // M28 re-aims army 4's engineer 0x400010 (a Move with a second Move queued: request 1 since its move task's ctor at
  // 947) through navigator:SetGoal; the original's goal waypoint at 1225 has speed-through 1; ours took the Lua
  // navigator object's own flag (0) and only the per-beat refresh in CommandStep put it back.
  const bool through = u->motion.nav ? u->motion.nav->speedThroughGoal : n->speedThroughGoal;
  MotionSetGoal(*sim, u, std::vector<Vec3>{p}, through, sim->tick());
  return 0;
}
int l_nav_SetDestUnit(lua_State* L) {
  NavigatorObject* n = N(L);
  if (Entity* e = ToObject<Entity>(L, 2)) {
    lua_settop(L, 1);
    PushVector(L, e->position);
    return l_nav_SetGoal(L);
  }
  return 0;
}
int l_nav_AbortMove(lua_State* L) {
  Unit* u = N(L)->unit;
  if (!IsAlive(u)) return 0;
  NavigatorAbortMove(*Sim::From(L), u);
  return 0;
}
// CAiNavigatorAir (air_nav.md 4.1, 4.6): GetStatus 0x5a37a0 = nav+0x64 (2 from SetGoal / SetDestUnit until the
// arrival or AbortMove: ours steering, or a SetGoal still pending for this beat's motion step, which the original
// has already handed over); GetGoalPos 0x5a49f0 = status 2 ? goal (+0x7c) : the unit's position; GetCurrentTargetPos
// 0x5a50b0 = curTarget (+0x70), which AbortMove, the arrival and SetDestUnit leave as it was (zero before the first
// SetGoal). Ours answered the unit's own position for both while it flew. Seen: Skirmish 1405, M28's
// IssueTrackedMove (m28orders.lua:265-272) re-aims a moving aircraft through navigator:SetGoal when GetCurrentTargetPos
// lies within 3 of its last ordered point; with ours' answer it cleared the queue and issued a new Move instead, so
// the UEF transport 0x400018 (uea0107) was stopped a second ahead at 1405 and got the new goal at 1406 (the original's
// target is the new goal at 1405; the watch log's "wn" lines show curTarget and goal as above).
static bool AirNavStatus2(const Unit* u) {
  const AirMotion& a = *u->motion.air;
  return a.steering || a.pending;
}
// A unit with a land navigator (the Unit ctor 0x6a5d9b: MotionType 1 and 3..8; Air (2) gets the air one, 0 none).
// Seen (watch log "wn" lines): an idle land navigator answers its last target cell, (0.5, h, 0.5) before its first
// goal; ours answered the unit's position. M28 reads it (IssueTrackedMove re-aims a moving unit with SetGoal when
// GetCurrentTargetPos is within 3 of its last order).
static bool LandNavigated(const Unit* u) {
  const MotionBlueprint* b = u->motion.bp;
  return b && !u->motion.air && b->motionType >= 1 && b->motionType <= 8 && b->motionType != kMotionAir;
}
int l_nav_GetGoalPos(lua_State* L) {
  Unit* u = N(L)->unit;
  Vec3 g;
  if (u->motion.air) {
    const AirMotion& a = *u->motion.air;
    PushVector(L, !AirNavStatus2(u) ? u->position : a.pending ? a.pendingGoal : a.goal);
    return 1;
  }
  if (LandNavigated(u)) {  // CAiNavigatorLand::GetGoalPos 0x5a3d80: status 0 -> the unit's position, else the goal cell
    PushVector(L, LandNavGoal(u, &g) ? g : u->position);
    return 1;
  }
  if (u->motion.navDriven && LandNavGoal(u, &g)) PushVector(L, g);
  else PushVector(L, u->motion.hasGoal && !u->motion.path.empty() ? u->motion.path.back() : u->position);
  return 1;
}
int l_nav_GetCurrentTargetPos(lua_State* L) {
  Unit* u = N(L)->unit;
  const UnitMotion& m = u->motion;
  Vec3 t;
  if (m.air) {
    PushVector(L, m.air->pending ? m.air->pendingGoal : m.air->curTarget);
    return 1;
  }
  if (LandNavigated(u) || m.navDriven) PushVector(L, LandNavTarget(u, &t) ? t : u->position);
  else PushVector(L, m.hasGoal && m.pathIndex < m.path.size() ? m.path[m.pathIndex] : u->position);
  return 1;
}
int l_nav_GetStatus(lua_State* L) {
  Unit* u = N(L)->unit;
  int st = 0;  // Idle
  if (u->motion.air) st = AirNavStatus2(u) ? 2 : 0;
  else if (u->motion.navDriven) st = LandNavStatus(u);
  else if (u->motion.hasGoal) st = S(L)->tick() < u->motion.driveTick ? 1 : 2;  // Thinking / Steering
  lua_pushnumber(L, st);
  return 1;
}
int l_nav_AtGoal(lua_State* L) {
  Unit* u = N(L)->unit;
  lua_pushboolean(L, !u->motion.hasGoal);
  return 1;
}
int l_nav_HasGoodPath(lua_State* L) {
  lua_pushboolean(L, !N(L)->unit->motion.failed);
  return 1;
}
int l_nav_CanPathToGoal(lua_State* L) {
  Vec3 p;
  if (!PosArg(L, 2, &p)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, CanPath(*S(L), N(L)->unit, p));
  return 1;
}
int l_nav_SetSpeedThroughGoal(lua_State* L) {
  NavigatorObject* n = N(L);
  n->speedThroughGoal = lua_toboolean(L, 2) != 0;
  // (a land navigator's SetSpeedThroughGoal 0x5a4240 is the steering's request, CAiSteeringImpl 0x5d2390)
  if (IsAlive(n->unit) && n->unit->motion.nav) LandNavSetSpeedThrough(n->unit, n->speedThroughGoal);
  return 0;
}
int l_nav_IgnoreFormation(lua_State* L) {
  N(L)->ignoreFormation = lua_toboolean(L, 2) != 0;
  return 0;
}
int l_nav_IsIgnoringFormation(lua_State* L) {
  lua_pushboolean(L, N(L)->ignoreFormation);
  return 1;
}
int l_nav_False(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}
int l_nav_Nothing(lua_State*) { return 0; }

// ---- spatial queries --------------------------------------------------------------------------

bool RectArgs(lua_State* L, float r[4]) {
  if (lua_istable(L, 1)) {
    const char* k[4] = {"x0", "y0", "x1", "y1"};
    for (int i = 0; i < 4; ++i) {
      lua_pushstring(L, k[i]);
      lua_gettable(L, 1);
      r[i] = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
    return true;
  }
  for (int i = 0; i < 4; ++i) r[i] = static_cast<float>(luaL_checknumber(L, i + 1));
  return true;
}

// GetUnitsInRect(rect) / (x0, z0, x1, z1) (0x75ae80): every unit registered in the entity-grid cells the
// rect touches (no position test; dead units too), in grid order; nil when none (rect_queries.md).
int l_GetUnitsInRect(lua_State* L) {
  float r[4];
  RectArgs(L, r);
  std::vector<Entity*> found;
  S(L)->entityGrid().Gather(r[0], r[1], r[2], r[3], 1, &found);
  if (found.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtablesized(L, static_cast<int>(found.size()), 0);
  int n = 0;
  for (Entity* e : found) {
    PushObject(L, static_cast<Unit*>(e));
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// brain:GetUnitsAroundPoint(category, position, radius [, alliance 'Ally'|'Enemy'|'Neutral'])
int l_GetUnitsAroundPoint(lua_State* L) {
  AiBrain* b = CheckObject<AiBrain>(L, 1);
  const uint64_t* cat = ToCategory(L, 2);
  Vec3 p;
  PosArg(L, 3, &p);
  float r = static_cast<float>(luaL_checknumber(L, 4));
  // the alliance with five arguments (0x58e5fc), an EAlliance by name (sim/enum_arg.h, SCR_GetEnum 0x58e65e:
  // Neutral 0, Ally 1, Enemy 2; a nil or a name it does not take raises)
  const int alliance = lua_gettop(L) == 5 ? EnumArgAt(L, 5, kEnumAlliance) : -1;
  Sim* sim = S(L);
  std::vector<Unit*> found;
  sim->ForUnitsInRect(p.x - r, p.z - r, p.x + r, p.z + r, [&](Unit* u) {
    if (u->dead) return;
    float dx = u->position.x - p.x, dz = u->position.z - p.z;
    if (dx * dx + dz * dz > r * r) return;
    if (cat && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(cat, u->blueprint->entityIndex)))
      return;
    if (alliance >= 0 && b->army && u->army) {
      int rel = 1;
      if (u->army == b->army) rel = 2;
      else if (u->army->index - 1 < static_cast<int>(b->army->alliance.size())) rel = b->army->alliance[u->army->index - 1];
      // (our relation 0 enemy / 1 neutral / 2 ally as an EAlliance; any other value matches no unit)
      if ((rel == 2 ? 1 : rel == 0 ? 2 : 0) != alliance) return;
    }
    found.push_back(u);
  });
  std::sort(found.begin(), found.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  lua_newtablesized(L, static_cast<int>(found.size()), 0);
  int n = 0;
  for (Unit* u : found) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

}  // namespace

void RegisterCommandBindings(lua_State* L) {
  SetGlobal(L, "IssueMove", l_IssueTarget<CommandType::Move>);
  SetGlobal(L, "IssueFactoryAssist", l_IssueFactoryAssist);
  SetGlobal(L, "IssueFormMove", l_IssueTarget<CommandType::FormMove>);
  SetGlobal(L, "IssueAggressiveMove", l_IssueTarget<CommandType::AggressiveMove>);
  SetGlobal(L, "IssueFormAggressiveMove", l_IssueTarget<CommandType::FormAggressiveMove>);
  SetGlobal(L, "IssuePatrol", l_IssueTarget<CommandType::Patrol>);
  SetGlobal(L, "IssueFormPatrol", l_IssueTarget<CommandType::FormPatrol>);
  SetGlobal(L, "IssueAttack", l_IssueTarget<CommandType::Attack>);
  SetGlobal(L, "IssueFormAttack", l_IssueTarget<CommandType::FormAttack>);
  SetGlobal(L, "IssueGuard", l_IssueTarget<CommandType::Guard>);
  SetGlobal(L, "IssueReclaim", l_IssueTarget<CommandType::Reclaim>);
  SetGlobal(L, "IssueRepair", l_IssueTarget<CommandType::Repair>);
  SetGlobal(L, "IssueCapture", l_IssueTarget<CommandType::Capture>);
  SetGlobal(L, "IssueNuke", l_IssueTarget<CommandType::Nuke>);
  SetGlobal(L, "IssueTactical", l_IssueTarget<CommandType::Tactical>);
  SetGlobal(L, "IssueTeleport", l_IssueTarget<CommandType::Teleport>);
  SetGlobal(L, "IssueOverCharge", l_IssueTarget<CommandType::OverCharge>);
  SetGlobal(L, "IssueBuildMobile", l_IssueBuildMobile);
  SetGlobal(L, "IssueBuildAllMobile", l_IssueBuildMobile);
  SetGlobal(L, "IssueBuildFactory", l_IssueBuildFactory);
  SetGlobal(L, "IssueUpgrade", l_IssueOther<CommandType::Upgrade>);
  SetGlobal(L, "IssueScript", l_IssueScript);
  SetGlobal(L, "IssueSiloBuildTactical", l_IssueOther<CommandType::BuildSiloTactical>);
  SetGlobal(L, "IssueSiloBuildNuke", l_IssueOther<CommandType::BuildSiloNuke>);
  SetGlobal(L, "IssueDive", l_IssueOther<CommandType::Dive>);
  SetGlobal(L, "IssueKillSelf", l_IssueOther<CommandType::KillSelf>);
  SetGlobal(L, "IssueDestroySelf", l_IssueOther<CommandType::DestroySelf>);
  SetGlobal(L, "IssueSacrifice", l_IssueOther<CommandType::Sacrifice>);
  SetGlobal(L, "IssuePause", l_IssueOther<CommandType::Pause>);
  SetGlobal(L, "IssueStop", l_IssueStop);
  SetGlobal(L, "IssueClearCommands", l_IssueClearCommands);
  SetGlobal(L, "IsCommandDone", l_IsCommandDone);
  SetGlobal(L, "GetUnitsInRect", l_GetUnitsInRect);

  SetMethod(L, "Unit", "GetCommandQueue", l_GetCommandQueue);
  SetMethod(L, "Unit", "IsMoving", l_IsMoving);
  SetMethod(L, "Unit", "IsIdleState", l_IsIdleState);
  SetMethod(L, "Unit", "GetVelocity", l_GetVelocity);
  SetMethod(L, "Unit", "GetCurrentMoveLocation", l_GetCurrentMoveLocation);
  SetMethod(L, "Unit", "CanPathTo", l_CanPathTo);
  SetMethod(L, "Unit", "CanPathToRect", l_CanPathToRect);
  SetMethod(L, "Unit", "SetSpeedMult", l_SetMult<0>);
  SetMethod(L, "Unit", "SetAccMult", l_SetMult<1>);
  SetMethod(L, "Unit", "SetTurnMult", l_SetMult<2>);
  SetMethod(L, "Unit", "SetBreakOffTriggerMult", l_SetMult<3>);
  SetMethod(L, "Unit", "SetBreakOffDistanceMult", l_SetMult<4>);
  SetMethod(L, "Unit", "SetImmobile", l_SetImmobile);
  SetMethod(L, "Unit", "ForceAltFootprint", l_ForceAltFootprint);
  SetMethod(L, "Unit", "IsMobile", l_IsMobile);
  SetMethod(L, "Unit", "GetNavigator", l_GetNavigator);

  SetMethod(L, "CAiNavigatorImpl", "SetGoal", l_nav_SetGoal);
  SetMethod(L, "CAiNavigatorImpl", "SetDestUnit", l_nav_SetDestUnit);
  SetMethod(L, "CAiNavigatorImpl", "AbortMove", l_nav_AbortMove);
  SetMethod(L, "CAiNavigatorImpl", "GetGoalPos", l_nav_GetGoalPos);
  SetMethod(L, "CAiNavigatorImpl", "GetCurrentTargetPos", l_nav_GetCurrentTargetPos);
  SetMethod(L, "CAiNavigatorImpl", "GetStatus", l_nav_GetStatus);
  SetMethod(L, "CAiNavigatorImpl", "AtGoal", l_nav_AtGoal);
  SetMethod(L, "CAiNavigatorImpl", "HasGoodPath", l_nav_HasGoodPath);
  SetMethod(L, "CAiNavigatorImpl", "CanPathToGoal", l_nav_CanPathToGoal);
  SetMethod(L, "CAiNavigatorImpl", "SetSpeedThroughGoal", l_nav_SetSpeedThroughGoal);
  SetMethod(L, "CAiNavigatorImpl", "IgnoreFormation", l_nav_IgnoreFormation);
  SetMethod(L, "CAiNavigatorImpl", "IsIgnorningFormation", l_nav_IsIgnoringFormation);
  SetMethod(L, "CAiNavigatorImpl", "FollowingLeader", l_nav_False);
  SetMethod(L, "CAiNavigatorImpl", "BroadcastResumeTaskEvent", l_nav_Nothing);

  SetMethod(L, "CAiBrain", "GetUnitsAroundPoint", l_GetUnitsAroundPoint);
}

void PushUnitCommand(lua_State* L, const std::shared_ptr<UnitCommand>& c) {
  if (c) PushCommand(L, c);
  else lua_pushnil(L);
}

void ReleaseCommand(Unit* u, UnitCommand& c) { ReleaseCommandImpl(u, c); }

bool IsCoordinating(const UnitCommand& c) { return !c.coordinatedWith.empty(); }

// 0x6e90a0: an empty list -> true; +0x154 clear -> false; then every entry whose WeakPtr still holds a command
// (a deleted partner's is empty and skipped) must have +0x154 set
bool CoordinationSatisfied(const UnitCommand& c) {
  if (c.coordinatedWith.empty()) return true;
  if (!c.coordinationStarted) return false;
  for (const auto& w : c.coordinatedWith)
    if (std::shared_ptr<UnitCommand> o = w.lock())
      if (!o->coordinationStarted) return false;
  return true;
}

std::shared_ptr<UnitCommand> MarkHeadCoordinationStarted(Unit* u) {
  if (u->commands.empty()) return nullptr;
  std::shared_ptr<UnitCommand> c = u->commands.front();
  if (c) c->coordinationStarted = true;
  return c;
}

}  // namespace moho
