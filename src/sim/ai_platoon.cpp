// Platoons (CPlatoon, CSquad) and the army's platoon bookkeeping (CArmyImpl+0x228). See sim/ai_platoon.h.
//
// Read in the FA exe (build 3839), 2026-10-15:
// - CPlatoon ctor 0x724cc0 (this, sim, army, name, plan): +0x34 sim, +0x38 army, +0x40 squad vector, +0x70 name,
//   +0x8c plan, +0xa8 unique name, +0xc4 formation override, +0xe0 disband-on-idle, +0xe4/+0xe8 kills/losses (int),
//   +0xec/+0xf0 damage dealt/taken (float), +0xf4 the GetPlatoonUnits table (a LuaObject), +0x108 "that table is
//   current"; it ends with the script's OnCreate(plan) (0x724e84).
// - CSquad ctor 0x723e70 (sim, class, formation): +0x08 EntitySet of its units (linked into the entity DB's set
//   list, so a purged entity leaves it), +0x30 class, +0x34 formation name, +0x54 prioritized target categories.
// - Every function that changes a platoon's units clears +0x108 (AddToSquad 0x725280, RemoveUnit 0x7253b0,
//   PullUnassignedUnitsFrom 0x725410, new squad 0x725580, Destroy 0x726210, MakePlatoon/AssignUnitsToPlatoon/
//   FormPlatoon after their AddRange); a purged unit leaving a squad's set does not (inferred: the set does not know
//   its platoon), so GetPlatoonUnits can hand out a table that still holds a destroyed unit until the next change.
// - The orders build an SSTICommandIssueData (ctor 0x552550) and call UNIT_IssueCommand 0x6f12c0 without clear,
//   once per squad (squad classes 1..5, or the one named) or, with a formation override, once for all units.
#include "sim/ai_platoon.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/ai_personality.h"
#include "sim/blueprints.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/enum_arg.h"
#include "sim/formation.h"
#include "sim/intel.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

// ---- small helpers -----------------------------------------------------------------------------------------------

bool IdLess(const Unit* a, const Unit* b) { return a->id < b->id; }

// EntitySetTemplate_Unit::Add 0x57ddd0: a sorted, unique set (ascending entity id).
void SetAdd(std::vector<Unit*>& s, Unit* u) {
  if (!u) return;
  auto it = std::lower_bound(s.begin(), s.end(), u, IdLess);
  if (it != s.end() && *it == u) return;
  s.insert(it, u);
}
void SetAddRange(std::vector<Unit*>& s, const std::vector<Unit*>& add) {
  for (Unit* u : add) SetAdd(s, u);
}
// (ids are unique among the entities that exist, so a binary search by id finds a member)
bool SetHas(const std::vector<Unit*>& s, const Unit* u) {
  auto it = std::lower_bound(s.begin(), s.end(), u, IdLess);
  return it != s.end() && *it == u;
}
bool SetErase(std::vector<Unit*>& s, Unit* u) {
  auto it = std::lower_bound(s.begin(), s.end(), u, IdLess);
  if (it == s.end() || *it != u) return false;
  s.erase(it);
  return true;
}

bool EqualNoCase(const std::string& a, const char* b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// The unit tests the squad code makes through IUnit's vtable: +0x28 dead, +0x2c destroy queued, +0x34 being built,
// +0x30 mobile (the blueprint has a motion type: our structure flag says it has none, inferred).
bool Dead(const Unit* u) { return u->dead; }
bool Gone(const Unit* u) { return u->dead || u->destroyQueued; }
bool Mobile(const Unit* u) { return !(u->bpData && u->bpData->structure); }
bool InCat(const Unit* u, const uint64_t* cat) {
  return cat && u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(cat, u->blueprint->entityIndex);
}

// "%s\n  expected %d args, but got %d" (0xe0a220) / "... between %d and %d args ..." (0xe0a270), with the binding's
// doc string, as every cfunc checks first.
void Args(lua_State* L, const char* doc, int lo, int hi) {
  int n = lua_gettop(L);
  if (lo == hi && n != lo) luaL_error(L, "%s\n  expected %d args, but got %d", doc, lo, n);
  if (lo != hi && (n < lo || n > hi)) luaL_error(L, "%s\n  expected between %d and %d args, but got %d", doc, lo, hi, n);
}

// The enum arguments (sim/enum_arg.h, SCR_GetEnum 0x4d2550): ESquadClass by its value; EAlliance (Neutral 0, Ally 1,
// Enemy 2) as our relation numbers (0 enemy, 1 neutral, 2 ally), and any other value (a number, or names or-ed
// with '|') as -1, which no relation equals (the exe compares the value itself with GetAllianceWith).
int SquadArg(lua_State* L, int idx) { return EnumArgAt(L, idx, kEnumSquad); }
int AllianceArg(lua_State* L, int idx) {
  const int v = EnumArgAt(L, idx, kEnumAlliance);
  return v == 0 ? 1 : v == 1 ? 2 : v == 2 ? 0 : -1;
}
// IArmy::GetAllianceWith 0x579430 in our numbers; the army itself is its own ally.
int RelationTo(const Army* a, const Army* b) {
  if (!a || !b) return 1;
  if (a == b) return 2;
  size_t i = static_cast<size_t>(b->index - 1);
  return i < a->alliance.size() ? a->alliance[i] : 1;
}

// SCR_FromLuaCopy<Vector3f> 0x4d0080: {x, y, z} or a table with x/y/z.
Vec3 VecArg(lua_State* L, int idx) {
  float v[3] = {0, 0, 0};
  if (!lua_istable(L, idx)) luaL_error(L, "expected a vector");
  lua_rawgeti(L, idx, 1);
  const bool arr = lua_isnumber(L, -1);
  lua_pop(L, 1);
  for (int i = 0; i < 3; ++i) {
    if (arr) {
      lua_rawgeti(L, idx, i + 1);
    } else {
      static const char* const k[3] = {"x", "y", "z"};
      lua_pushstring(L, k[i]);
      lua_gettable(L, idx);
    }
    v[i] = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
  }
  return Vec3{v[0], v[1], v[2]};
}
// Moho::IsValid_Vector3f 0x5657f0 (inferred: no NaN component).
bool ValidVec(const Vec3& v) { return v.x == v.x && v.y == v.y && v.z == v.z; }

float NumArg(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "number");
  return static_cast<float>(lua_tonumber(L, idx));
}

// SCR_FromLua_CPlatoon 0x593af0 (errors on a destroyed platoon) and func_GetCPlatoonOpt 0x593a30 (nullptr for a
// destroyed one; errors when the value is no game object at all).
Platoon* P(lua_State* L) { return CheckObject<Platoon>(L, 1); }
Platoon* POpt(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  lua_rawgetcobject(L, idx);
  void** box = static_cast<void**>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!box) luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
  if (!*box) return nullptr;
  Platoon* p = ToObject<Platoon>(L, idx);
  if (!p) luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of ':'?)");
  return p;
}

void PushUnitTable(lua_State* L, const std::vector<Unit*>& units) {
  lua_newtablesized(L, static_cast<int>(units.size()), 0);
  int n = 0;
  for (Unit* u : units) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++n);
  }
}
void PushCommands(lua_State* L, const std::vector<std::shared_ptr<UnitCommand>>& cmds) {
  lua_newtablesized(L, static_cast<int>(cmds.size()), 0);
  int n = 0;
  for (auto& c : cmds) {
    PushUnitCommand(L, c);
    lua_rawseti(L, -2, ++n);
  }
}

// Blueprint Defense.{Air,Surface,Sub,Economy}ThreatLevel (RUnitBlueprint +0x42c/+0x430/+0x434/+0x438), cached per
// blueprint. (A threat-map helper may want the same values: kept here as a small function.)
struct ThreatLevels {
  float v[4] = {0, 0, 0, 0};
};
const ThreatLevels& BlueprintThreat(Sim& sim, const BlueprintInfo* bp) {
  static const SimCache<std::map<const BlueprintInfo*, ThreatLevels>> cacheSlot;
  auto& cache = cacheSlot.For(sim);
  auto it = cache.find(bp);
  if (it != cache.end()) return it->second;
  ThreatLevels t;
  lua_State* L = sim.L();
  const int top = lua_gettop(L);
  sim.blueprints().PushTable(L, *bp);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Defense");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      static const char* const k[4] = {"AirThreatLevel", "SurfaceThreatLevel", "SubThreatLevel", "EconomyThreatLevel"};
      for (int i = 0; i < 4; ++i) {
        lua_pushstring(L, k[i]);
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) t.v[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
      }
    }
  }
  lua_settop(L, top);
  return cache.emplace(bp, t).first->second;
}
// The threat type names of CalculatePlatoonThreat (0x732678..): Air 0, Surface 1, Sub 2, Economy 3; any other name
// sums all four (the switch's default).
int ThreatType(const char* s) {
  std::string v = s ? s : "";
  if (EqualNoCase(v, "Air")) return 0;
  if (EqualNoCase(v, "Surface")) return 1;
  if (EqualNoCase(v, "Sub")) return 2;
  if (EqualNoCase(v, "Economy")) return 3;
  return -1;
}
float AddThreat(float sum, const ThreatLevels& t, int type) {
  if (type >= 0 && type <= 3) return t.v[type] + sum;
  // default: ((sum + air) + surface) + sub) + economy, in that order (0x7327dd)
  float s = t.v[0] + sum;
  s = s + t.v[1];
  s = s + t.v[2];
  return s + t.v[3];
}

}  // namespace

// ---- squads and platoons ---------------------------------------------------------------------------------------

