// Unit commands and their Lua bindings (see commands.h).
#include "sim/commands.h"
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
  if (u) {
    auto it = c.slots.find(u);
    if (it != c.slots.end()) return it->second;
  }
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) return e->position;
  return c.pos;
}

void SetMoving(Unit* u, bool on) {
  if (on) u->unitStates.insert("Moving");
  else u->unitStates.erase("Moving");
}

// Remove the head command of u (it is done for this unit).
void PopHead(Unit* u) {
  if (u->engageId) ClearEngagement(*Sim::From(u->luaState()), u);
  if (u->task) {
    EndBuildTask(*Sim::From(u->luaState()), u, *u->task, true);
    u->task = nullptr;
  }
  if (u->commands.empty()) return;
  u->motion.speedCap = 0;
  u->commands.front()->units.erase(u);
  u->commands.pop_front();
  u->headState = kNotStarted;
}

bool NextIsMove(const Unit* u) { return u->commands.size() > 1 && MoveLike(u->commands[1]->type); }

void RunPathSearch(Sim& sim, Unit* u, bool /*continuing*/) {
  if (u->commands.empty()) return;
  UnitCommand& c = *u->commands.front();
  const MotionBlueprint& b = *u->motion.bp;
  Vec3 goal = TargetPos(sim, c, u);
  u->motion.speedCap = c.slots.count(u) ? c.formationSpeed : 0;
  std::vector<Vec3> path;
  bool ok = true;
  if (b.motionType == kMotionAir) {
    path.push_back(goal);
  } else {
    ok = sim.navigation().FindPath(b.footprint, u->position, goal, &path);
  }
  if (!ok || path.empty()) {
    u->motion.failed = true;
    u->headState = kRunning;
    return;
  }
  // it drives 3 ticks after the search (air units: the next tick)
  uint32_t drive = sim.tick() + (b.motionType == kMotionAir ? 1 : 3);
  bool through = NextIsMove(u) && MoveLike(c.type);
  MotionSetGoal(sim, u, path, through, drive);
  u->headState = kRunning;
}

void StartHead(Sim& sim, Unit* u) {
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
      if (driving || u->motion.bp->motionType == kMotionAir) {
        RunPathSearch(sim, u, true);  // continuing a move, or flying: no search queue
      } else {
        u->headState = kWaitingPath;
        sim.pathQueue.push_back(u);
      }
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
  for (auto& c : u->commands) c->units.erase(u);
  u->commands.clear();
  u->headState = kNotStarted;
  auto& q = Sim::From(u->luaState())->pathQueue;
  q.erase(std::remove(q.begin(), q.end(), u), q.end());
}

