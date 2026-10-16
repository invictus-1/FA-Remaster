// Unit commands and their Lua bindings (see commands.h).
#include "sim/air.h"
#include "sim/commands.h"
#include "sim/entity_grid.h"
#include "sim/formation.h"
#include "sim/landnav.h"
#include "sim/combat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "core/dmath.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/replay.h"
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
  if (Entity* b = sim.FindEntity(c.beaconRef)) {
    static const bool dbg = getenv("MOHO64_DEBUG_TASKS") != nullptr;
    if (dbg)
      Logf(LogLevel::Info, "beacon %u of command %u (type %d) goes at tick %u: unit %u let the command go (in a beat: %d)", b->id,
           c.id, static_cast<int>(c.type), sim.tick(), u->id, sim.RecorderTick() != 0xffffffffu ? 1 : 0);
    // The beacon dies in the command object's destructor (0x6e8500), and the original deletes an emptied
    // command a beat's end later than its last holder lets go (seen twice in the Transports replay: a player's
    // order clears the transport's queue after tick T, the beacon is still listed after tick T + 1 and gone
    // after T + 2). Where the deletion is decided was not found in the exe: this is the observed timing.
    if (!b->destroyQueued) sim.QueueDestroyAfterBeat(b);
  }
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
  // CUnitMoveTask's constructor 0x6180e0 (the test 0x618a00, then 0x618344): a Move that came from a factory (a
  // rally point: cmd+0x142), is the unit's last command, the unit not TransportLoading, Teleporting or Guarding,
  // has its target rounded to whole numbers and put through PrepareMove (0x62b780, spacing 0, no exclusion). So
  // the second unit out of a factory does not head for the cell the first one stands on: it gets the nearest free
  // spot of the ring two footprints out.
  if (c.type == CommandType::Move && c.factoryIssued && u->commands.size() == 1 && u->motion.bp &&
      !u->unitStates.count("TransportLoading") && !u->unitStates.count("Teleporting") && !u->unitStates.count("Guarding")) {
    Vec3 p{std::nearbyint(goal.x), goal.y, std::nearbyint(goal.z)};
    if (p.x != 0.0f || p.y != 0.0f || p.z != 0.0f) {
      const float none[4] = {0, 0, 0, 0};
      if (u->motion.bp->motionType == kMotionAir) AirPrepareMove(sim, u, &p);
      else GroundPrepareMove(sim, u, &p, none, 0);
      goal = {p.x, goal.y, p.z};
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
  const NamedFootprint& f = UnitFootprint(u);  // (0x61b671: Entity::GetFootprint)
  Vec3 g = TargetPos(sim, *u->commands.front(), u);
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

void PatrolSetGoal(Sim& sim, Unit* u) {
  if (u->commands.empty()) return;
  std::vector<Vec3> path{TargetPos(sim, *u->commands.front(), u)};
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
  u->unitStates.insert("Patrolling");
  PatrolSearchBox(sim, u, *t->pdata);
  AirSpeedThroughEvent(sim, u);  // the patrol task ctor
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
  // Carried by a transport: nothing is dispatched until it is let go. Then the queue runs again from the NEXT
  // beat, also while the unit still falls (seen: a tank dropped by "unload these units" has that order in its own
  // queue, three times; it cannot carry them out and all three are gone the beat after the drop, long before it
  // lands). Not in the beat of the drop itself: the transport's thread, woken by its arrival, has its turn after
  // the cargo's. What blocks is unit state Attached alone (IAiCommandDispatchImpl::TaskTick 0x598f89, IsUnitState
  // 14), not the attach link: cargo a script took out of that state runs its orders (a DetachFromTransport, say).
  if (u->unitStates.count("Attached") || u->detachedAt == sim.tick()) {
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
  while (!u->commands.empty()) {
    UnitCommand& c = *u->commands.front();
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
    if (IsPatrolType(c.type) && u->motion.bp && u->motion.bp->mobile() && !u->immobile) {
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
      if (MoveLike(c.type)) AirSpeedThroughEvent(sim, u);  // the move task ctor
      return;
    }
    // Not carried out yet: finish it at once so the scripts see an idle unit.
    // MOHO64_DEBUG_TASKS=1: every (command type, blueprint) this happens to, once, with the tick and the count
    // so far for the type
    static std::map<int, int> notDone;
    static std::set<std::pair<int, std::string>> notDoneFor;
    static const bool dbgTasks = getenv("MOHO64_DEBUG_TASKS") != nullptr;
    if (dbgTasks) {
      const int n = ++notDone[static_cast<int>(c.type)];
      const std::string bp = u->blueprint ? u->blueprint->id : "?";
      if (notDoneFor.insert({static_cast<int>(c.type), bp}).second)
        Logf(LogLevel::Info, "moho64: tick %u: command %s not carried out for %s (unit %#x, %s%s; x%d for the type)",
             sim.tick(), kCommandNames[static_cast<int>(c.type)], bp.c_str(), u->id, mobile ? "mobile" : "immobile",
             c.targetId ? ", entity target" : "", n);
    }
    static std::set<int> logged;
    if (logged.insert(static_cast<int>(c.type)).second)
      Logf(LogLevel::Debug, "moho64: command %s is not carried out yet (finished at once)",
           kCommandNames[static_cast<int>(c.type)]);
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
static bool ThreadAsleepOnMove(const Unit* u) {
  if (u->commands.empty() || u->headState == kNotStarted) return false;
  if (u->commands.front()->type == CommandType::Move) return true;
  const BuildTask* t = u->task;
  return t && (t->moving || (t->sub && t->sub->moving)) && u->motion.hasGoal;
}

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
  // ... and the same destructor (0x618700) calls the navigator's AbortMove when the task ends by itself
  // (queue+0x1c is 0 then, and its table 0x618998 says "abort" for 0): an aircraft at the end of a Move order is
  // stopped at the point one second ahead (CAiNavigatorAir::AbortMove 0x5a4f00), not held at its goal cell
  if (keep->type == CommandType::Move && u->motion.air) AirAbort(sim, u);
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
        if (IsAlive(u) && !u->commands.empty()) u->headState = kNotStarted;
        break;
      } else if (task->pdata && (task->type == CommandType::Patrol || task->type == CommandType::FormPatrol) &&
                 !u->commands.empty()) {
        // a patrol point goes to the back of the queue (the loop); attack-moves are removed
        std::shared_ptr<UnitCommand> keep = u->commands.front();
        PopHead(u);
        u->commands.push_back(keep);
        keep->units.insert(u);
        // the next patrol leg starts a beat later (seen: one beat with neither "Patrolling" nor the drive-through
        // byte between two legs, the new goal the beat after)
        if (IsAlive(u) && !u->commands.empty()) u->headState = kNotStarted;
        break;
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
  Vec3 at = e->position;
  if (e->kind == Entity::Kind::Blip) {
    auto* b = static_cast<ReconBlip*>(e);
    if (b->source && !b->source->destroyQueued) {
      if (b->fake) c.targetFakeId = EntityRef(b);
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
  c.targetId = 0;
  c.targetPoint = -1;
  c.targetViaBlip = false;
  c.targetFakeId = 0;
  if (t.type == 1) {
    if (Entity* e = sim.FindEntityById(t.entity)) {
      c.targetPoint = TargetPointDraw(sim, e);  // CAiTarget::UpdateTarget: a unit / unit blip draws
      SetCommandEntityTarget(c, e);
    } else if (getenv("MOHO64_DEBUG_BLIP")) {
      Logf(LogLevel::Info, "command %u: target entity %#x does not exist", sim.tick(), t.entity);
    }
  } else if (t.type == 2) {
    c.pos = t.pos;
    c.hasPos = true;
  }
}

bool RemoveCommandFromUnit(Sim& sim, Unit* u, UnitCommand& c) {
  auto& q = u->commands;
  size_t i = 0;
  while (i < q.size() && q[i].get() != &c) ++i;
  if (i == q.size()) return false;
  std::shared_ptr<UnitCommand> keep = q[i];
  if (i == 0) {  // the executing head: its task is interrupted (destructor abort -> AbortMove)
    const bool stopped = HeadTaskStopsMotion(u);  // a pod's task: its dtor's Stop is the last word
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
    if (!stopped) MotionStop(u);
    SetMoving(u, false);
  } else {
    q.erase(q.begin() + static_cast<long>(i));
    ReleaseCommandImpl(u, c);
  }
  return true;
}

namespace {
// CommandDatabase::AddIssueData: the CUnitCommand of a stream message (one per message, made for the first unit
// that takes it)
std::shared_ptr<UnitCommand> MakeStreamCommand(Sim& sim, const StreamIssueData& d) {
  auto c = std::make_shared<UnitCommand>();
  if ((d.id >> 24) == 0xffu) {  // AddIssueData 0x6e0db0: an engine-made order (the CPlatoon movers) gets a sim id
    c->id = sim.nextCommandId++;
    sim.commandsById[c->id] = c;
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

std::shared_ptr<UnitCommand> IssueFromStream(Sim& sim, const std::vector<Unit*>& units, const StreamIssueData& d,
                                             bool clear) {
  // types whose move-task destructor does not abort the move when the next command is one of them (0x618998)
  auto keepsMove = [](int t) {
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
  for (Unit* u : units) {
    // ProcessUnitCommand 0x6ef9e0 (outline only, command_stream.md 5.2): dead units refuse
    if (!IsAlive(u)) continue;
    if (sameOrderAgain(u)) continue;
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
      const bool moveAsleep = hadCommands && u->headState != kNotStarted && u->commands.front()->type == CommandType::Move;
      const bool asleep = ThreadAsleepOnMove(u);  // (a task's walking child too)
      u->clearKeepsMove = keep;
      const bool stopped = HeadTaskStopsMotion(u);  // a pod's task: its dtor's Stop is the last word
      ForgetUnitCommands(u);
      u->clearKeepsMove = false;
      // CUnitMoveTask's destructor takes the unit out of state Moving FIRST (0x6187bd: bit 2 of unit+0x4a0) and
      // aborts the move after it (0x618884). The abort's CheckCollisions therefore sees this unit as standing:
      // "a standing unit has priority over a mover". When one Stop reaches two neighbours, the first one torn
      // down puts its record on the neighbour that still moves; the neighbour's own teardown wipes that record
      // and, both standing now, the formation rule decides: one of the two brakes hard, not both.
      if (moveAsleep) SetMoving(u, false);
      if (!keep && hadCommands && !stopped) MotionStop(u);
      if (asleep) sim.ResumeCommandThread(u);
      SetMoving(u, false);
      at = -1;
    }
    if (d.insertBefore != -1 && at < 0) continue;  // not in this unit's queue: it does not get the command
    if (at < 0) {
      u->commands.push_back(c);
    } else {
      u->commands.insert(u->commands.begin() + at, c);
      if (at == 0) {  // a new head: the running one is interrupted and dispatched again later (inferred)
        if (u->task) {
          EndBuildTask(sim, u, *u->task, false);
          u->task = nullptr;
        }
        u->headState = kNotStarted;
      }
    }
    c->units.insert(u);
    AirSpeedThroughEvent(sim, u);  // queue event 0
  }
  return c;
}

void IssueClearCommandsTo(Sim& sim, Unit* u) {
  const bool moveAsleep = ThreadAsleepOnMove(u);  // (a task's walking child too)
  const bool stopped = HeadTaskStopsMotion(u);
  ForgetUnitCommands(u);
  if (!stopped) MotionStop(u);
  SetMoving(u, false);
  if (moveAsleep) sim.ResumeCommandThread(u);
  AttackerStop(sim, u);
}

// The acquire task's auto-initiated Attack (0x5d9415: UNIT_IssueCommand(sim, {u}, Attack on the entity, clear)):
// what is left in the queue goes, the command ctor draws the target point.
std::shared_ptr<UnitCommand> IssueAutoAttack(Sim& sim, Unit* u, Entity* target, bool viaBlip, bool pastDeath,
                                             uint32_t fakeId) {
  if (!IsAlive(u) || !target) return nullptr;
  auto c = std::make_shared<UnitCommand>();
  c->id = sim.nextCommandId++;
  c->type = CommandType::Attack;
  sim.commandsById[c->id] = c;
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
  if (getenv("MOHO64_DEBUG_AUTOATTACK"))
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
  if (!IsAlive(u)) return nullptr;
  auto c = std::make_shared<UnitCommand>();
  c->id = sim.nextCommandId++;
  c->type = CommandType::AssistCommander;
  sim.commandsById[c->id] = c;
  u->commands.push_back(c);
  c->units.insert(u);
  AirSpeedThroughEvent(sim, u);  // queue event 0
  return c;
}

namespace {

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
    // a blip the script passed stands for its unit, as in a player's order (BlackOps' drone carriers:
    // IssueAttack({drone}, unit:GetBlip(army)); seen in Experimental2 at 964: the original's drones fly at the
    // enemy, ours ended the order at once - no weapon could take the blip itself)
    SetCommandEntityTarget(*c, e);
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
    if (!(LiveCommandCaps(L, u) & 0x8u) || !u->isFactoryBuilder || u->dead) continue;  // RULEUCC_Guard
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
  lua_pushboolean(L, u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y ||
                         u->position.z != u->lastPosition.z);
  return 1;
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
    u->unitStates.insert("Immobile");
  } else {
    u->unitStates.erase("Immobile");
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

}  // namespace moho