namespace {

void Invalidate(Platoon* p) { p->unitsValid = false; }

// CPlatoon::GetSquad 0x725630
Squad* SquadOf(Platoon* p, int cls) {
  for (auto& s : p->squads)
    if (s->cls == cls) return s.get();
  return nullptr;
}
// CSquad::operator new 0x725580: a new squad at the end of the platoon's list.
Squad* NewSquad(Platoon* p, int cls, const std::string& formation) {
  Invalidate(p);
  auto s = std::make_unique<Squad>();
  s->cls = cls;
  s->formation = formation;
  p->squads.push_back(std::move(s));
  return p->squads.back().get();
}
// sub_725770: every squad's units (Unassigned included) as one EntitySet.
std::vector<Unit*> AllUnits(const Platoon* p) {
  std::vector<Unit*> out;
  for (auto& s : p->squads) SetAddRange(out, s->units);
  return out;
}
size_t UnitCount(const Platoon* p) {
  size_t n = 0;
  for (auto& s : p->squads) n += s->units.size();
  return n;
}
// CPlatoon::RemoveUnit 0x7253b0: out of the first squad that holds it.
void RemoveUnit(Platoon* p, Unit* u) {
  Invalidate(p);
  for (auto& s : p->squads)
    if (SetErase(s->units, u)) return;
}
// sub_725280: into the squad of that class, if the platoon has one.
void AddToSquad(Platoon* p, int cls, const std::vector<Unit*>& units) {
  for (auto& s : p->squads)
    if (s->cls == cls) {
      SetAddRange(s->units, units);
      break;
    }
  Invalidate(p);
}

// CArmyImpl::GetPlatoonByName 0x700470: the first platoon whose unique name matches (case-insensitive).
Platoon* PlatoonByName(Army* a, const char* name) {
  if (!a) return nullptr;
  for (Platoon* p : a->platoons)
    if (EqualNoCase(p->uniqueName, name)) return p;
  return nullptr;
}
// CArmyImpl::GetPlatoonFor 0x7004e0 (IsInPlatoon 0x7251d0, GetSquadClass 0x725220)
Platoon* PlatoonFor(Army* a, const Unit* u, int* cls) {
  if (!a) return nullptr;
  for (Platoon* p : a->platoons)
    for (auto& s : p->squads)
      if (SetHas(s->units, u)) {
        if (cls) *cls = s->cls;
        return p;
      }
  return nullptr;
}
// CArmyImpl::RemoveFromPlatoon 0x700700 / RemoveUnitsFromPlatoons 0x700730
void RemoveFromPlatoon(Army* a, Unit* u) {
  if (Platoon* p = PlatoonFor(a, u, nullptr)) RemoveUnit(p, u);
}
void RemoveUnitsFromPlatoons(Army* a, const std::vector<Unit*>& units) {
  for (Unit* u : units) RemoveFromPlatoon(a, u);
}

// CPlatoon::PullUnassignedUnitsFrom 0x725410 (this = the pool): every squad of `from` is emptied into the pool's
// Unassigned squad and deleted; without an Unassigned squad nothing happens.
void PullUnassignedUnitsFrom(Platoon* pool, Platoon* from) {
  Invalidate(from);
  Squad* dst = SquadOf(pool, kSquadUnassigned);
  if (!dst) return;
  Invalidate(pool);
  for (auto& s : from->squads) {
    std::vector<Unit*> units = s->units;
    s->units.clear();
    SetAddRange(dst->units, units);
    Invalidate(from);
  }
  from->squads.clear();
}

// The end of a platoon (CArmyImpl::DisbandPlatoon 0x7005cb / CleanUpPlatoons 0x7009b2): the script's OnDestroy,
// then the object is deleted (vt+8): its Lua object no longer has an engine object (PlatoonExists false).
void DeletePlatoon(Sim& sim, Platoon* p) {
  lua_State* L = sim.L();
  sim.CallMethod(L, p, "OnDestroy", 0);
  p->squads.clear();
  if (p->unitsRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p->unitsRef);
  p->unitsRef = LUA_NOREF;
  p->unitsValid = false;
  p->army = nullptr;
  p->UnbindLua();
}

// CArmyImpl::DisbandPlatoon 0x700540: the pool takes its units; the pool itself is never disbanded.
void DisbandPlatoon(Sim& sim, Army* a, Platoon* p) {
  Platoon* pool = PlatoonByName(a, "ArmyPool");
  auto& v = a->platoons;
  auto it = std::find(v.begin(), v.end(), p);
  if (it == v.end() || p == pool) return;
  if (pool) PullUnassignedUnitsFrom(pool, p);
  v.erase(it);
  DeletePlatoon(sim, p);
}
// CArmyImpl::DisbandPlatoonUniquelyNamed 0x7005f0: the first platoon of that unique name (the pool too, if named).
void DisbandPlatoonUniquelyNamed(Sim& sim, Army* a, const char* name) {
  Platoon* pool = PlatoonByName(a, "ArmyPool");
  auto& v = a->platoons;
  for (size_t i = 0; i < v.size(); ++i) {
    Platoon* p = v[i];
    if (!EqualNoCase(p->uniqueName, name)) continue;
    if (pool && p != pool) PullUnassignedUnitsFrom(pool, p);
    v.erase(v.begin() + static_cast<long>(i));
    DeletePlatoon(sim, p);
    return;
  }
}

// CSquad::UnitHasOrder 0x7247a0 (in fact "no live unit has an order") and CPlatoon::SquadsHaveOrders 0x729fe0 (in
// fact "every squad 1..5 is idle").
bool SquadIdle(const Squad* s) {
  for (Unit* u : s->units)
    if (!Dead(u) && !u->commands.empty()) return false;
  return true;
}
bool AllSquadsIdle(Platoon* p) {
  for (int c = 1; c < 6; ++c)
    if (Squad* s = SquadOf(p, c))
      if (!SquadIdle(s)) return false;
  return true;
}

// CPlatoon::SquadHasState 0x729f90: some live unit of squad cls (6: of squads 1..5) is in that unit state.
bool SquadHasState(Platoon* p, int cls, const char* state) {
  for (int c = 1; c < 6; ++c) {
    if (cls != kSquadAll && cls != c) continue;
    if (Squad* s = SquadOf(p, c))
      for (Unit* u : s->units)
        if (!Dead(u) && u->unitStates.count(state)) return true;
  }
  return false;
}

// CSquad::CanAttackTarget 0x7244e0: a live unit with an attacker (u+0x544: a unit with weapons) whose attacker's
// vt+0x30 (0x5d6f40) takes the entity: not while being built; any weapon's PickTargetPoint 0x6d5590 (which draws a
// target point of a seabed unit for an above/below-water-only weapon, as our Targetable does).
bool SquadCanAttack(Sim& sim, const Squad* s, Entity* target) {
  if (!target) return false;
  AiTarget t;
  t.type = 1;
  t.entityId = EntityRef(target);
  for (Unit* u : s->units) {
    if (Dead(u) || u->weapons.empty()) continue;
    if (u->beingBuilt) continue;
    for (UnitWeapon* w : u->weapons)
      if (w->bp && WeaponCanAttackTarget(sim, w, t)) return true;
  }
  return false;
}

// CSquad::GetCenter 0x724020: the mean of the squad's unit positions (summed in set order, times 1/n); (0,0,0) for
// an empty squad.
Vec3 SquadCenter(const Squad* s) {
  Vec3 c{0, 0, 0};
  if (s->units.empty()) return c;
  for (Unit* u : s->units) {
    c.x = u->position.x + c.x;
    c.y = u->position.y + c.y;
    c.z = u->position.z + c.z;
  }
  const float inv = 1.0f / static_cast<float>(s->units.size());
  return Vec3{c.x * inv, c.y * inv, c.z * inv};
}

// The squad's unit list (copy_CSquadUnits_into_EntitySet 0x723a50): all of them, dead ones too.
std::vector<Unit*> SquadUnits(const Squad* s) { return s->units; }

}  // namespace

Platoon* Sim::CreatePlatoon(lua_State* L, Army* army, const std::string& name, const std::string& plan) {
  // CArmyImpl::MakePlatoon 0x700410 = new CPlatoon (0x72a300 -> ctor 0x724cc0), then appended to the army's list.
  int top = lua_gettop(L);
  if (!PushImport(L, "/lua/platoon.lua")) return nullptr;
  lua_pushstring(L, "Platoon");
  lua_gettable(L, -2);
  auto owned = std::make_unique<Platoon>();
  Platoon* p = owned.get();
  p->army = army;
  p->name = name;
  p->plan = plan;
  lua_newtable(L);
  lua_insert(L, -2);
  lua_setmetatable(L, -2);
  BindObject(L, -1, p);
  owned_.push_back(std::move(owned));
  lua_settop(L, top);
  // the ctor ends with its script's OnCreate(plan) (0x724e84), the army pool's included
  lua_pushstring(L, plan.c_str());
  CallMethod(L, p, "OnCreate", 1);
  lua_settop(L, top);
  if (army) army->platoons.push_back(p);
  return p;
}

Platoon* CreateArmyPool(Sim& sim, lua_State* L, Army* army) {
  // CArmyImpl ctor 0x6ff179: MakePlatoon('Pool', 'PoolAI') (the script's OnCreate forks platoon.lua's empty PoolAI),
  // an Unassigned squad without formation (0x725580(0, null)), then unique name 'ArmyPool' (0x6ff19c).
  Platoon* p = sim.CreatePlatoon(L, army, "Pool", "PoolAI");
  if (!p) return nullptr;
  NewSquad(p, kSquadUnassigned, "");
  p->uniqueName = "ArmyPool";
  return p;
}

void AssignNewUnitToPool(Army* army, Unit* u) {
  // Unit ctor 0x6a63e6: army vt+0x78 = CArmyImpl::AssignUnitsToPlatoon 0x7006c0({u}, 'ArmyPool'): out of any
  // platoon, then into the Unassigned squad of the platoon named 'ArmyPool' (none: nowhere). No script callback.
  if (!army || !u) return;
  RemoveFromPlatoon(army, u);
  if (Platoon* pool = PlatoonByName(army, "ArmyPool")) AddToSquad(pool, kSquadUnassigned, {u});
}

void PlatoonsForgetUnit(Sim& sim, Unit* u) {
  // The squads' sets are EntitySets linked into the entity DB's set list: a purged entity leaves all of them (the
  // platoons' +0x108 is not cleared, inferred). A unit is in at most one squad; its own army's platoons first.
  if (u->army)
    for (Platoon* p : u->army->platoons)
      for (auto& s : p->squads)
        if (SetErase(s->units, u)) return;
  for (auto& a : sim.armies())
    for (Platoon* p : a->platoons)
      for (auto& s : p->squads)
        if (SetErase(s->units, u)) return;
}

void PlatoonsNotifyUpgrade(Unit* from, Unit* to) {
  // NotifyUpgrade 0x6cd0c7..0x6cd119 (army = from's): `to` leaves its platoon; `from` leaves its own and `to` goes
  // into the same squad class of it (sub_7252D0: into that class's squad if the platoon has one).
  Army* a = from ? from->army : nullptr;
  if (!a || !to) return;
  int cls = 0;
  if (Platoon* p = PlatoonFor(a, to, &cls)) RemoveUnit(p, to);
  cls = 0;
  if (Platoon* p = PlatoonFor(a, from, &cls)) {
    RemoveUnit(p, from);
    AddToSquad(p, cls, {to});
  }
}

