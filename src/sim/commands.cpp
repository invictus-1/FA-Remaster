// Unit commands and their Lua bindings (see commands.h).
#include "sim/commands.h"
#include "sim/entity_grid.h"
#include "sim/formation.h"
#include "sim/landnav.h"
#include "sim/combat.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "core/dmath.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/transport.h"
#include "sim/units.h"

namespace moho {
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
  if (on) u->unitStates.insert("Moving");
  else u->unitStates.erase("Moving");
}

// A unit drops a command: when no unit holds it any more the command object goes, and with it
// the ferry beacon it created (CUnitCommand::DestroyInternal 0x6e8500).
void ReleaseCommandImpl(Unit* u, UnitCommand& c) {
  FormationRemoveUnit(c, u);
  c.units.erase(u);
  if (!c.units.empty() || !c.beaconRef) return;
  Sim& sim = *Sim::From(u->luaState());
  if (Entity* b = sim.FindEntity(c.beaconRef))
    if (!b->destroyQueued) sim.QueueDestroy(b);
  c.beaconRef = 0;
}

// Remove the head command of u (it is done for this unit).
void PopHead(Unit* u) {
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

void RunPathSearch(Sim& sim, Unit* u, bool /*continuing*/) {
  if (u->commands.empty()) return;
  UnitCommand& c = *u->commands.front();
  Vec3 goal = TargetPos(sim, c, u);
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

// ---- CUnitPatrolTask: Patrol 16, FormPatrol 18, AggressiveMove 35, FormAggressiveMove 36 ----------------
// (engine-ref attack_move.md 2). Runs every 6 ticks; finds enemies in a box along its leg and pushes an
// attack child; otherwise (re-)issues the navigator goal; ends when the unit is within ceil(speed) cells of
// the goal cell (from the 4th run on).
struct PatrolData {
  bool goalIssued = false, atGoal = false, aggressive = false;
  int ticks = 0;
  PatrolBox box;
  BuildTask* child = nullptr;
  uint32_t childFrom = 0;
};

namespace {

bool IsPatrolType(CommandType t) {
  return t == CommandType::Patrol || t == CommandType::FormPatrol || t == CommandType::AggressiveMove ||
         t == CommandType::FormAggressiveMove;
}

// RecomputePatrolSearchBox 0x61b2f0: the leg from the previous patrol point (or the unit) to the goal,
// widened by GuardScanRadius
void PatrolSearchBox(Sim& sim, Unit* u, PatrolData& p) {
  if (u->commands.empty()) return;
  const UnitCommand& cur = *u->commands.front();
  const UnitCommand& last = *u->commands.back();
  Vec3 E = TargetPos(sim, cur, u);
  Vec3 S = u->position;
  if (&last != &cur && (last.type == CommandType::Patrol || last.type == CommandType::FormPatrol))
    S = TargetPos(sim, last, u);
  float dx = E.x - S.x, dz = E.z - S.z, len = std::sqrt(dx * dx + dz * dz);
  float r = GuardScanRadiusOf(u);
  p.box.cx = (E.x + S.x) * 0.5f;
  p.box.cz = (E.z + S.z) * 0.5f;
  p.box.dx = len < 0.001f ? 0.0f : dx / len;
  p.box.dz = len < 0.001f ? 1.0f : dz / len;
  p.box.side = r;
  p.box.along = len * 0.5f + r;
}

float UnitTopSpeed(const Unit* u) {  // u+0x594
  const UnitMotion& m = u->motion;
  if (m.speedCap > 0) return m.speedCap;
  return m.bp ? m.bp->maxSpeed * m.speedMult : 0;
}

// AtPatrolGoal 0x61b610
bool AtPatrolGoal(Sim& sim, Unit* u, const PatrolData& p) {
  if (u->commands.empty() || !u->motion.bp) return false;
  const MotionBlueprint& b = *u->motion.bp;
  Vec3 g = TargetPos(sim, *u->commands.front(), u);
  int gx = static_cast<int>(std::nearbyint(g.x - b.footprint.sizeX * 0.5f));
  int gz = static_cast<int>(std::nearbyint(g.z - b.footprint.sizeZ * 0.5f));
  int x0 = gx, z0 = gz, x1 = gx + 1, z1 = gz + 1;
  if (p.goalIssued) {
    int m = static_cast<int>(std::ceil(UnitTopSpeed(u)));
    x0 -= m;
    z0 -= m;
    x1 += m;
    z1 += m;
  }
  int cx = static_cast<int>(std::nearbyint(u->position.x - b.footprint.sizeX * 0.5f));
  int cz = static_cast<int>(std::nearbyint(u->position.z - b.footprint.sizeZ * 0.5f));
  return x0 <= cx && cx <= x1 && z0 <= cz && cz <= z1;
}

void PatrolSetGoal(Sim& sim, Unit* u) {
  if (u->commands.empty()) return;
  std::vector<Vec3> path{TargetPos(sim, *u->commands.front(), u)};
  u->motion.arrived = u->motion.failed = false;
  MotionSetGoal(sim, u, path, NextIsMove(u), sim.tick());
  SetMoving(u, true);
}

}  // namespace

BuildTask* StartPatrolTask(Sim& sim, Unit* u, const UnitCommand& c) {
  auto t = std::make_unique<BuildTask>();
  t->type = c.type;
  t->order = "Patrol";
  t->pdata = std::make_shared<PatrolData>();
  t->pdata->aggressive = c.type == CommandType::AggressiveMove;
  u->unitStates.insert("Patrolling");
  PatrolSearchBox(sim, u, *t->pdata);
  BuildTask* r = t.get();
  sim.Own(std::move(t));
  return r;
}

int TickPatrol(Sim& sim, Unit* u, BuildTask& t) {
  PatrolData& p = *t.pdata;
  if (p.child) {  // the child runs; when it ends the patrol runs again in the same tick
    if (sim.tick() < p.childFrom) return kTaskRunning;
    int r = TickBuildTask(sim, u, *p.child);
    if (r == kTaskRunning) return kTaskRunning;
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
      p.child = c;
      p.childFrom = next;
      p.goalIssued = false;
      t.waitUntil = next;
      return kTaskRunning;
    }
  }
  // (d) move / arrival
  if (u->unitStates.count("NeedToTerminateTask")) return kTaskDone;
  UnitCommand* c = u->commands.empty() ? nullptr : u->commands.front().get();
  if (p.goalIssued && p.ticks > 3 && AtPatrolGoal(sim, u, p)) {
    if (!c || !c->form) return kTaskDone;
    if (u->formAllAtGoal || FormationGroupAtGoal(sim, *c, u)) return kTaskDone;
    t.waitUntil = next;
    return kTaskRunning;
  }
  if (!u->motion.hasGoal || !p.goalIssued) {
    PatrolSetGoal(sim, u);
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
  if (u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y || u->position.z != u->lastPosition.z)
    MotionStop(u);  // AbortMove only when it moved this tick
  u->unitStates.erase("NeedToTerminateTask");
  u->unitStates.erase("Patrolling");
  u->leashPos = {};
}

namespace {

void StartHead(Sim& sim, Unit* u) {
  // carried by a transport: nothing is dispatched until dropped; a dropped unit waits until it
  // landed (FAF makes it immobile while it falls; the original's move task would wait too)
  if ((u->parentId && u->attachFull) || u->unitStates.count("Attached") || u->motion.ballistic) {
    u->headState = kNotStarted;
    return;
  }
  while (!u->commands.empty()) {
    UnitCommand& c = *u->commands.front();
    if (c.type == CommandType::Stop) {
      // Stop: everything queued up to here is dropped; the unit coasts to a stop.
      PopHead(u);
      MotionStop(u);
      SetMoving(u, false);
      continue;
    }
    if (SiloCommand(sim, u, c)) {
      PopHead(u);
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
    if (IsPatrolType(c.type) && u->motion.bp && u->motion.bp->mobile() && !u->immobile) {
      u->task = StartPatrolTask(sim, u, c);
      u->headState = kRunning;
      return;
    }
    if (BuildTask* bt = StartBuildTask(sim, u, c)) {
      u->task = bt;
      u->headState = kRunning;
      return;
    }
    bool mobile = u->motion.bp && u->motion.bp->mobile() && !u->immobile;
    if ((MoveLike(c.type) || ApproachLike(c.type)) && mobile) {
      SetMoving(u, true);
      bool driving = u->motion.hasGoal || (u->motion.vel.x * u->motion.vel.x + u->motion.vel.z * u->motion.vel.z) > 1e-6f;
      u->motion.arrived = false;
      u->motion.failed = false;
      (void)driving;
      RunPathSearch(sim, u, true);  // land units queue their search in the navigator
      return;
    }
    // Not carried out yet: finish it at once so the scripts see an idle unit.
    static std::map<int, int> notDone;
    if (getenv("MOHO64_DEBUG_TASKS") && (++notDone[static_cast<int>(c.type)] % 100) == 1)
      Logf(LogLevel::Debug, "moho64: command %s not carried out (x%d)", kCommandNames[static_cast<int>(c.type)],
           notDone[static_cast<int>(c.type)]);
    static std::set<int> logged;
    if (logged.insert(static_cast<int>(c.type)).second)
      Logf(LogLevel::Debug, "moho64: command %s is not carried out yet (finished at once)",
           kCommandNames[static_cast<int>(c.type)]);
    PopHead(u);
  }
  SetMoving(u, false);
}

}  // namespace

void ForgetUnitCommands(Unit* u) {
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
// A finished move ends its command (the move task returns -1 and the dispatcher starts the next one).
void ArrivalStep(Sim& sim, Unit* u) {
  if (!IsAlive(u) || u->commands.empty()) return;
  if (u->headState != kRunning) return;
  UnitMotion& m = u->motion;
  if (!m.arrived && !m.failed) return;
  if (u->task) return;  // a task command (build, guard, attack, ...) handles its own moves
  if (u->engageId) {  // arrived at the enemy it stopped for, not at the patrol point
    m.arrived = m.failed = false;
    return;
  }
  UnitCommand& c = *u->commands.front();
  // CUnitFormAndMoveTask: arriving does not end it; it waits for its group to be at goal (formations.md 11)
  if (c.type == CommandType::FormMove && !m.failed && FormationHas(c, u)) {
    m.arrived = false;
    return;
  }
  bool patrol = c.type == CommandType::Patrol || c.type == CommandType::FormPatrol;
  std::shared_ptr<UnitCommand> keep = u->commands.front();
  PopHead(u);
  m.arrived = false;
  if (patrol && !m.failed) {  // patrols cycle through their points
    u->commands.push_back(keep);
    keep->units.insert(u);
  }
  m.failed = false;
  SetMoving(u, false);  // the move task's dtor clears it; a following move sets it again
  if (u->commands.empty()) return;
  StartHead(sim, u);
}

void CommandStep(Sim& sim, Unit* u) {
  if (!IsAlive(u)) return;
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
      } else if (task->pdata && (task->type == CommandType::Patrol || task->type == CommandType::FormPatrol) &&
                 !u->commands.empty()) {
        // a patrol point goes to the back of the queue (the loop); attack-moves are removed
        std::shared_ptr<UnitCommand> keep = u->commands.front();
        PopHead(u);
        u->commands.push_back(keep);
        keep->units.insert(u);
      } else {
        PopHead(u);
      }
      if (!IsAlive(u) || u->commands.empty()) break;
      StartHead(sim, u);
    }
    return;
  }
  // a form move ends when its group (or every group) is at goal: FormationAtGoal / u+0x590
  if (st == kRunning && !u->commands.empty() && u->commands.front()->type == CommandType::FormMove &&
      FormationHas(*u->commands.front(), u) &&
      (u->formAllAtGoal || FormationGroupAtGoal(sim, *u->commands.front(), u))) {
    PopHead(u);
    u->motion.arrived = u->motion.failed = false;
    SetMoving(u, false);
    if (!u->commands.empty()) StartHead(sim, u);
    return;
  }
  // keep "drive through" up to date when moves were queued behind the current one
  if (st == kRunning && u->motion.hasGoal && !u->commands.empty() && MoveLike(u->commands.front()->type)) {
    if (u->motion.navDriven) LandNavSetSpeedThrough(u, NextIsMove(u));
    else u->motion.passThrough = NextIsMove(u);
  }
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
    ArrivalStep(sim, u);
    CommandStep(sim, u);
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

std::vector<Unit*> UnitsArg(lua_State* L, int idx) {
  std::vector<Unit*> out;
  if (!lua_istable(L, idx)) luaL_error(L, "expected a table of units");
  // a single unit table also works
  if (Unit* u = ToObject<Unit>(L, idx)) {
    out.push_back(u);
    return out;
  }
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (Unit* u = ToObject<Unit>(L, -1))
      if (IsAlive(u)) out.push_back(u);
    lua_pop(L, 1);
  }
  return out;
}

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
  lua_pushnumber(L, c->id);
  lua_rawset(L, -3);
}