void CommandsBeforeMotion(Sim& sim) {
  const auto& all = sim.units();
  for (size_t i = 0; i < all.size(); ++i) {  // (scripts run from here may add units at the end)
    Unit* u = all[i];
    if (!IsAlive(u)) continue;
    if (u->commands.empty()) continue;
    int& st = u->headState;
    if (st == kNotStarted) StartHead(sim, u);
    if (u->task && st == kRunning) {
      int r = TickBuildTask(sim, u, *u->task);
      if (r != kTaskRunning) {
        u->task = nullptr;
        PopHead(u);
        StartHead(sim, u);
      }
      continue;
    }
    // keep "drive through" up to date when moves were queued behind the current one
    if (st == kRunning && u->motion.hasGoal && !u->commands.empty() && MoveLike(u->commands.front()->type))
      u->motion.passThrough = NextIsMove(u);
    // aggressive moves and patrols stop to fight what they meet
    if (st == kRunning && !u->commands.empty()) {
      CommandType ct = u->commands.front()->type;
      if (ct == CommandType::AggressiveMove || ct == CommandType::FormAggressiveMove || ct == CommandType::Patrol ||
          ct == CommandType::FormPatrol) {
        PatrolEngageTick(sim, u, *u->commands.front());
        if (u->headState == kNotStarted) StartHead(sim, u);
        continue;
      }
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

void CommandsAfterMotion(Sim& sim) {
  const auto& all = sim.units();
  for (size_t i = 0; i < all.size(); ++i) {
    Unit* u = all[i];
    if (!IsAlive(u) || u->commands.empty()) continue;
    if (u->headState != kRunning) continue;
    UnitMotion& m = u->motion;
    if (!m.arrived && !m.failed) continue;
    if (u->task) continue;  // a task command (build, guard, attack, ...) handles its own moves
    if (u->engageId) {  // arrived at the enemy it stopped for, not at the patrol point
      m.arrived = m.failed = false;
      continue;
    }
    UnitCommand& c = *u->commands.front();
    bool patrol = c.type == CommandType::Patrol || c.type == CommandType::FormPatrol;
    std::shared_ptr<UnitCommand> keep = u->commands.front();
    PopHead(u);
    m.arrived = false;
    if (patrol && !m.failed) {  // patrols cycle through their points
      u->commands.push_back(keep);
      keep->units.insert(u);
    }
    m.failed = false;
    if (u->commands.empty()) {
      SetMoving(u, false);
      continue;
    }
    StartHead(sim, u);
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
bool IsFactory(const Unit* u) { return u->bpData && u->bpData->structure && u->bpData->hasBuilder; }

std::shared_ptr<UnitCommand> Issue(lua_State* L, const std::vector<Unit*>& units, CommandType type) {
  auto c = std::make_shared<UnitCommand>();
  Sim* sim = S(L);
  c->id = sim->nextCommandId++;
  c->type = type;
  sim->commandsById[c->id] = c;
  for (Unit* u : units) {
    if (IsFactory(u) && FactoryCommand(type)) {
      u->factoryCommands.push_back(c);
      continue;
    }
    u->commands.push_back(c);
    c->units.insert(u);
  }
  return c;
}

// Formation slots from the formation script (/lua/formations.lua: <name>(units) returns
// { x, z, filter category, row, ... } per slot, x to the right, z forward, in unit-size steps).
// Each slot takes the nearest unassigned unit its filter allows. TODO(M3b): the original's
// CFormationInstance (slot assignment order, travel formation, catch-up speeds).
void PlaceFormation(lua_State* L, UnitCommand& c, const std::vector<Unit*>& units) {
  if (units.size() < 2 || c.formation.empty() || c.formation == "NoFormation") return;
  int top = lua_gettop(L);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/formations.lua");
  if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
    lua_settop(L, top);
    return;
  }
  lua_pushstring(L, c.formation.c_str());
  lua_gettable(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return;
  }
  lua_newtable(L);
  int n = 0;
  for (Unit* u : units) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
  if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
    if (lua_isstring(L, -1)) LogScriptError(lua_tostring(L, -1));
    lua_settop(L, top);
    return;
  }
  int slotsIdx = lua_gettop(L);
  // a formation unit is (largest footprint + 2) world units (formations' categorizeUnits.lua)
  int largest = 1;
  for (Unit* u : units)
    if (u->motion.bp) largest = std::max<int>(largest, std::max(u->motion.bp->footprint.sizeX, u->motion.bp->footprint.sizeZ));
  const float scale = static_cast<float>(largest + 2);
  float sn = dmath::Sin(c.heading), cs = dmath::Cos(c.heading);
  // forward = (sin h, cos h), right = (cos h, -sin h)
  // candidates in entity-id order (ties go to the lowest id: the same on every machine)
  std::vector<Unit*> free(units.begin(), units.end());
  std::sort(free.begin(), free.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  free.erase(std::unique(free.begin(), free.end()), free.end());
  float slowest = 1e30f;
  for (int i = 1; !free.empty(); ++i) {
    lua_rawgeti(L, slotsIdx, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    int sl = lua_gettop(L);
    lua_rawgeti(L, sl, 1);
    float sx = static_cast<float>(lua_tonumber(L, -1)) * scale;
    lua_rawgeti(L, sl, 2);
    float sz = static_cast<float>(lua_tonumber(L, -1)) * scale;
    lua_rawgeti(L, sl, 3);
    const uint64_t* filter = ToCategory(L, -1);
    Vec3 p{c.pos.x + sx * cs + sz * sn, c.pos.y, c.pos.z - sx * sn + sz * cs};
    Unit* best = nullptr;
    float bestD = 1e30f;
    for (Unit* u : free) {
      if (filter && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(filter, u->blueprint->entityIndex)))
        continue;
      float dx = u->position.x - p.x, dz = u->position.z - p.z;
      float d = dx * dx + dz * dz;
      if (d < bestD) {
        bestD = d;
        best = u;
      }
    }
    if (best) {
      c.slots[best] = p;
      free.erase(std::find(free.begin(), free.end(), best));
      if (best->motion.bp) slowest = std::min(slowest, best->motion.bp->maxSpeed);
    }
    lua_settop(L, slotsIdx);
  }
  c.formationSpeed = slowest < 1e29f ? slowest : 0;
  if (getenv("MOHO64_DEBUG_FORM")) for (auto& [u, p] : c.slots) Logf(LogLevel::Debug, "form slot %u %.2f %.2f", u->id, p.x, p.z);
  lua_settop(L, top);
}

// Issue<Type>(units, position or entity target)
template <CommandType T>
int l_IssueTarget(lua_State* L) {
  auto units = UnitsArg(L, 1);
  auto c = Issue(L, units, T);
  if (Entity* e = ToObject<Entity>(L, 2)) {
    c->targetId = EntityRef(e);
    c->pos = e->position;
    c->hasPos = true;
  } else {
    c->hasPos = PosArg(L, 2, &c->pos);
  }
  if (T == CommandType::FormMove || T == CommandType::FormAttack || T == CommandType::FormPatrol ||
      T == CommandType::FormAggressiveMove) {
    if (lua_isstring(L, 3)) c->formation = lua_tostring(L, 3);
    c->heading = static_cast<float>(lua_tonumber(L, 4));
    if (c->hasPos) PlaceFormation(L, *c, units);
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
    else if (Entity* e = ToObject<Entity>(L, i)) c->targetId = EntityRef(e);
    else if (!c->hasPos && lua_istable(L, i)) {
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
  Unit* u = U(L);
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
      lua_pushstring(L, std::to_string(RefToId(c->targetId)).c_str());
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
  std::vector<Vec3> path;
  Sim* sim = S(L);
  if (u->motion.bp->motionType == kMotionAir) path.push_back(p);
  else if (!sim->navigation().FindPath(u->motion.bp->footprint, u->position, p, &path)) return 0;
  MotionSetGoal(*sim, u, path, n->speedThroughGoal, sim->tick() + 3);
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
  PushVector(L, u->motion.hasGoal && !u->motion.path.empty() ? u->motion.path.back() : u->position);
  return 1;
}
int l_nav_GetCurrentTargetPos(lua_State* L) {
  Unit* u = N(L)->unit;
  const UnitMotion& m = u->motion;
  PushVector(L, m.hasGoal && m.pathIndex < m.path.size() ? m.path[m.pathIndex] : u->position);
  return 1;
}
int l_nav_GetStatus(lua_State* L) {
  Unit* u = N(L)->unit;
  int st = 0;  // Idle
  if (u->motion.hasGoal) st = S(L)->tick() < u->motion.driveTick ? 1 : 2;  // Thinking / Steering
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

// GetUnitsInRect(rect) / (x0, z0, x1, z1): units whose position is inside; nil when none.
// TODO(speed pass): spatial grid instead of a scan; the original tests collision bounds.
int l_GetUnitsInRect(lua_State* L) {
  float r[4];
  RectArgs(L, r);
  float x0 = std::min(r[0], r[2]), x1 = std::max(r[0], r[2]), z0 = std::min(r[1], r[3]), z1 = std::max(r[1], r[3]);
  std::vector<Unit*> found;
  S(L)->ForUnitsInRect(x0, z0, x1, z1, [&](Unit* u) {
    if (!u->dead) found.push_back(u);
  });
  if (found.empty()) {
    lua_pushnil(L);
    return 1;
  }
  std::sort(found.begin(), found.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  lua_newtablesized(L, static_cast<int>(found.size()), 0);
  int n = 0;
  for (Unit* u : found) {
    PushObject(L, u);
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
  SetGlobal(L, "IssueTransportUnload", l_IssueTarget<CommandType::TransportUnloadUnits>);
  SetGlobal(L, "IssueFerry", l_IssueTarget<CommandType::Ferry>);
  SetGlobal(L, "IssueBuildMobile", l_IssueBuildMobile);
  SetGlobal(L, "IssueBuildAllMobile", l_IssueBuildMobile);
  SetGlobal(L, "IssueBuildFactory", l_IssueOther<CommandType::BuildFactory>);
  SetGlobal(L, "IssueUpgrade", l_IssueOther<CommandType::Upgrade>);
  SetGlobal(L, "IssueScript", l_IssueScript);
  SetGlobal(L, "IssueSiloBuildTactical", l_IssueOther<CommandType::BuildSiloTactical>);
  SetGlobal(L, "IssueSiloBuildNuke", l_IssueOther<CommandType::BuildSiloNuke>);
  SetGlobal(L, "IssueTransportLoad", l_IssueOther<CommandType::TransportLoadUnits>);
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

}  // namespace moho