void CleanUpPlatoons(Sim& sim, Army& army) {
  // CArmyImpl::CleanUpPlatoons 0x700820. A platoon set to disband on idle whose squads 1..5 have no orders gives its
  // units to the pool and goes; a platoon without a unique name goes once it has no units; the ones removed are told
  // OnDestroy and deleted after the pass, in list order.
  std::vector<Platoon*> gone;
  auto& v = army.platoons;
  for (size_t i = 0; i < v.size();) {
    Platoon* p = v[i];
    bool disband = false;
    if (p->disbandOnIdle && AllSquadsIdle(p)) {
      Platoon* pool = PlatoonByName(&army, "ArmyPool");
      if (pool && pool != p) PullUnassignedUnitsFrom(pool, p);
      disband = true;
    }
    bool remove;
    if (!p->uniqueName.empty()) remove = disband;  // __stricmp(name, "") != 0
    else remove = UnitCount(p) == 0 || disband;
    if (remove) {
      gone.push_back(p);
      v.erase(v.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
  for (Platoon* p : gone) DeletePlatoon(sim, p);
}

void PlatoonStatDamage(Unit* dealer, Unit* receiver, float damage) {
  // DealDamage stats (damage.md 4): the dealing unit's platoon +0xec, the damaged unit's platoon +0xf0
  if (dealer)
    if (Platoon* p = PlatoonFor(dealer->army, dealer, nullptr)) p->damageDealt += damage;
  if (receiver)
    if (Platoon* p = PlatoonFor(receiver->army, receiver, nullptr)) p->damageTaken += damage;
}
void PlatoonStatKill(Unit* victim, Unit* killer) {
  // Unit kill stats (damage.md 14, 15): the victim's platoon +0xe8 += 1; an enemy unit killer's platoon +0xe4 += 1
  if (victim)
    if (Platoon* p = PlatoonFor(victim->army, victim, nullptr)) ++p->losses;
  if (killer)
    if (Platoon* p = PlatoonFor(killer->army, killer, nullptr)) ++p->kills;
}

// ---- orders --------------------------------------------------------------------------------------------------

namespace {

// SSTICommandIssueData (ctor 0x552550: no id, no insert, formation -1, quaternion (1,0,0,0), scale 1) for
// UNIT_IssueCommand 0x6f12c0 without clear. withForm: the platoon code's formation fields (index, a zero
// quaternion from Zeroed<Quaternion> 0x4ebec0 / the static 0x10c7a58, scale 1).
std::shared_ptr<UnitCommand> Order(Sim& sim, const std::vector<Unit*>& units, CommandType type, const StreamTarget& t,
                                   bool withForm = false, int formIndex = -1) {
  StreamIssueData d;
  d.type = static_cast<int>(type);
  d.target = t;
  if (withForm) {
    d.formIndex = formIndex;
    d.qw = d.qx = d.qy = d.qz = 0;
    d.scale = 1;
  }
  return IssueFromStream(sim, units, d, false);
}
StreamTarget Ground(const Vec3& p) {
  StreamTarget t;
  t.type = 2;
  t.pos = p;
  return t;
}
StreamTarget OnEntity(const Entity* e) {
  StreamTarget t;
  t.type = 1;
  t.entity = e ? e->id : 0xf0000000u;
  return t;
}
// The formation DB's index of a formation name for these units (sim+0x980 vt+8; ours FormationScriptIndex).
int FormIndex(Sim& sim, const std::string& name, const std::vector<Unit*>& units) {
  if (name.empty()) return -1;
  return FormationScriptIndex(sim.L(), name, units);
}
// The order of Patrol / MoveToLocation / MoveToTarget / AttackTarget for one unit set: the formation type with the
// formation when the name resolves and there are 2+ units (the set's size: dead units count), else the plain type.
std::shared_ptr<UnitCommand> FormOrPlain(Sim& sim, const std::vector<Unit*>& units, const std::string& formation,
                                         CommandType plain, CommandType form, const StreamTarget& t) {
  const int idx = FormIndex(sim, formation, units);
  if (idx >= 0 && units.size() > 1) return Order(sim, units, form, t, true, idx);
  return Order(sim, units, plain, t);
}
// The surface height the movers put the point at (STIMap::GetElevation, the water's if higher).
void ToSurface(Sim& sim, Vec3& p) {
  if (const TerrainMap* m = sim.map()) p.y = m->SurfaceHeight(p.x, p.z);
}

// Run f(squad) for squad classes lo..hi that cls names (kSquadAll: all of them) and that the platoon has.
template <class F>
void ForSquads(Platoon* p, int cls, int lo, int hi, F&& f) {
  for (int c = lo; c <= hi; ++c) {
    if (cls != kSquadAll && cls != c) continue;
    if (Squad* s = SquadOf(p, c)) f(s);
  }
}

// CPlatoon::Patrol 0x726420 (returns nothing)
void Patrol(Sim& sim, Platoon* p, Vec3 pos, int cls) {
  ToSurface(sim, pos);
  if (!p->formationOverride.empty()) {
    FormOrPlain(sim, AllUnits(p), p->formationOverride, CommandType::Patrol, CommandType::FormPatrol, Ground(pos));
    return;
  }
  ForSquads(p, cls, 1, 5, [&](Squad* s) {
    FormOrPlain(sim, SquadUnits(s), s->formation, CommandType::Patrol, CommandType::FormPatrol, Ground(pos));
  });
}

// CPlatoon::AggressiveMoveToLocation 0x7268e0: no unit-count test; the type is FormAggressiveMove when the name
// resolves (setge + 0x23), and the index field is the index or 0 (cmovg), so a plain AggressiveMove carries index 0.
void AggressiveMove(Sim& sim, Platoon* p, Vec3 pos, int cls, std::vector<std::shared_ptr<UnitCommand>>& out) {
  ToSurface(sim, pos);
  auto issue = [&](const std::vector<Unit*>& units, const std::string& formation) {
    const int idx = FormIndex(sim, formation, units);
    auto c = Order(sim, units, idx >= 0 ? CommandType::FormAggressiveMove : CommandType::AggressiveMove, Ground(pos),
                   true, idx > 0 ? idx : 0);
    if (c) out.push_back(c);
  };
  if (!p->formationOverride.empty()) {
    issue(AllUnits(p), p->formationOverride);
    return;
  }
  ForSquads(p, cls, 1, 5, [&](Squad* s) { issue(SquadUnits(s), s->formation); });
}

// CPlatoon::MoveToLocation 0x726de0 / MoveToTarget 0x727740. With useTransports: one plain Move for the named
// squads' units plus every live TRANSPORTATION unit of squads 1..5.
void MoveTo(Sim& sim, Platoon* p, const StreamTarget& t, bool useTransports, int cls,
            std::vector<std::shared_ptr<UnitCommand>>& out) {
  if (useTransports) {
    std::vector<Unit*> transports, units;
    for (int c = 1; c < 6; ++c) {
      Squad* s = SquadOf(p, c);
      if (!s) continue;
      for (Unit* u : s->units)
        if (!Dead(u) && BpInCategory(sim, u->blueprint, "TRANSPORTATION")) SetAdd(transports, u);
      if (cls == kSquadAll || cls == c) SetAddRange(units, s->units);
    }
    if (!units.empty()) {
      SetAddRange(units, transports);
      if (auto c = Order(sim, units, CommandType::Move, t)) out.push_back(c);
    }
    return;
  }
  if (!p->formationOverride.empty()) {
    if (auto c = FormOrPlain(sim, AllUnits(p), p->formationOverride, CommandType::Move, CommandType::FormMove, t))
      out.push_back(c);
    return;
  }
  ForSquads(p, cls, 1, 5, [&](Squad* s) {
    if (auto c = FormOrPlain(sim, SquadUnits(s), s->formation, CommandType::Move, CommandType::FormMove, t))
      out.push_back(c);
  });
}

// CPlatoon::AttackTarget 0x727f50: squads Attack and Artillery only (the loop runs 1..2).
void AttackTarget(Sim& sim, Platoon* p, Entity* target, int cls, std::vector<std::shared_ptr<UnitCommand>>& out) {
  const StreamTarget t = OnEntity(target);
  if (!p->formationOverride.empty()) {
    if (auto c = FormOrPlain(sim, AllUnits(p), p->formationOverride, CommandType::Attack, CommandType::FormAttack, t))
      out.push_back(c);
    return;
  }
  ForSquads(p, cls, 1, 2, [&](Squad* s) {
    if (auto c = FormOrPlain(sim, SquadUnits(s), s->formation, CommandType::Attack, CommandType::FormAttack, t))
      out.push_back(c);
  });
}

// func_IssueGuardTargetToPlatoon 0x728420: Guard without formation; squads Attack and Artillery only.
void GuardTarget(Sim& sim, Platoon* p, Entity* target, int cls, std::vector<std::shared_ptr<UnitCommand>>& out) {
  const StreamTarget t = OnEntity(target);
  if (!p->formationOverride.empty()) {
    if (auto c = Order(sim, AllUnits(p), CommandType::Guard, t)) out.push_back(c);
    return;
  }
  ForSquads(p, cls, 1, 2, [&](Squad* s) {
    if (auto c = Order(sim, SquadUnits(s), CommandType::Guard, t)) out.push_back(c);
  });
}

// CPlatoon::FerryToLocation 0x728700: the live, finished TRANSPORTATION units of squads 1..5 get one Ferry.
void FerryToLocation(Sim& sim, Platoon* p, Vec3 pos, std::vector<std::shared_ptr<UnitCommand>>& out) {
  ToSurface(sim, pos);
  std::vector<Unit*> units;
  for (int c = 1; c < 6; ++c)
    if (Squad* s = SquadOf(p, c))
      for (Unit* u : s->units)
        if (!Dead(u) && !u->beingBuilt && BpInCategory(sim, u->blueprint, "TRANSPORTATION")) SetAdd(units, u);
  if (!units.empty())
    if (auto c = Order(sim, units, CommandType::Ferry, Ground(pos))) out.push_back(c);
}

// CPlatoon::UnloadAllAtLocation 0x729690: the live, finished TRANSPORTATION or CARRIER units of squads 1..5 get one
// TransportUnloadUnits at the point as given.
void UnloadAllAtLocation(Sim& sim, Platoon* p, const Vec3& pos, std::vector<std::shared_ptr<UnitCommand>>& out) {
  std::vector<Unit*> units;
  for (int c = 1; c < 6; ++c)
    if (Squad* s = SquadOf(p, c))
      for (Unit* u : s->units)
        if (!Dead(u) && !u->beingBuilt &&
            (BpInCategory(sim, u->blueprint, "TRANSPORTATION") || BpInCategory(sim, u->blueprint, "CARRIER")))
          SetAdd(units, u);
  if (!units.empty())
    if (auto c = Order(sim, units, CommandType::TransportUnloadUnits, Ground(pos))) out.push_back(c);
}

// CPlatoon::UseFerryBeacon 0x729a70 (ferry_commands.md 1.5): the live, finished, mobile units of squads 1..5 in the
// category get TransportLoadUnits onto the beacon.
void UseFerryBeacon(Sim& sim, Platoon* p, const uint64_t* cat, Entity* beacon,
                    std::vector<std::shared_ptr<UnitCommand>>& out) {
  std::vector<Unit*> units;
  for (int c = 1; c < 6; ++c)
    if (Squad* s = SquadOf(p, c))
      for (Unit* u : s->units)
        if (!Dead(u) && !u->beingBuilt && Mobile(u) && InCat(u, cat)) SetAdd(units, u);
  if (!units.empty())
    if (auto c = Order(sim, units, CommandType::TransportLoadUnits, OnEntity(beacon))) out.push_back(c);
}

// CPlatoon::UseTeleporter 0x729d20: all units of the named squads (dead ones too) and the teleporter itself get
// TransportLoadUnits onto the teleporter.
void UseTeleporter(Sim& sim, Platoon* p, Unit* teleporter, int cls, std::vector<std::shared_ptr<UnitCommand>>& out) {
  std::vector<Unit*> units;
  ForSquads(p, cls, 1, 5, [&](Squad* s) { SetAddRange(units, s->units); });
  if (units.empty() || !teleporter) return;
  SetAdd(units, teleporter);
  if (auto c = Order(sim, units, CommandType::TransportLoadUnits, OnEntity(teleporter))) out.push_back(c);
}

// CPlatoon::Stop 0x7261b0 -> CSquad::Stop 0x724820: every live unit of the squad (6: of every squad but Unassigned)
// gets ClearCommandQueue and its attacker's Stop - what IssueClearCommands does.
void Stop(Sim& sim, Platoon* p, int cls) {
  for (auto& s : p->squads) {
    if (cls == kSquadAll ? s->cls == kSquadUnassigned : s->cls != cls) continue;
    std::vector<Unit*> units = s->units;
    for (Unit* u : units)
      if (!Dead(u)) IssueClearCommandsTo(sim, u);
    if (cls != kSquadAll) break;
  }
}

// sub_726210 (CPlatoon:Destroy): the squads' units leave the platoon and the live ones get DestroySelf.
void DestroyUnits(Sim& sim, Platoon* p, int cls) {
  Invalidate(p);
  std::vector<Unit*> live;
  for (int c = 1; c < 6; ++c) {
    if (cls != kSquadAll && cls != c) continue;
    Squad* s = SquadOf(p, c);
    if (!s) continue;
    for (Unit* u : s->units)
      if (!Dead(u)) SetAdd(live, u);
    s->units.clear();
  }
  StreamTarget none;
  Order(sim, live, CommandType::DestroySelf, none);
}

// ---- the Find* queries ----

// func_GetUnitsAroundPoint 0x57b290 (brain, category, point, radius, alliance): the entity DB's units around the
// point (0x722350, mask 0x100) that are not dead or destroy-queued, of that alliance, known to the brain's army
// (recon DB vt+0x4c: a blip) unless its own, in the category; as an EntitySet. (The circle test of 0x722350 was not
// read: taken as our GetUnitsAroundPoint's 2D one.)
std::vector<Unit*> UnitsAroundPoint(Sim& sim, Army* army, const uint64_t* cat, const Vec3& p, float r, int rel) {
  std::vector<Unit*> found;
  const int ai = army ? army->index - 1 : -1;
  sim.ForUnitsInRect(p.x - r, p.z - r, p.x + r, p.z + r, [&](Unit* u) {
    if (Gone(u)) return;
    const float dx = u->position.x - p.x, dz = u->position.z - p.z;
    if (dx * dx + dz * dz > r * r) return;
    if (RelationTo(army, u->army) != rel) return;
    if (u->army != army && !ArmyHasBlip(u, ai)) return;
    if (!InCat(u, cat)) return;
    found.push_back(u);
  });
  std::sort(found.begin(), found.end(), IdLess);
  return found;
}

// CPlatoon::FindPrioritizedUnit 0x7259d0: of the units around the point, the one in the earliest category of the
// squad's priority list, the nearest (3D distance squared) among those; ties keep the first (lowest id).
Unit* FindPrioritized(Sim& sim, Platoon* p, int cls, int rel, bool canAttack, const Vec3& pos, float radius) {
  Squad* s = SquadOf(p, cls);
  if (!s) return nullptr;
  Unit* best = nullptr;
  int bestPri = 99999;
  float bestDist = INFINITY;  // 0x10b9680 = +inf (static init 0xbdab00)
  const uint64_t* all = nullptr;  // the rules' ALLUNITS (sim+0x8c8 vt+0x58)
  {
    lua_State* L = sim.L();
    const int top = lua_gettop(L);
    lua_getglobal(L, "categories");
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "ALLUNITS");
      lua_gettable(L, -2);
      all = ToCategory(L, -1);
    }
    std::vector<Unit*> around = UnitsAroundPoint(sim, p->army, all, pos, radius, rel);
    lua_settop(L, top);
    for (Unit* u : around) {
      if (canAttack && !SquadCanAttack(sim, s, u)) continue;
      int pri = 1;
      bool found = false;
      for (auto& c : s->priorities) {
        if (InCat(u, c.data())) {
          found = true;
          break;
        }
        if (++pri > bestPri) break;
      }
      if (!found || pri > bestPri) continue;
      const float dy = pos.y - u->position.y, dx = pos.x - u->position.x, dz = pos.z - u->position.z;
      const float d = (dz * dz + dy * dy) + dx * dx;
      if (pri < bestPri || (pri == bestPri && bestDist > d)) {
        best = u;
        bestPri = pri;
        bestDist = d;
      }
    }
  }
  return best;
}

// CPlatoon::FindClosestUnitToPos 0x725cf0: over the units the platoon's army holds blips of (the recon DB's blip
// list, vt+0x20 / vt+0x1c, blip+0x270 = the source unit): not dead, in the category, of that alliance; mode 0 the
// nearest (3D), 1 the farthest, 2 the highest BuildCostMass + BuildCostEnergy. A value is taken when better than the
// best or while the best is still unset (-1). (Our blips are per unit, not a per-army list: the order is our unit
// list's, inferred to matter only for exact ties.)
Unit* FindClosestToPos(Sim& sim, Platoon* p, int cls, const uint64_t* cat, int mode, int rel, bool canAttack,
                       const Vec3& pos) {
  Squad* s = SquadOf(p, cls);
  if (!s) return nullptr;
  Army* a = p->army;
  const int ai = a ? a->index - 1 : -1;
  Unit* best = nullptr;
  float bestVal = -1.0f;  // 0xe4f6e8
  for (Unit* u : sim.units()) {
    if (!a || u->army == a || !ArmyHasBlip(u, ai)) continue;
    if (Dead(u) || !InCat(u, cat)) continue;
    if (RelationTo(a, u->army) != rel) continue;
    float v;
    bool take;
    if (mode == 0 || mode == 1) {
      const float dz = u->position.z - pos.z, dx = u->position.x - pos.x, dy = u->position.y - pos.y;
      v = (dy * dy + dz * dz) + dx * dx;
      take = mode == 0 ? (bestVal > v || 0.0f > bestVal) : (v > bestVal || 0.0f > bestVal);
    } else if (mode == 2) {
      v = (u->bpData ? u->bpData->buildCostMass : 0) + (u->bpData ? u->bpData->buildCostEnergy : 0);
      take = v > bestVal || 0.0f > bestVal;
    } else {
      continue;  // (mode 3, the value of the units around it, is no cfunc's: not built)
    }
    if (!take) continue;
    if (canAttack && !SquadCanAttack(sim, s, u)) continue;
    best = u;
    bestVal = v;
  }
  return best;
}
// CPlatoon::FindClosestUnit 0x726120: relative to the squad's centre; nothing for a centre at (0,0,0) (0x726164..0x726177:
// the centre, 0x724020, against the zero vector by memcmp 0x4f0a50).
Unit* FindFromSquad(Sim& sim, Platoon* p, int cls, const uint64_t* cat, int mode, int rel, bool canAttack) {
  Squad* s = SquadOf(p, cls);
  if (!s) return nullptr;
  const Vec3 c = SquadCenter(s);
  if (vm::BitsZero(c)) return nullptr;
  return FindClosestToPos(sim, p, cls, cat, mode, rel, canAttack, c);
}
// CAiBrain::CenterOfArmy 0x57a510: the mean position of the army's live units in STRUCTURE - MOBILE (army vt+0x9c,
// EntityCategory::Sub 0x557710); (0,0,0) without any.
Vec3 CenterOfArmy(Sim& sim, Army* a) {
  Vec3 c{0, 0, 0};
  if (!a) return c;
  int n = 0;
  for (Unit* u : a->units) {
    if (Gone(u)) continue;
    if (!BpInCategory(sim, u->blueprint, "STRUCTURE") || BpInCategory(sim, u->blueprint, "MOBILE")) continue;
    c.x = u->position.x + c.x;
    c.y = u->position.y + c.y;
    c.z = u->position.z + c.z;
    ++n;
  }
  if (n == 0) return c;
  const float inv = 1.0f / static_cast<float>(n);
  return Vec3{c.x * inv, c.y * inv, c.z * inv};
}

}  // namespace

// ---- Lua: CPlatoon ---------------------------------------------------------------------------------------------

namespace {

// IsOpponentAIRunning 0x72b890: the sim var AI_RunOpponentAI (0x10ad724, registered 0xbcb080 with 1).
int l_IsOpponentAIRunning(lua_State* L) {
  Args(L, "CPlatoon:IsOpponentAIRunning()", 1, 1);
  P(L);
  lua_pushboolean(L, 1);
  return 1;
}
// GetPersonality 0x72b9f0: the brain's personality (brain+0x3c), nil without one.
int l_GetPersonality(lua_State* L) {
  Args(L, "CPlatoon:GetPersonality()", 1, 1);
  Platoon* p = P(L);
  AiBrain* b = p->army ? p->army->brain : nullptr;
  PushObject(L, b ? b->personality : nullptr);
  return 1;
}
int l_GetBrain(lua_State* L) {  // 0x72bb50
  Args(L, "CPlatoon:GetBrain()", 1, 1);
  Platoon* p = P(L);
  PushObject(L, p->army ? p->army->brain : nullptr);
  return 1;
}
int l_GetFactionIndex(lua_State* L) {  // 0x72bc90: army+0x174 + 1
  Args(L, "CPlatoon:GetFactionIndex()", 1, 1);
  Platoon* p = P(L);
  lua_pushnumber(L, static_cast<float>((p->army ? p->army->faction : 0) + 1));
  return 1;
}
int l_UniquelyNamePlatoon(lua_State* L) {  // 0x72bde0: only a string is taken
  Args(L, "CPlatoon:UniquelyNamePlatoon()", 2, 2);
  Platoon* p = P(L);
  if (lua_isstring(L, 2)) p->uniqueName = lua_tostring(L, 2);
  return 0;
}
int l_GetPlatoonUniqueName(lua_State* L) {  // 0x72bf70
  Args(L, "CPlatoon:GetPlatoonUniqueName()", 1, 1);
  lua_pushstring(L, P(L)->uniqueName.c_str());
  return 1;
}
// GetAIPlan 0x72c0c0 returns the BRAIN's plan string (brain+0x40), which only the engine's CAiBrain:SetCurrentPlan
// (0x586390) writes; FAF's AI brains override SetCurrentPlan in Lua, so it stays '' (not the platoon's plan).
int l_GetAIPlan(lua_State* L) {
  Args(L, "CPlatoon:GetAIPlan()", 1, 1);
  P(L);
  lua_pushstring(L, "");
  return 1;
}
// SwitchAIPlan 0x72c210 -> CPlatoon::SwitchAIPlan 0x725150: a different plan: OnDestroy, the new plan, OnCreate(plan).
int l_SwitchAIPlan(lua_State* L) {
  Args(L, "CPlatoon:SwitchAIPlan()", 2, 2);
  Platoon* p = P(L);
  if (!lua_isstring(L, 2)) return 0;
  std::string plan = lua_tostring(L, 2);
  if (plan == p->plan) return 0;
  Sim* sim = S(L);
  sim->CallMethod(L, p, "OnDestroy", 0);
  p->plan = plan;
  lua_pushstring(L, plan.c_str());
  sim->CallMethod(L, p, "OnCreate", 1);
  return 0;
}
// GetPlatoonPosition 0x72c380: the mean of all units' positions (set order, times 1/n); nil without units.
int l_GetPlatoonPosition(lua_State* L) {
  Args(L, "CPlatoon:GetPlatoonPosition()", 1, 1);
  std::vector<Unit*> units = AllUnits(P(L));
  if (units.empty()) {
    lua_pushnil(L);
    return 1;
  }
  Vec3 c{0, 0, 0};
  for (Unit* u : units) {
    c.x = c.x + u->position.x;
    c.y = u->position.y + c.y;
    c.z = u->position.z + c.z;
  }
  const float inv = 1.0f / static_cast<float>(units.size());
  PushVector(L, c.x * inv, c.y * inv, c.z * inv);
  return 1;
}
int l_GetSquadPosition(lua_State* L) {  // 0x72c640
  Args(L, "CPlatoon:GetSquadPosition()", 2, 2);
  Platoon* p = P(L);
  Squad* s = SquadOf(p, SquadArg(L, 2));
  if (!s) {
    lua_pushnil(L);
    return 1;
  }
  const Vec3 c = SquadCenter(s);
  PushVector(L, c.x, c.y, c.z);
  return 1;
}
int l_GetSquadUnits(lua_State* L) {  // 0x72c850 -> CSquad::GetUnits 0x72b700 (FillLuaTableWithEntities 0x6934e0)
  Args(L, "CPlatoon:GetSquadUnits()", 2, 2);
  Platoon* p = P(L);
  Squad* s = SquadOf(p, SquadArg(L, 2));
  if (!s) {
    lua_pushnil(L);
    return 1;
  }
  PushUnitTable(L, s->units);
  return 1;
}
// GetPlatoonUnits 0x72ca30: the table at +0xf4, rebuilt only when +0x108 is clear (so callers share it); a destroyed
// platoon gives a new empty table.
int l_GetPlatoonUnits(lua_State* L) {
  Args(L, "CPlatoon:GetPlatoonUnits()", 1, 1);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_newtable(L);
    return 1;
  }
  if (!p->unitsValid || p->unitsRef == LUA_NOREF) {
    if (p->unitsRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, p->unitsRef);
    PushUnitTable(L, AllUnits(p));
    p->unitsRef = luaL_ref(L, LUA_REGISTRYINDEX);
    p->unitsValid = true;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, p->unitsRef);
  return 1;
}

// A template's entry: { unit id or category, min count, max count, squad, formation }.
struct TemplateItem {
  bool isId = false;
  std::string id;
  const uint64_t* cat = nullptr;
};
TemplateItem ItemOf(lua_State* L, int entry) {
  TemplateItem t;
  lua_rawgeti(L, entry, 1);
  if (lua_type(L, -1) == LUA_TSTRING) {  // LuaObject::IsString
    t.isId = true;
    t.id = lua_tostring(L, -1);
  } else {
    t.cat = ToCategory(L, -1);  // func_GetCObj_EntityCategory 0x5575e0
  }
  lua_pop(L, 1);
  return t;
}
int EntryInt(lua_State* L, int entry, int i) {
  lua_rawgeti(L, entry, i);
  int v = static_cast<int>(lua_tonumber(L, -1));  // LuaObject::GetInteger
  lua_pop(L, 1);
  return v;
}
std::string EntryStr(lua_State* L, int entry, int i) {
  lua_rawgeti(L, entry, i);
  std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return v;
}
// floor(count * mult) as the cfuncs round it: frndint, minus one when that rounded up (0x72d224 / 0x72dc19).
int FloorCount(float x) {
  float r = std::nearbyint(x);
  return static_cast<int>(r) + (x < r ? -1 : 0);
}
// CSquad::CountUnitsWithBP 0x724220 / CountUnitsInCategory 0x7242b0 / AppendUnitsWithBP 0x724350 /
// AppendUnitsInCategory 0x724400 on the pool's Unassigned squad: live, not destroy-queued, finished units; the
// append stops once it took `max` (it takes one before testing, so max 0 takes one).
bool ItemMatch(const Unit* u, const TemplateItem& t) {
  if (Gone(u) || u->beingBuilt) return false;
  if (t.isId) return u->blueprint && EqualNoCase(u->blueprint->id, t.id.c_str());
  return InCat(u, t.cat);
}
int CountUnassigned(Platoon* pool, const TemplateItem& t) {
  Squad* s = SquadOf(pool, kSquadUnassigned);
  if (!s) return 0;
  int n = 0;
  for (Unit* u : s->units)
    if (ItemMatch(u, t)) ++n;
  return n;
}
void GetUnassigned(Platoon* pool, const TemplateItem& t, int max, std::vector<Unit*>& out) {
  Squad* s = SquadOf(pool, kSquadUnassigned);
  if (!s) return;
  int n = 0;
  for (Unit* u : s->units) {
    if (!ItemMatch(u, t)) continue;
    SetAdd(out, u);
    if (++n >= max) break;
  }
}

// CanConsiderFormingPlatoon 0x72cc80 (template, name): whether the template's name starts with `name`
// (__strnicmp over name's length).
int l_CanConsiderFormingPlatoon(lua_State* L) {
  Args(L, "CPlatoon:CanConsiderFormingPlatoon()", 3, 3);
  P(L);
  const char* name = lua_tostring(L, 3);
  if (!name) luaL_typerror(L, 3, "string");
  std::string tname;
  if (lua_istable(L, 2)) tname = EntryStr(L, 2, 1);
  const size_t n = std::strlen(name);
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    const char a = i < tname.size() ? tname[i] : 0;
    if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(name[i]))) {
      ok = false;
      break;
    }
  }
  lua_pushboolean(L, ok);
  return 1;
}