// Commands a factory passes on to what it builds (its rally point, patrols, ...).
bool FactoryCommand(CommandType t) {
  return MoveLike(t) || t == CommandType::Attack || t == CommandType::FormAttack || t == CommandType::Guard ||
         t == CommandType::Ferry;
}

std::shared_ptr<UnitCommand> Issue(lua_State* L, const std::vector<Unit*>& units, CommandType type) {
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
  c->id = sim->nextCommandId++;
  c->type = type;
  sim->commandsById[c->id] = c;
  for (Unit* u : units) {
    // FilterByCommandCap 0x6eecf0 drops immobile factories (their orders go to the rally queue only
    // through IssueFactoryRallyPoint / IssueFactoryCommand, factory_handoff.md 3.2)
    if (FactoryCommand(type) && IsImmobileFactory(*sim, u)) continue;
    u->commands.push_back(c);
    c->units.insert(u);
  }
  return c;
}

// Issue<Type>(units, position or entity target)
template <CommandType T>
int l_IssueTarget(lua_State* L) {
  // SCR_GetTarget 0x6eef60 converts the target first: an entity target picks a target point (one draw,
  // value unused); the CUnitCommand ctor decodes it again (a second draw) when a command is created
  Entity* te = ToObject<Entity>(L, 2);
  TargetPointDraw(*S(L), te);
  auto units = UnitsArg(L, 1);
  constexpr bool kForm = T == CommandType::FormMove || T == CommandType::FormAttack || T == CommandType::FormPatrol ||
                         T == CommandType::FormAggressiveMove;
  int formIndex = -1;
  if (kForm) {  // IssueForm*: the formation name -> script index; nothing is issued for an unknown one
    std::string name = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
    formIndex = FormationScriptIndex(L, name, units);
    if (formIndex < 0) return 0;
  }
  auto c = Issue(L, units, T);
  if (Entity* e = te) {
    c->targetId = EntityRef(e);
    c->pos = e->position;
    c->hasPos = true;
    if (!c->units.empty()) c->targetPoint = TargetPointDraw(*S(L), e);
  } else {
    c->hasPos = PosArg(L, 2, &c->pos);
  }
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
  auto c = Issue(L, units, T);
  for (int i = 2; i <= lua_gettop(L); ++i) {
    if (lua_type(L, i) == LUA_TSTRING && c->blueprintId.empty()) c->blueprintId = lua_tostring(L, i);
    else if (lua_type(L, i) == LUA_TNUMBER) c->count = static_cast<int>(lua_tonumber(L, i));
    else if (Entity* e = ToObject<Entity>(L, i)) {
      c->targetId = EntityRef(e);
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
  for (Unit* u : UnitsArg(L, 1)) {
    if (!(UnitCommandCaps(L, *u->blueprint) & 0x8u) || !u->isFactoryBuilder || u->dead) continue;  // RULEUCC_Guard
    if (!c) {
      c = std::make_shared<UnitCommand>();
      c->id = sim.nextCommandId++;
      c->type = CommandType::Guard;
      sim.commandsById[c->id] = c;
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
  auto c = Issue(L, units, CommandType::BuildMobile);
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
  for (Unit* u : UnitsArg(L, 1)) {
    ForgetUnitCommands(u);
    MotionStop(u);
    SetMoving(u, false);
  }
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
  for (auto& c : u->commands) {
    lua_newtable(L);
    lua_pushstring(L, "commandType");
    lua_pushnumber(L, static_cast<int>(c->type));
    lua_rawset(L, -3);
    Vec3 p = TargetPos(*sim, *c, u);
    if (c->hasPos || c->targetId) {
      lua_pushstring(L, "x");
      lua_pushnumber(L, p.x);
      lua_rawset(L, -3);
      lua_pushstring(L, "y");
      lua_pushnumber(L, p.y);
      lua_rawset(L, -3);
      lua_pushstring(L, "z");
      lua_pushnumber(L, p.z);
      lua_rawset(L, -3);
    }
    if (c->targetId) {
      if (Entity* e = sim->FindEntity(c->targetId)) {
        lua_pushstring(L, "target");
        PushObject(L, e);
        lua_rawset(L, -3);
      }
      lua_pushstring(L, "targetId");
      {
        Entity* te = S(L)->FindEntity(c->targetId);
        lua_pushstring(L, std::to_string(te ? te->id : 0u).c_str());
      }
      lua_rawset(L, -3);
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
  lua_pushnumber(L, u->motion.lastMove.x);
  lua_pushnumber(L, u->motion.lastMove.y);
  lua_pushnumber(L, u->motion.lastMove.z);
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
  if (!sim.navigation().FindPath(u->motion.bp->footprint, u->position, to, &path) || path.empty()) return false;
  // reachable only if the path ends in the goal's cell
  int gx, gz, ex, ez;
  GoalCell(*u->motion.bp, to.x, to.z, &gx, &gz);
  GoalCell(*u->motion.bp, path.back().x, path.back().z, &ex, &ez);
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
  if (Which == 0) u->motion.speedMult = v;
  else if (Which == 1) u->motion.accMult = v;
  else u->motion.turnMult = v;
  return 0;
}

int l_SetImmobile(lua_State* L) {
  Unit* u = U(L);
  u->immobile = lua_toboolean(L, 2) != 0;
  if (u->immobile) MotionStop(u);
  return 0;
}

int l_IsMobile(lua_State* L) {
  Unit* u = U(L);
  lua_pushboolean(L, u->motion.bp && u->motion.bp->mobile() && !u->immobile);
  return 1;
}

// ---- navigator -------------------------------------------------------------------------------

int l_GetNavigator(lua_State* L) {
  Unit* u = U(L);
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

int l_nav_SetGoal(lua_State* L) {
  NavigatorObject* n = N(L);
  Vec3 p;
  if (!PosArg(L, 2, &p) || !IsAlive(n->unit)) return 0;
  Unit* u = n->unit;
  if (!u->motion.bp || !u->motion.bp->mobile()) return 0;
  Sim* sim = S(L);
  MotionSetGoal(*sim, u, std::vector<Vec3>{p}, n->speedThroughGoal, sim->tick());
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
  if (IsAlive(N(L)->unit)) MotionStop(N(L)->unit);
  return 0;
}
int l_nav_GetGoalPos(lua_State* L) {
  Unit* u = N(L)->unit;
  Vec3 g;
  if (u->motion.navDriven && LandNavGoal(u, &g)) PushVector(L, g);
  else PushVector(L, u->motion.hasGoal && !u->motion.path.empty() ? u->motion.path.back() : u->position);
  return 1;
}
int l_nav_GetCurrentTargetPos(lua_State* L) {
  Unit* u = N(L)->unit;
  const UnitMotion& m = u->motion;
  Vec3 t;
  if (m.navDriven) PushVector(L, LandNavTarget(u, &t) ? t : u->position);
  else PushVector(L, m.hasGoal && m.pathIndex < m.path.size() ? m.path[m.pathIndex] : u->position);
  return 1;
}
int l_nav_GetStatus(lua_State* L) {
  Unit* u = N(L)->unit;
  int st = 0;  // Idle
  if (u->motion.navDriven) st = LandNavStatus(u);
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
  N(L)->speedThroughGoal = lua_toboolean(L, 2) != 0;
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
  std::string alliance = lua_isstring(L, 5) ? lua_tostring(L, 5) : "";
  Sim* sim = S(L);
  std::vector<Unit*> found;
  sim->ForUnitsInRect(p.x - r, p.z - r, p.x + r, p.z + r, [&](Unit* u) {
    if (u->dead) return;
    float dx = u->position.x - p.x, dz = u->position.z - p.z;
    if (dx * dx + dz * dz > r * r) return;
    if (cat && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(cat, u->blueprint->entityIndex)))
      return;
    if (!alliance.empty() && b->army && u->army) {
      int rel = 1;
      if (u->army == b->army) rel = 2;
      else if (u->army->index - 1 < static_cast<int>(b->army->alliance.size())) rel = b->army->alliance[u->army->index - 1];
      if (alliance == "Ally" && rel != 2) return;
      if (alliance == "Enemy" && rel != 0) return;
      if (alliance == "Neutral" && rel != 1) return;
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
  SetMethod(L, "Unit", "IsIdleState", l_IsIdleState);
  SetMethod(L, "Unit", "GetVelocity", l_GetVelocity);
  SetMethod(L, "Unit", "GetCurrentMoveLocation", l_GetCurrentMoveLocation);
  SetMethod(L, "Unit", "CanPathTo", l_CanPathTo);
  SetMethod(L, "Unit", "CanPathToRect", l_CanPathToRect);
  SetMethod(L, "Unit", "SetSpeedMult", l_SetMult<0>);
  SetMethod(L, "Unit", "SetAccMult", l_SetMult<1>);
  SetMethod(L, "Unit", "SetTurnMult", l_SetMult<2>);
  SetMethod(L, "Unit", "SetImmobile", l_SetImmobile);
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

}  // namespace moho