// CanFormPlatoon 0x72cef0 (pool, template, multiplier [, position, radius]): every entry's floor(min * mult) units
// are in the pool (with a position: within the radius, 2D, strictly), and at least one unit counts at all.
int l_CanFormPlatoon(lua_State* L) {
  Args(L, "CPlatoon:CanFormPlatoon()", 3, 5);
  Platoon* pool = P(L);
  if (!lua_istable(L, 2)) {
    luaL_error(L, "Attempted to pass in a non table as a platoon template");
    return 0;
  }
  const int n = ObjectCount(L, 2);  // GetCount 0x72d151, read once; an entry that is no table is skipped
  int total = 0;
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    const int e = lua_gettop(L);
    if (!lua_istable(L, e)) {
      lua_pop(L, 1);
      continue;
    }
    const TemplateItem item = ItemOf(L, e);
    const float mult = static_cast<float>(lua_tonumber(L, 3));
    const int need = FloorCount(static_cast<float>(EntryInt(L, e, 2)) * mult);
    lua_pop(L, 1);
    if (lua_gettop(L) > 4) {
      std::vector<Unit*> units;
      GetUnassigned(pool, item, 1000, units);
      const Vec3 pos = VecArg(L, 4);
      const float r = NumArg(L, 5);
      int have = 0;
      for (Unit* u : units) {
        if (Gone(u) || u->beingBuilt) continue;
        const float dx = u->position.x - pos.x, dz = u->position.z - pos.z;
        // sqrt 0x40eaf0 (0x72d421): at PC_24 its x87 result is a float, compared with the radius (0x72d426..)
        const float d = std::sqrt(dx * dx + dz * dz);
        if (r > d) {
          ++have;
          ++total;
        }
      }
      if (have < need) {
        lua_pushboolean(L, 0);
        return 1;
      }
    } else {
      const int have = CountUnassigned(pool, item);
      total += have;
      if (have < need) {
        lua_pushboolean(L, 0);
        return 1;
      }
    }
  }
  lua_pushboolean(L, total != 0);
  return 1;
}

// FormPlatoon 0x72d8f0 (pool, template, multiplier [, position, radius]): a new platoon (MakePlatoon(template[1],
// template[2])) gets, per entry, up to floor(max * mult) of the pool's units (with a position: the nearest within
// the radius, by 2D distance squared, strictly inside) in the entry's squad; a squad is made with the entry's
// formation when the platoon has none of that class.
int l_FormPlatoon(lua_State* L) {
  Args(L, "CPlatoon:FormPlatoon()", 3, 5);
  Platoon* pool = P(L);
  Sim* sim = S(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  const int n = ObjectCount(L, 2);  // GetCount 0x72d9fa, read once; an entry that is no table is skipped
  const std::string name = EntryStr(L, 2, 1), plan = EntryStr(L, 2, 2);
  Platoon* np = sim->CreatePlatoon(L, pool->army, name, plan);
  if (!np) {
    lua_pushnil(L);
    return 1;
  }
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    const int e = lua_gettop(L);
    if (!lua_istable(L, e)) {
      lua_pop(L, 1);
      continue;
    }
    const TemplateItem item = ItemOf(L, e);
    const int maxCount = EntryInt(L, e, 3);
    const float mult = static_cast<float>(lua_tonumber(L, 3));
    const int take = FloorCount(mult * static_cast<float>(maxCount));
    lua_rawgeti(L, e, 4);
    const int cls = SquadArg(L, lua_gettop(L));
    lua_pop(L, 1);
    const std::string formation = EntryStr(L, e, 5);
    lua_pop(L, 1);
    std::vector<Unit*> units;
    if (lua_gettop(L) > 4) {
      std::vector<Unit*> cand;
      GetUnassigned(pool, item, 1000, cand);
      const Vec3 pos = VecArg(L, 4);
      const float r = NumArg(L, 5);
      const float r2 = NumArg(L, 5) * r;
      std::vector<std::pair<Unit*, float>> near;
      for (Unit* u : cand) {
        if (Gone(u) || u->beingBuilt) continue;
        const float dz = u->position.z - pos.z, dx = u->position.x - pos.x;
        const float d = dx * dx + dz * dz;
        if (r2 > d) near.emplace_back(u, d);
      }
      // std::sort with the distance (0x733b40; insertion sort up to 32: stable)
      std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      int k = 0;
      for (auto& [u, d] : near) {  // (the count is tested after each add: a count of 0 takes them all)
        SetAdd(units, u);
        if (++k == take) break;
      }
    } else {
      GetUnassigned(pool, item, take, units);
    }
    RemoveUnitsFromPlatoons(np->army, units);
    Squad* s = SquadOf(np, cls);
    if (!s) s = NewSquad(np, cls, formation);
    SetAddRange(s->units, units);
    Invalidate(np);
  }
  PushObject(L, np);
  return 1;
}

int StateQuery(lua_State* L, const char* doc, const char* state) {
  Args(L, doc, 2, 2);
  Platoon* p = P(L);
  lua_pushboolean(L, SquadHasState(p, SquadArg(L, 2), state));
  return 1;
}
// IsAttacking 0x72e2c0 (EUnitState 3), IsMoving 0x72e460 (2), IsPatrolling 0x72e600 (12), IsFerrying 0x72e7a0 (20)
int l_IsAttacking(lua_State* L) { return StateQuery(L, "CPlatoon:IsAttacking()", "Attacking"); }
int l_IsMoving(lua_State* L) { return StateQuery(L, "CPlatoon:IsMoving()", "Moving"); }
int l_IsPatrolling(lua_State* L) { return StateQuery(L, "CPlatoon:IsPatrolling()", "Patrolling"); }
int l_IsFerrying(lua_State* L) { return StateQuery(L, "CPlatoon:IsFerrying()", "Ferrying"); }

// SetPrioritizedTargetList 0x72e940 (squad, { categories }): the squad's priority list (+0x54), if it has the squad.
// i = 1 .. GetCount (0x72ea69, read again each pass; nothing here changes the list) through func_GetCObj_EntityCategory
// 0x5575e0: a value that is no category (a nil hole included) is a Lua error (LuaObject::GetUserData 0x907bc0's
// luaG_typeerror, or the upcast's).
int l_SetPrioritizedTargetList(lua_State* L) {
  Args(L, "CPlatoon:SetPrioritizedTargetList()", 3, 3);
  Platoon* p = P(L);
  const int cls = SquadArg(L, 2);
  std::vector<std::vector<uint64_t>> list;
  if (lua_istable(L, 3)) {
    const int n = ObjectCount(L, 3);
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 3, i);
      const uint64_t* c = ToCategory(L, -1);
      if (!c) luaL_error(L, "attempt to get as UserData a %s value", lua_typename(L, lua_type(L, -1)));
      list.emplace_back(c, c + CategoryWordCount(L));
      lua_pop(L, 1);
    }
    if (Squad* s = SquadOf(p, cls)) s->priorities = std::move(list);
  }
  return 0;
}
// FindPrioritizedUnit 0x72ebb0 (squad, alliance, canAttack, position, radius)
int l_FindPrioritizedUnit(lua_State* L) {
  Args(L, "CPlatoon:FindPrioritizedUnit()", 6, 6);
  Platoon* p = P(L);
  const int cls = SquadArg(L, 2);
  const int rel = AllianceArg(L, 3);
  const bool canAttack = lua_toboolean(L, 4) != 0;
  const Vec3 pos = VecArg(L, 5);
  const float r = NumArg(L, 6);
  PushObject(L, FindPrioritized(*S(L), p, cls, rel, canAttack, pos, r));
  return 1;
}
// FindClosestUnit 0x72ee60 / FindFurthestUnit 0x72f310 / FindHighestValueUnit 0x72f550 (squad, alliance, canAttack,
// category): FindClosestUnit 0x726120 with mode 0 / 1 / 2.
int FindQuery(lua_State* L, const char* doc, int mode) {
  Args(L, doc, 5, 5);
  Platoon* p = P(L);
  const int cls = SquadArg(L, 2);
  const int rel = AllianceArg(L, 3);
  const bool canAttack = lua_toboolean(L, 4) != 0;
  const uint64_t* cat = ToCategory(L, 5);
  PushObject(L, FindFromSquad(*S(L), p, cls, cat, mode, rel, canAttack));
  return 1;
}
int l_FindClosestUnit(lua_State* L) { return FindQuery(L, "CPlatoon:FindClosestUnit()", 0); }
int l_FindFurthestUnit(lua_State* L) { return FindQuery(L, "CPlatoon:FindFurthestUnit()", 1); }
int l_FindHighestValueUnit(lua_State* L) { return FindQuery(L, "CPlatoon:FindHighestValueUnit()", 2); }
// FindClosestUnitToBase 0x72f0a0: the nearest to the brain's CenterOfArmy 0x57a510.
int l_FindClosestUnitToBase(lua_State* L) {
  Args(L, "CPlatoon:FindClosestUnitToBase()", 5, 5);
  Platoon* p = P(L);
  const int cls = SquadArg(L, 2);
  const int rel = AllianceArg(L, 3);
  const bool canAttack = lua_toboolean(L, 4) != 0;
  const uint64_t* cat = ToCategory(L, 5);
  Sim& sim = *S(L);
  PushObject(L, FindClosestToPos(sim, p, cls, cat, 0, rel, canAttack, CenterOfArmy(sim, p->army)));
  return 1;
}
// CanAttackTarget 0x72f790 (squad, unit)
int l_CanAttackTarget(lua_State* L) {
  Args(L, "CPlatoon:CanAttackTarget()", 3, 3);
  Platoon* p = P(L);
  const int cls = SquadArg(L, 2);
  Unit* t = CheckObject<Unit>(L, 3);
  Squad* s = SquadOf(p, cls);
  lua_pushboolean(L, s && SquadCanAttack(*S(L), s, t));
  return 1;
}
// Stop 0x72f9c0 ([squad])
int l_Stop(lua_State* L) {
  Args(L, "CPlatoon:Stop()", 1, 2);
  Platoon* p = P(L);
  const int cls = lua_gettop(L) > 1 ? SquadArg(L, 2) : kSquadAll;
  Stop(*S(L), p, cls);
  return 0;
}
// AttackTarget 0x72fb60 (target [, squad]) -> the commands
int l_AttackTarget(lua_State* L) {
  Args(L, "CPlatoon:AttackTarget()", 2, 3);
  Platoon* p = P(L);
  Entity* t = CheckObject<Entity>(L, 2);
  const int cls = lua_gettop(L) > 2 ? SquadArg(L, 3) : kSquadAll;
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  AttackTarget(*S(L), p, t, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// MoveToTarget 0x72fe30 (target, useTransports [, squad])
int l_MoveToTarget(lua_State* L) {
  Args(L, "CPlatoon:MoveToTarget()", 2, 4);
  Platoon* p = P(L);
  Entity* t = CheckObject<Entity>(L, 2);
  const bool transports = lua_toboolean(L, 3) != 0;
  const int cls = lua_gettop(L) > 3 ? SquadArg(L, 4) : kSquadAll;
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  MoveTo(*S(L), p, OnEntity(t), transports, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// MoveToLocation 0x730120 (position, useTransports [, squad])
int l_MoveToLocation(lua_State* L) {
  Args(L, "CPlatoon:MoveToLocation()", 2, 4);
  Platoon* p = P(L);
  Vec3 pos = VecArg(L, 2);
  const bool transports = lua_toboolean(L, 3) != 0;
  if (!ValidVec(pos)) luaL_error(L, "Platoon:MoveToTarget Passed in an invalid target point");
  const int cls = lua_gettop(L) > 3 ? SquadArg(L, 4) : kSquadAll;
  Sim& sim = *S(L);
  ToSurface(sim, pos);
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  MoveTo(sim, p, Ground(pos), transports, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// AggressiveMoveToLocation 0x730420 (position [, squad])
int l_AggressiveMoveToLocation(lua_State* L) {
  Args(L, "CPlatoon:AggressiveMoveToLocation()", 2, 3);
  Platoon* p = P(L);
  const Vec3 pos = VecArg(L, 2);
  if (!ValidVec(pos)) luaL_error(L, "Platoon:AggressiveMoveToLocation Passed in an invalid target point");
  const int cls = lua_gettop(L) > 2 ? SquadArg(L, 3) : kSquadAll;
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  AggressiveMove(*S(L), p, pos, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// FerryToLocation 0x730700 (position)
int l_FerryToLocation(lua_State* L) {
  Args(L, "CPlatoon:FerryToLocation()", 2, 2);
  Platoon* p = P(L);
  const Vec3 pos = VecArg(L, 2);
  if (!ValidVec(pos)) luaL_error(L, "Platoon:FerryToLocation Passed in an invalid target point");
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  FerryToLocation(*S(L), p, pos, cmds);
  PushCommands(L, cmds);
  return 1;
}
// UnloadAllAtLocation 0x730e30 (position)
int l_UnloadAllAtLocation(lua_State* L) {
  Args(L, "CPlatoon:UnloadAllAtLocation()", 2, 2);
  Platoon* p = P(L);
  const Vec3 pos = VecArg(L, 2);
  if (!ValidVec(pos)) luaL_error(L, "Platoon:UnloadAllAtLocation Passed in an invalid target point");
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  UnloadAllAtLocation(*S(L), p, pos, cmds);
  PushCommands(L, cmds);
  return 1;
}
// GuardTarget 0x7310a0 (unit [, squad])
int l_GuardTarget(lua_State* L) {
  Args(L, "CPlatoon:GuardTarget()", 2, 3);
  Platoon* p = P(L);
  Unit* t = CheckObject<Unit>(L, 2);
  const int cls = lua_gettop(L) > 2 ? SquadArg(L, 3) : kSquadAll;
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  GuardTarget(*S(L), p, t, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// Patrol 0x731370 (position [, squad]); returns nothing
int l_Patrol(lua_State* L) {
  Args(L, "CPlatoon:Patrol()", 2, 3);
  Platoon* p = P(L);
  const Vec3 pos = VecArg(L, 2);
  if (!ValidVec(pos)) luaL_error(L, "Platoon:Patrol Passed in an invalid target point");
  const int cls = lua_gettop(L) > 2 ? SquadArg(L, 3) : kSquadAll;
  Patrol(*S(L), p, pos, cls);
  return 0;
}
// Destroy 0x731570 ([squad]): nothing for a destroyed platoon
int l_Destroy(lua_State* L) {
  Args(L, "CPlatoon:Destroy()", 1, 2);
  Platoon* p = POpt(L, 1);
  if (!p) return 0;
  const int cls = lua_gettop(L) > 1 ? SquadArg(L, 2) : kSquadAll;
  DestroyUnits(*S(L), p, cls);
  return 0;
}
int l_DisbandOnIdle(lua_State* L) {  // 0x731710
  Args(L, "CPlatoon:DisbandOnIdle()", 1, 1);
  P(L)->disbandOnIdle = true;
  return 0;
}
// GetFerryBeacons 0x731840 -> CPlatoon::GetFerryBeacons 0x725870 (ferry_commands.md 2.5): the beacons of the
// commands the platoon's Ferrying units execute, as an EntitySet.
int l_GetFerryBeacons(lua_State* L) {
  Args(L, "CPlatoon:GetFerryBeacons()", 1, 1);
  Platoon* p = P(L);
  Sim& sim = *S(L);
  std::vector<Entity*> beacons;
  for (Unit* u : AllUnits(p)) {
    if (!u->unitStates.Has(US("Ferrying")) || u->commands.empty()) continue;
    const UnitCommand& c = *u->commands.front();
    if (!c.beaconRef) continue;
    if (Entity* b = sim.FindEntity(c.beaconRef))
      if (std::find(beacons.begin(), beacons.end(), b) == beacons.end()) beacons.push_back(b);
  }
  std::sort(beacons.begin(), beacons.end(), [](const Entity* a, const Entity* b) { return a->id < b->id; });
  lua_newtablesized(L, static_cast<int>(beacons.size()), 0);
  int n = 0;
  for (Entity* b : beacons) {
    PushObject(L, b);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
// UseFerryBeacon 0x731a30 (category, beacon unit)
int l_UseFerryBeacon(lua_State* L) {
  Args(L, "CPlatoon:UseFerryBeacon()", 3, 3);
  Platoon* p = P(L);
  const uint64_t* cat = ToCategory(L, 2);
  Unit* beacon = CheckObject<Unit>(L, 3);
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  UseFerryBeacon(*S(L), p, cat, beacon, cmds);
  PushCommands(L, cmds);
  return 1;
}
// UseTeleporter 0x731cb0 (teleporter unit [, squad])
int l_UseTeleporter(lua_State* L) {
  Args(L, "CPlatoon:UseTeleporter()", 2, 3);
  Platoon* p = P(L);
  Unit* t = CheckObject<Unit>(L, 2);
  const int cls = lua_gettop(L) > 2 ? SquadArg(L, 3) : kSquadAll;
  std::vector<std::shared_ptr<UnitCommand>> cmds;
  UseTeleporter(*S(L), p, t, cls, cmds);
  PushCommands(L, cmds);
  return 1;
}
// IsCommandsActive 0x731f80 ({commands}): some command of the list still exists (GetCUnitCommandOpt 0x6f8d80):
// in ours, a command no unit holds any more is done (IsCommandDone). i = 1 .. GetCount (0x73202a, read again each
// pass; nothing here changes the list), the first live command answers true; a value that is no command (a nil hole
// included) is the Opt getter's Lua error.
int l_IsCommandsActive(lua_State* L) {
  Args(L, "CPlatoon:IsCommandsActive()", 2, 2);
  P(L);
  Sim& sim = *S(L);
  bool active = false;
  if (lua_istable(L, 2)) {
    const int n = ObjectCount(L, 2);
    for (int i = 1; i <= n && !active; ++i) {
      lua_rawgeti(L, 2, i);
      bool isCommand = false;
      if (lua_istable(L, -1)) {
        lua_pushstring(L, "moho64_cmd");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
          isCommand = true;
          const uint32_t id = static_cast<uint32_t>(lua_tonumber(L, -1));
          auto it = sim.commandsById.find(id);
          if (it != sim.commandsById.end())
            if (auto c = it->second.lock()) active = !c->units.empty();
        }
        lua_pop(L, 1);
      }
      if (!isCommand) {
        if (GetObject(L, -1)) luaL_error(L, "Incorrect type of game object.  (Did you call with '.' instead of ':'?)");
        luaL_error(L, "Expected a game object. (Did you call with '.' instead of ':'?)");
      }
      lua_pop(L, 1);
    }
  }
  lua_pushboolean(L, active);
  return 1;
}
// SetPlatoonFormationOverride 0x732140 (name): '' or 'None' (case-insensitive) clear it
int l_SetPlatoonFormationOverride(lua_State* L) {
  Args(L, "CPlatoon:SetPlatoonFormationOverride()", 2, 2);
  Platoon* p = P(L);
  const char* s = lua_tostring(L, 2);
  if (!s) luaL_typerror(L, 2, "string");
  std::string v = s;
  p->formationOverride = (v.empty() || EqualNoCase(v, "None")) ? "" : v;
  return 0;
}
// GetPlatoonLifetimeStats 0x732350: kills, losses, damage dealt, damage taken; nil for a destroyed platoon
int l_GetPlatoonLifetimeStats(lua_State* L) {
  Args(L, "CPlatoon:GetPlatoonLifetimeStats()", 1, 1);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, static_cast<float>(p->kills));
  lua_pushnumber(L, static_cast<float>(p->losses));
  lua_pushnumber(L, p->damageDealt);
  lua_pushnumber(L, p->damageTaken);
  return 4;
}
// CalculatePlatoonThreat 0x732520 (type, category): the units' (not dead, not destroy-queued, in the category)
// blueprint threat levels of that type, summed in set order; 0 for a destroyed platoon.
int l_CalculatePlatoonThreat(lua_State* L) {
  Args(L, "CPlatoon:CalculatePlatoonThreat()", 3, 3);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_pushnumber(L, 0);
    return 1;
  }
  const char* ts = lua_tostring(L, 2);
  if (!ts) luaL_typerror(L, 2, "string");
  const int type = ThreatType(ts);
  const uint64_t* cat = ToCategory(L, 3);
  Sim& sim = *S(L);
  float sum = 0;
  for (Unit* u : AllUnits(p)) {
    if (Gone(u) || !u->blueprint || !InCat(u, cat)) continue;
    sum = AddThreat(sum, BlueprintThreat(sim, u->blueprint), type);
  }
  lua_pushnumber(L, sum);
  return 1;
}
// CalculatePlatoonThreatAroundPosition 0x732960 (type, category, position, radius): the same within the radius
// (2D: dz^2 + dx^2 <= radius * radius)
int l_CalculatePlatoonThreatAroundPosition(lua_State* L) {
  Args(L, "CPlatoon:CalculatePlatoonThreatAroundPosition()", 5, 5);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_pushnumber(L, 0);
    return 1;
  }
  const char* ts = lua_tostring(L, 2);
  if (!ts) luaL_typerror(L, 2, "string");
  const int type = ThreatType(ts);
  const uint64_t* cat = ToCategory(L, 3);
  const Vec3 pos = VecArg(L, 4);
  const float r = NumArg(L, 5);
  const float r2 = NumArg(L, 5) * r;
  Sim& sim = *S(L);
  float sum = 0;
  for (Unit* u : AllUnits(p)) {
    if (Gone(u)) continue;
    const float dz = pos.z - u->position.z, dx = pos.x - u->position.x;
    if (dz * dz + dx * dx > r2) continue;
    if (!u->blueprint || !InCat(u, cat)) continue;
    sum = AddThreat(sum, BlueprintThreat(sim, u->blueprint), type);
  }
  lua_pushnumber(L, sum);
  return 1;
}
// PlatoonCategoryCountAroundPosition 0x732ec0 (category, position, radius) / PlatoonCategoryCount 0x733240
// (category): the units not dead, not destroy-queued, in the category (and within the radius, 2D); 0 for a
// destroyed platoon.
int l_PlatoonCategoryCountAroundPosition(lua_State* L) {
  Args(L, "CPlatoon:PlatoonCategoryCountAroundPosition()", 4, 4);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_pushnumber(L, 0);
    return 1;
  }
  const uint64_t* cat = ToCategory(L, 2);
  const Vec3 pos = VecArg(L, 3);
  const float r = NumArg(L, 4);
  const float r2 = NumArg(L, 4) * r;
  int n = 0;
  for (Unit* u : AllUnits(p)) {
    if (Gone(u)) continue;
    const float dx = pos.x - u->position.x, dz = pos.z - u->position.z;
    if (dx * dx + dz * dz > r2) continue;
    if (InCat(u, cat)) ++n;
  }
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}
int l_PlatoonCategoryCount(lua_State* L) {
  Args(L, "CPlatoon:PlatoonCategoryCount()", 2, 2);
  Platoon* p = POpt(L, 1);
  if (!p) {
    lua_pushnumber(L, 0);
    return 1;
  }
  const uint64_t* cat = ToCategory(L, 2);
  int n = 0;
  for (Unit* u : AllUnits(p))
    if (!Gone(u) && InCat(u, cat)) ++n;
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}

// ---- Lua: CAiBrain's platoon functions ----

AiBrain* B(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

// MakePlatoon 0x58d6d0: (name, plan), both strings, or (template): the template form fills the new platoon from
// the pool like FormPlatoon without the multiplier (entry[3] units of entry[1], squad entry[4], formation entry[5]).
int l_MakePlatoon(lua_State* L) {
  Args(L, "CAiBrain:MakePlatoon()", 2, 3);
  AiBrain* b = B(L);
  Sim* sim = S(L);
  Army* a = b->army;
  if (lua_istable(L, 2)) {
    Platoon* pool = PlatoonByName(a, "ArmyPool");
    if (!pool) {
      lua_pushnil(L);
      return 1;
    }
    const int n = ObjectCount(L, 2);  // GetCount 0x58d7d3, read once; an entry that is no table is skipped
    const std::string name = EntryStr(L, 2, 1), plan = EntryStr(L, 2, 2);
    Platoon* np = sim->CreatePlatoon(L, a, name, plan);
    if (!np) {
      lua_pushnil(L);
      return 1;
    }
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 2, i);
      const int e = lua_gettop(L);
      if (!lua_istable(L, e)) {
        lua_pop(L, 1);
        continue;
      }
      lua_rawgeti(L, e, 1);
      const std::string id = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
      lua_pop(L, 1);
      const int count = EntryInt(L, e, 3);
      lua_rawgeti(L, e, 4);
      const int cls = SquadArg(L, lua_gettop(L));
      lua_pop(L, 1);
      const std::string formation = EntryStr(L, e, 5);
      lua_pop(L, 1);
      TemplateItem item;
      item.isId = true;
      item.id = id;
      std::vector<Unit*> units;
      GetUnassigned(pool, item, count, units);  // CPlatoon::GetUnassignedUnitsWithBP 0x725730
      RemoveUnitsFromPlatoons(a, units);
      Squad* s = SquadOf(np, cls);
      if (!s) s = NewSquad(np, cls, formation);
      SetAddRange(s->units, units);
      Invalidate(np);
    }
    PushObject(L, np);
    return 1;
  }
  if (lua_isstring(L, 2) && lua_gettop(L) > 2 && lua_isstring(L, 3)) {
    PushObject(L, sim->CreatePlatoon(L, a, lua_tostring(L, 2), lua_tostring(L, 3)));
    return 1;
  }
  lua_pushnil(L);
  return 1;
}
// GetPlatoonUniquelyNamed 0x58e020
int l_GetPlatoonUniquelyNamed(lua_State* L) {
  Args(L, "CAiBrain:GetPlatoonUniquelyNamed()", 2, 2);
  AiBrain* b = B(L);
  Platoon* p = lua_isstring(L, 2) ? PlatoonByName(b->army, lua_tostring(L, 2)) : nullptr;
  PushObject(L, p);
  return 1;
}
// AssignUnitsToPlatoon 0x58dce0 (platoon or unique name, units, squad, formation): the units leave their platoons
// and join that squad (made with the formation if missing); then the platoon's OnUnitsAddedToPlatoon.
int l_AssignUnitsToPlatoon(lua_State* L) {
  Args(L, "CAiBrain:AssignUnitsToPlatoon()", 5, 5);
  AiBrain* b = B(L);
  const int cls = SquadArg(L, 4);
  if (!lua_istable(L, 3)) return 0;
  Platoon* p = lua_isstring(L, 2) ? PlatoonByName(b->army, lua_tostring(L, 2)) : CheckObject<Platoon>(L, 2);
  if (!p) return 0;
  // sub_5919F0: i = 1 .. GetCount (0x591a1e, read again each pass; nothing here changes the list) through
  // SCR_FromLua_Unit 0x5936c0 (a nil hole, a destroyed unit or another value is a Lua error), appended in the list's
  // order (0x591a54: a plain push_back, no sort); the squad's AddRange sorts them.
  std::vector<Unit*> units;
  const int n = ObjectCount(L, 3);
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 3, i);
    units.push_back(CheckObject<Unit>(L, -1));
    lua_pop(L, 1);
  }
  RemoveUnitsFromPlatoons(b->army, units);
  Squad* s = SquadOf(p, cls);
  if (!s) s = NewSquad(p, cls, lua_isstring(L, 5) ? lua_tostring(L, 5) : "");
  SetAddRange(s->units, units);
  Invalidate(p);
  S(L)->CallMethod(L, p, "OnUnitsAddedToPlatoon", 0);
  return 0;
}
// DisbandPlatoon 0x58d3e0 (platoon) -> CArmyImpl::DisbandPlatoon 0x700540
int l_DisbandPlatoon(lua_State* L) {
  Args(L, "CAiBrain:DisbandPlatoon()", 2, 2);
  AiBrain* b = B(L);
  Platoon* p = CheckObject<Platoon>(L, 2);
  if (b->army) DisbandPlatoon(*S(L), b->army, p);
  return 0;
}
// DisbandPlatoonUniquelyNamed 0x58d550 (name) -> 0x7005f0
int l_DisbandPlatoonUniquelyNamed(lua_State* L) {
  Args(L, "CAiBrain:DisbandPlatoonUniquelyNamed()", 2, 2);
  AiBrain* b = B(L);
  if (lua_isstring(L, 2) && b->army) DisbandPlatoonUniquelyNamed(*S(L), b->army, lua_tostring(L, 2));
  return 0;
}
// GetNumPlatoonsTemplateNamed 0x58cea0 (name) -> 0x700770: platoons whose name (+0x70) matches, case-insensitive
int l_GetNumPlatoonsTemplateNamed(lua_State* L) {
  Args(L, "CAiBrain:GetNumPlatoonsTemplateNamed()", 2, 2);
  AiBrain* b = B(L);
  int n = 0;
  if (lua_isstring(L, 2) && b->army)
    for (Platoon* p : b->army->platoons)
      if (EqualNoCase(p->name, lua_tostring(L, 2))) ++n;
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}
// GetNumPlatoonsWithAI 0x58cd10 (plan) -> 0x7007c0: platoons whose plan (+0x8c) matches, case-insensitive
int l_GetNumPlatoonsWithAI(lua_State* L) {
  Args(L, "CAiBrain:GetNumPlatoonsWithAI()", 2, 2);
  AiBrain* b = B(L);
  int n = 0;
  if (lua_isstring(L, 2) && b->army)
    for (Platoon* p : b->army->platoons)
      if (EqualNoCase(p->plan, lua_tostring(L, 2))) ++n;
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}
// GetPlatoonsList 0x58d1c0: the army's platoons with units, but not the one named 'ArmyPool'
int l_GetPlatoonsList(lua_State* L) {
  Args(L, "CAiBrain:GetPlatoonsList()", 1, 1);
  AiBrain* b = B(L);
  lua_newtable(L);
  if (!b->army) return 1;
  std::vector<Platoon*> list = b->army->platoons;
  int n = 0;
  for (Platoon* p : list) {
    if (EqualNoCase(p->uniqueName, "ArmyPool") || UnitCount(p) == 0) continue;
    PushObject(L, p);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}
// PlatoonExists 0x58d040 (platoon): its engine object still exists
int l_PlatoonExists(lua_State* L) {
  Args(L, "CAiBrain:PlatoonExists()", 2, 2);
  B(L);
  lua_pushboolean(L, POpt(L, 2) != nullptr);
  return 1;
}

}  // namespace

void RegisterPlatoonBindings(lua_State* L) {
  SetMethod(L, "CPlatoon", "IsOpponentAIRunning", l_IsOpponentAIRunning);
  SetMethod(L, "CPlatoon", "GetPersonality", l_GetPersonality);
  SetMethod(L, "CPlatoon", "GetBrain", l_GetBrain);
  SetMethod(L, "CPlatoon", "GetFactionIndex", l_GetFactionIndex);
  SetMethod(L, "CPlatoon", "UniquelyNamePlatoon", l_UniquelyNamePlatoon);
  SetMethod(L, "CPlatoon", "GetPlatoonUniqueName", l_GetPlatoonUniqueName);
  SetMethod(L, "CPlatoon", "GetAIPlan", l_GetAIPlan);
  SetMethod(L, "CPlatoon", "SwitchAIPlan", l_SwitchAIPlan);
  SetMethod(L, "CPlatoon", "GetPlatoonPosition", l_GetPlatoonPosition);
  SetMethod(L, "CPlatoon", "GetSquadPosition", l_GetSquadPosition);
  SetMethod(L, "CPlatoon", "GetSquadUnits", l_GetSquadUnits);
  SetMethod(L, "CPlatoon", "GetPlatoonUnits", l_GetPlatoonUnits);
  SetMethod(L, "CPlatoon", "CanConsiderFormingPlatoon", l_CanConsiderFormingPlatoon);
  SetMethod(L, "CPlatoon", "CanFormPlatoon", l_CanFormPlatoon);
  SetMethod(L, "CPlatoon", "FormPlatoon", l_FormPlatoon);
  SetMethod(L, "CPlatoon", "IsAttacking", l_IsAttacking);
  SetMethod(L, "CPlatoon", "IsMoving", l_IsMoving);
  SetMethod(L, "CPlatoon", "IsPatrolling", l_IsPatrolling);
  SetMethod(L, "CPlatoon", "IsFerrying", l_IsFerrying);
  SetMethod(L, "CPlatoon", "SetPrioritizedTargetList", l_SetPrioritizedTargetList);
  SetMethod(L, "CPlatoon", "FindPrioritizedUnit", l_FindPrioritizedUnit);
  SetMethod(L, "CPlatoon", "FindClosestUnit", l_FindClosestUnit);
  SetMethod(L, "CPlatoon", "FindClosestUnitToBase", l_FindClosestUnitToBase);
  SetMethod(L, "CPlatoon", "FindFurthestUnit", l_FindFurthestUnit);
  SetMethod(L, "CPlatoon", "FindHighestValueUnit", l_FindHighestValueUnit);
  SetMethod(L, "CPlatoon", "CanAttackTarget", l_CanAttackTarget);
  SetMethod(L, "CPlatoon", "Stop", l_Stop);
  SetMethod(L, "CPlatoon", "AttackTarget", l_AttackTarget);
  SetMethod(L, "CPlatoon", "MoveToTarget", l_MoveToTarget);
  SetMethod(L, "CPlatoon", "MoveToLocation", l_MoveToLocation);
  SetMethod(L, "CPlatoon", "AggressiveMoveToLocation", l_AggressiveMoveToLocation);
  SetMethod(L, "CPlatoon", "FerryToLocation", l_FerryToLocation);
  SetMethod(L, "CPlatoon", "UnloadAllAtLocation", l_UnloadAllAtLocation);
  SetMethod(L, "CPlatoon", "GuardTarget", l_GuardTarget);
  SetMethod(L, "CPlatoon", "Patrol", l_Patrol);
  SetMethod(L, "CPlatoon", "Destroy", l_Destroy);
  SetMethod(L, "CPlatoon", "DisbandOnIdle", l_DisbandOnIdle);
  SetMethod(L, "CPlatoon", "GetFerryBeacons", l_GetFerryBeacons);
  SetMethod(L, "CPlatoon", "UseFerryBeacon", l_UseFerryBeacon);
  SetMethod(L, "CPlatoon", "UseTeleporter", l_UseTeleporter);
  SetMethod(L, "CPlatoon", "IsCommandsActive", l_IsCommandsActive);
  SetMethod(L, "CPlatoon", "SetPlatoonFormationOverride", l_SetPlatoonFormationOverride);
  SetMethod(L, "CPlatoon", "GetPlatoonLifetimeStats", l_GetPlatoonLifetimeStats);
  SetMethod(L, "CPlatoon", "CalculatePlatoonThreat", l_CalculatePlatoonThreat);
  SetMethod(L, "CPlatoon", "CalculatePlatoonThreatAroundPosition", l_CalculatePlatoonThreatAroundPosition);
  SetMethod(L, "CPlatoon", "PlatoonCategoryCountAroundPosition", l_PlatoonCategoryCountAroundPosition);
  SetMethod(L, "CPlatoon", "PlatoonCategoryCount", l_PlatoonCategoryCount);

  SetMethod(L, "CAiBrain", "MakePlatoon", l_MakePlatoon);
  SetMethod(L, "CAiBrain", "GetPlatoonUniquelyNamed", l_GetPlatoonUniquelyNamed);
  SetMethod(L, "CAiBrain", "AssignUnitsToPlatoon", l_AssignUnitsToPlatoon);
  SetMethod(L, "CAiBrain", "DisbandPlatoon", l_DisbandPlatoon);
  SetMethod(L, "CAiBrain", "DisbandPlatoonUniquelyNamed", l_DisbandPlatoonUniquelyNamed);
  SetMethod(L, "CAiBrain", "GetNumPlatoonsTemplateNamed", l_GetNumPlatoonsTemplateNamed);
  SetMethod(L, "CAiBrain", "GetNumPlatoonsWithAI", l_GetNumPlatoonsWithAI);
  SetMethod(L, "CAiBrain", "GetPlatoonsList", l_GetPlatoonsList);
  SetMethod(L, "CAiBrain", "PlatoonExists", l_PlatoonExists);
}

}  // namespace moho
