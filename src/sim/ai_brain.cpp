// The classic AI brain's small state and the CAiBrain methods around it: see ai_brain.h.
//
// Read in the FA exe (build 3839), 2026-10-15, with the addresses:
//   the Lua functions cfunc_CAiBrain*L: IsOpponentAIRunning 0x585f70, SetCurrentPlan 0x586390, SetCurrentEnemy
//   0x586670, GetCurrentEnemy 0x5867f0, SetGreaterOf 0x587320, FindClosestArmyWithBase 0x58e8b0,
//   SetUpAttackVectorsToArmy 0x58ebc0, GetAttackVectors 0x58ede0, PickBestAttackVector 0x58efe0, GetNoRushTicks
//   0x591870;
//   CAiBrain::CenterOfArmy 0x57a510, ProcessAttackVectors 0x57bdb0, PickBestAttackVector 0x57c290 (and its unit
//   search sub_57B620 0x57b620, func_GetUnitsAroundPoint 0x57b290), CSquad::GetCenter 0x724020, CSquad::FitsAt
//   0x724550 (OCCUPY_FootprintFits 0x7209e0, OCCUPY_HoverFootprintFits 0x720b20), CArmyImpl::GetUnits 0x700eb0,
//   CountUnitsInBoundsXZ 0x700a00, IArmy::GetAllianceWith 0x579430, the army ctor's NoRushOption (0x6ff385 and FAF's
//   patch at 0x12917de), CArmyImpl::OnTick 0x6ffd91 (the no-rush countdown) and 0x6ffed8..0x6fff4d (the debug call
//   of ProcessAttackVectors), the sim vars AI_RunOpponentAI 0x10ad724 (registered 0xbcb080, default 1),
//   AI_DebugArmyIndex 0x10ad5d0 (0xbcb100, -1) and AI_RenderDebugAttackVectors 0x10ad54c (0xbcb180, 0); the enums
//   EAlliance 0x509e20, ECompareType 0x5798d0, ESquadClass 0x723bd0.
// Inferred (not read) is marked as such.
#include "sim/ai_brain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/log.h"
#include "sim/ai_platoon.h"
#include "sim/blueprints.h"
#include "sim/combat.h"
#include "sim/intel.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

// MOHO64_DEBUG_AIBRAIN=1: a log line for every change of enemy, every set of attack vectors and every pick.
bool Dbg() {
  static const bool on = getenv("MOHO64_DEBUG_AIBRAIN") != nullptr;
  return on;
}

// SPointVector (0x18 bytes): a point and a direction; SCR_ToLua<SPointVector> 0x4d0350 names them px py pz vx vy vz.
struct AttackVector {
  float px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
};

struct BrainState {
  Army* enemy = nullptr;               // brain+0x38 (IArmy*)
  std::string plan;                    // brain+0x40
  std::vector<AttackVector> vectors;   // brain+0x5c
  std::vector<uint64_t> vectorCat;     // brain+0x70 (an EntityCategory copy; empty: none set yet)
  int noRushStart = 0;                 // army+0x1cc as the army ctor left it
  // the no-rush circle's (army+0x1d0 the radius, +0x1d4 / +0x1d8 the offsets in x / z), for the view only
  float noRushRadius = 0, noRushOffX = 0, noRushOffZ = 0;
};
std::vector<BrainState> g_state;  // by 0-based army index

BrainState& StateOf(const AiBrain* b) {
  static BrainState none;
  const int i = b && b->army ? b->army->index - 1 : -1;
  if (i < 0 || static_cast<size_t>(i) >= g_state.size()) {
    none = BrainState();
    return none;
  }
  return g_state[static_cast<size_t>(i)];
}

// "%s\n  expected %d args, but got %d" (0xe0a220) / "... between %d and %d args ..." (0xe0a270), with the
// binding's help text, as every cfunc checks first.
void Args(lua_State* L, const char* doc, int lo, int hi) {
  const int n = lua_gettop(L);
  if (lo == hi && n != lo) luaL_error(L, "%s\n  expected %d args, but got %d", doc, lo, n);
  if (lo != hi && (n < lo || n > hi))
    luaL_error(L, "%s\n  expected between %d and %d args, but got %d", doc, lo, hi, n);
}

AiBrain* B(lua_State* L) { return CheckObject<AiBrain>(L, 1); }  // SCR_FromLua_CAiBrain 0x5930d0

// IArmy::GetAllianceWith 0x579430 as EAlliance (0x509e20: Neutral 0, Ally 1, Enemy 2): no army -> Neutral, the army
// itself -> Ally, else the first of its Neutral / Ally / Enemy sets that holds the other's index (none: Neutral).
// Our armies keep one relation per army (0 enemy, 1 neutral, 2 ally).
enum : int { kNeutral = 0, kAlly = 1, kEnemy = 2 };
int AllianceWith(const Army* self, const Army* other) {
  if (!self || !other) return kNeutral;
  if (self == other) return kAlly;
  const size_t i = static_cast<size_t>(other->index - 1);
  const int r = i < self->alliance.size() ? self->alliance[i] : 1;
  return r == 2 ? kAlly : r == 0 ? kEnemy : kNeutral;
}

// SCR_GetEnum 0x4d2550 -> REnumType::SetLexical 0x8d9670 for the three enums these functions take: '|'-separated
// parts, each with an optional (case-sensitive) prefix, a name (case-insensitive, the whole part) or a decimal
// number; the values are or-ed. (Hex and octal numbers, which func_ParseNum 0x8d8fa0 also takes, are left out.)
struct EnumName {
  const char* name;
  int value;
};
int EnumArg(lua_State* L, int idx, const char* prefix, const EnumName* names, int count) {
  const char* s = lua_tostring(L, idx);
  if (!s) luaL_typerror(L, idx, "string");
  const size_t plen = std::strlen(prefix);
  int v = 0;
  const char* p = s;
  for (;;) {
    const char* bar = std::strchr(p, '|');
    const char* end = bar ? bar : p + std::strlen(p);
    const char* q = std::strncmp(p, prefix, plen) == 0 ? p + plen : p;
    const size_t len = end > q ? static_cast<size_t>(end - q) : 0;
    bool found = false;
    for (int i = 0; i < count; ++i)
      if (strncasecmp(q, names[i].name, len) == 0 && names[i].name[len] == 0) {
        v |= names[i].value;
        found = true;
        break;
      }
    if (!found) {
      bool neg = false;
      const char* d = q;
      if (d < end && *d == '-') {
        neg = true;
        ++d;
      }
      int num = 0;
      bool ok = d < end;
      for (; d < end; ++d) {
        if (*d < '0' || *d > '9') {
          ok = false;
          break;
        }
        num = num * 10 + (*d - '0');
      }
      if (!ok) {
        std::string msg = std::string("Invalid enum value ") + s + "\nValid Options are:\n";
        for (int i = 0; i < count; ++i) msg += std::string("\n   ") + names[i].name;
        luaL_error(L, "%s", msg.c_str());
      }
      v |= neg ? -num : num;
    }
    if (!bar) break;
    p = bar + 1;
  }
  return v;
}
const EnumName kAllianceNames[] = {{"Neutral", 0}, {"Ally", 1}, {"Enemy", 2}};
const EnumName kCompareNames[] = {{"Closest", 0}, {"Furthest", 1}, {"HighestValue", 2}, {"LeastDefended", 4}};
const EnumName kSquadNames[] = {{"Unassigned", 0}, {"Attack", 1}, {"Artillery", 2}, {"Guard", 3},
                                {"Support", 4},    {"Scout", 5}};

// func_GetCObj_EntityCategory 0x5575e0: an EntityCategory value or a gpg::BadRefCast error (its text inferred).
std::vector<uint64_t> CategoryArg(lua_State* L, int idx) {
  const uint64_t* c = ToCategory(L, idx);
  if (!c) luaL_error(L, "Expected an EntityCategory, got %s", lua_typename(L, lua_type(L, idx)));
  return std::vector<uint64_t>(c, c + CategoryWordCount());
}
bool InCat(const std::vector<uint64_t>& cat, const Unit* u) {
  const int e = u->blueprint ? u->blueprint->entityIndex : -1;
  return e >= 0 && static_cast<size_t>(e >> 6) < cat.size() && CategoryHas(cat.data(), e);
}
// The original's category sets keep only the words between their lowest and highest member, so "empty" (no words,
// PickBestAttackVector 0x57c535) is "no member" here.
bool CatEmpty(const std::vector<uint64_t>& cat) {
  for (uint64_t w : cat)
    if (w) return false;
  return true;
}
// rules vt+0x58 'STRUCTURE' minus rules vt+0x58 'MOBILE' (EntityCategory::Sub 0x557710): the default category of
// SetUpAttackVectorsToArmy and CenterOfArmy.
std::vector<uint64_t> StructureNotMobile(const Sim& sim) {
  std::vector<uint64_t> c(static_cast<size_t>(CategoryWordCount()), 0);
  const auto& cats = sim.blueprints().Categories();
  auto set = [&](const char* name, bool on) {
    auto it = cats.find(name);
    if (it == cats.end()) return;
    for (int i : it->second) {
      if (i < 0 || static_cast<size_t>(i >> 6) >= c.size()) continue;
      if (on) c[static_cast<size_t>(i >> 6)] |= uint64_t{1} << (i & 63);
      else c[static_cast<size_t>(i >> 6)] &= ~(uint64_t{1} << (i & 63));
    }
  };
  set("STRUCTURE", true);
  set("MOBILE", false);
  return c;
}

void PushArmyBrain(lua_State* L, const Army* a) {
  if (a && a->brain) PushObject(L, a->brain);  // IArmy vt+0x1c GetArmyBrain 0x6fdc90, then its LuaObject (+0x20)
  else lua_pushnil(L);
}

// CArmyImpl::GetUnits 0x700eb0: the army's units whose blueprint is in the category. The army keeps every unit in
// a per-blueprint list from its ctor (Unit::Unit, vt+0x94) to its dtor (vt+0x98), so killed units not yet purged
// are in it: our Army::units (ascending id, as the EntitySet the original fills).
std::vector<Unit*> ArmyUnitsIn(const Army* a, const std::vector<uint64_t>& cat) {
  std::vector<Unit*> out;
  if (!a) return out;
  for (Unit* u : a->units)
    if (InCat(cat, u)) out.push_back(u);
  return out;
}

// CAiBrain::CenterOfArmy 0x57a510: the mean position of the army's units in STRUCTURE - MOBILE that are neither
// dead (IUnit vt+0x28) nor queued for destruction (vt+0x2c); summed in set order, times 1/n; (0,0,0) without any.
// (The same as the platoons' CenterOfArmy in sim/ai_platoon.cpp, which is private to that file.)
Vec3 CenterOfArmy(const Sim& sim, const Army* a) {
  Vec3 c{0, 0, 0};
  int n = 0;
  for (Unit* u : ArmyUnitsIn(a, StructureNotMobile(sim))) {
    if (u->dead || u->destroyQueued) continue;
    c.x = u->position.x + c.x;
    c.y = u->position.y + c.y;
    c.z = u->position.z + c.z;
    ++n;
  }
  if (n == 0) return c;
  const float inv = 1.0f / static_cast<float>(n);
  return Vec3{c.x * inv, c.y * inv, c.z * inv};
}

// The units behind army index a's blips (its recon DB's blip list, ReconGetBlips 0x5c1590, each blip's source
// +0x270), in our order: army by army, ascending id. A unit whose entity is gone has no source any more (its blip's
// weak pointer is null): it is not in any army's list. (Its fake blips name the same unit, so they add nothing.)
template <class F>
void ForBlipSources(const Sim& sim, int a, F&& f) {
  if (a < 0 || a >= 16) return;
  for (const auto& army : sim.armies())
    for (Unit* u : army->units) {
      bool held = ArmyHasBlip(u, a);
      for (const ReconBlip* fb : u->fakeBlips)
        if (!held && (fb->rflags[a] & kReconInUse)) held = true;
      if (held) f(u);
    }
}

// ---- simple state --------------------------------------------------------------------------------------------------

// IsOpponentAIRunning 0x585f70: false with '/noai' on the command line (CFG_GetArgOption 0x41b560), else the sim var
// AI_RunOpponentAI (default 1). We have neither the option nor sim vars (they are set by console command only): true.
int l_IsOpponentAIRunning(lua_State* L) {
  Args(L, "Returns true if opponent AI should be running", 1, 1);
  B(L);
  lua_pushboolean(L, 1);
  return 1;
}
// SetCurrentPlan 0x586390: a string (LuaObject::IsString: a Lua string, not a number) becomes brain+0x40; anything
// else changes nothing. Nothing in the engine reads it.
int l_SetCurrentPlan(lua_State* L) {
  Args(L, "Set the current plan for this brain to run", 2, 2);
  BrainState& st = StateOf(B(L));
  if (lua_type(L, 2) == LUA_TSTRING) st.plan = lua_tostring(L, 2);
  return 0;
}
// SetCurrentEnemy 0x586670: nil clears brain+0x38; else the value must be a brain (SCR_FromLua_CAiBrain: a Lua
// error otherwise) and its army becomes the enemy (the brain's own army too).
int l_SetCurrentEnemy(lua_State* L) {
  Args(L, "Set the current enemy for this brain to attack", 2, 2);
  AiBrain* b = B(L);
  BrainState& st = StateOf(b);
  if (lua_isnil(L, 2)) {
    st.enemy = nullptr;
  } else {
    AiBrain* e = CheckObject<AiBrain>(L, 2);
    st.enemy = e ? e->army : nullptr;
  }
  if (Dbg())
    Logf(LogLevel::Info, "aibrain: tick %u army %d SetCurrentEnemy -> %d", S(L)->tick(), b->army ? b->army->index : 0,
         st.enemy ? st.enemy->index : 0);
  return 0;
}
// GetCurrentEnemy 0x5867f0: the enemy army's brain, nil without one.
int l_GetCurrentEnemy(lua_State* L) {
  Args(L, "Return this brain's current enemy", 1, 1);
  PushArmyBrain(L, StateOf(B(L)).enemy);
  return 1;
}
// GetNoRushTicks 0x591870: (float) army+0x1cc. The army's OnTick takes one off at its start while it is above 0
// (0x6ffd91), in every beat; so after T beats it is max(start - T, 0) for a start >= 0 (only FAF's NoRushOption sets
// it, never below 0; SetNoRushTimer 0x6fe290, the other writer, has no caller we make). Our beat counter is the
// game tick: it counts the beats whose army stage has run (Sim::Tick advances it before the economy). A script that
// runs inside the army stage of an army before ours (a stat trigger) would see this army one beat ahead (inferred:
// negligible).
int l_GetNoRushTicks(lua_State* L) {
  Args(L, "CAiBrain:GetNoRushTicks()", 1, 1);
  const BrainState& st = StateOf(B(L));
  const long long beats = S(L)->tick();
  long long v = st.noRushStart;
  if (v > 0) v = std::max<long long>(v - beats, 0);
  lua_pushnumber(L, static_cast<float>(static_cast<int>(v)));
  return 1;
}
// SetGreaterOf(statName, value) 0x587320: arg 3 must be a number (else the 'integer' type error), truncated to an
// int (_ftol2); the army's stat item of that name (made as an int stat when missing, 0x5945e0) takes it when it is
// greater than the item's int value (0x5932c0, which also makes the item an int stat, 0x40d2d0). (The function's
// float branch is unreachable: it tests the same type again.) Our army stats keep no types: the stored number is
// compared as an int (the original compares the item's 32 bits as an int, which differs only for a float stat).
int l_SetGreaterOf(lua_State* L) {
  Args(L, "SetGreaterOf(statname,val)", 3, 3);
  AiBrain* b = B(L);
  const char* name = lua_tostring(L, 2);
  if (!name) luaL_typerror(L, 2, "string");
  if (lua_type(L, 3) != LUA_TNUMBER) luaL_typerror(L, 3, "integer");
  const double d = lua_tonumber(L, 3);
  const int v = (d > -2147483649.0 && d < 2147483648.0) ? static_cast<int>(d) : static_cast<int>(0x80000000u);
  if (!b->army) return 0;
  auto it = b->army->stats.find(name);
  if (it == b->army->stats.end()) it = b->army->stats.emplace(name, 0.0f).first;
  if (v > static_cast<int>(it->second)) it->second = static_cast<float>(v);
  return 0;
}

// ---- FindClosestArmyWithBase -------------------------------------------------------------------------------------

// FindClosestArmyWithBase(alliance) 0x58e8b0: over the brain's blips, the source unit (not dead, IUnit vt+0x28)
// whose army stands in that alliance with ours and whose blueprint is in STRUCTURE, nearest (squared distance, 3D,
// strictly smaller; the start is +inf, 0x10ad7b4 = 0xf3f7d0) to the brain's CenterOfArmy: its army's brain, nil when
// there is none. It reads the units themselves, not where the blips show them. (Two structures at exactly the same
// distance: the original takes the first in its blip list, we the first in army and id order.)
int l_FindClosestArmyWithBase(lua_State* L) {
  Args(L, "CAiBrain:FindClosestArmyWithBase()", 2, 2);
  AiBrain* b = B(L);
  const int want = EnumArg(L, 2, "ALLIANCE_", kAllianceNames, 3);
  Sim& sim = *S(L);
  const Vec3 c = CenterOfArmy(sim, b->army);
  float best = std::numeric_limits<float>::infinity();
  const Army* found = nullptr;
  const int a = b->army ? b->army->index - 1 : -1;
  ForBlipSources(sim, a, [&](Unit* u) {
    if (u->dead) return;
    if (AllianceWith(b->army, u->army) != want) return;
    if (!BpInCategory(sim, u->blueprint, "STRUCTURE")) return;
    const float dx = c.x - u->position.x, dy = c.y - u->position.y, dz = c.z - u->position.z;
    float d = dz * dz + dy * dy;
    d = d + dx * dx;
    if (best > d) {
      best = d;
      found = u->army;
    }
  });
  if (Dbg())
    Logf(LogLevel::Info, "aibrain: tick %u army %d FindClosestArmyWithBase(%d) -> %d", sim.tick(),
         b->army ? b->army->index : 0, want, found ? found->index : 0);
  PushArmyBrain(L, found);
  return 1;
}

// ---- attack vectors ----------------------------------------------------------------------------------------------

// CAiBrain::ProcessAttackVectors 0x57bdb0. Nothing without an enemy (brain+0x38; the old vectors stay). Else the
// vectors are made anew: the map (the heightfield's size - 1, which is TerrainMap::width/height) is cut into cells
// of 32 (cols = width >> 5, rows = height >> 5); a cell is taken when the enemy (army vt+0x9c GetUnits with the
// brain's category, then vt+0x8c CountUnitsInBoundsXZ 0x700a00) has a unit whose position lies in
// [cx - 16, cx + 16] x [cz - 16, cz + 16], bounds included, around the cell's centre (16 + 32 col, 16 + 32 row).
// Then, row by row and column by column, every cell NOT taken gets one vector per taken cell among its eight
// neighbours (neighbour rows, then columns, ascending): from its centre (y 0) to the neighbour's centre.
void ProcessAttackVectors(Sim& sim, AiBrain* b, BrainState& st) {
  if (!st.enemy) return;
  const TerrainMap* map = sim.map();
  const int cols = map ? static_cast<int>(static_cast<unsigned>(map->width()) >> 5) : 0;
  const int rows = map ? static_cast<int>(static_cast<unsigned>(map->height()) >> 5) : 0;
  st.vectors.clear();
  const std::vector<Unit*> units = ArmyUnitsIn(st.enemy, st.vectorCat);
  std::vector<std::vector<bool>> taken(static_cast<size_t>(std::max(rows, 0)));
  for (int r = 0; r < rows; ++r) {
    const float zc = static_cast<float>(16 + 32 * r);
    const float z0 = zc - 16.0f, z1 = zc + 16.0f;  // (0xe4f938)
    for (int k = 0; k < cols; ++k) {
      const float xc = static_cast<float>(16 + 32 * k);
      const float x0 = xc - 16.0f, x1 = xc + 16.0f;
      int n = 0;
      for (const Unit* u : units) {
        const Vec3& p = u->position;  // IUnit vt+0x14 GetPosition 0x6a49b0
        // (comiss / ja: a NaN coordinate counts as inside)
        if (x0 > p.x || p.x > x1 || z0 > p.z || p.z > z1) continue;
        ++n;
      }
      taken[static_cast<size_t>(r)].push_back(n > 0);
    }
  }
  for (int r = 0; r < rows; ++r)
    for (int k = 0; k < cols; ++k) {
      if (taken[static_cast<size_t>(r)][static_cast<size_t>(k)]) continue;
      const float xc = static_cast<float>(16 + 32 * k), zc = static_cast<float>(16 + 32 * r);
      const int r0 = std::max(r - 1, 0), r1 = std::min(r + 1, rows - 1);
      const int k0 = std::max(k - 1, 0), k1 = std::min(k + 1, cols - 1);
      for (int nr = r0; nr <= r1; ++nr)
        for (int nk = k0; nk <= k1; ++nk) {
          if (!taken[static_cast<size_t>(nr)][static_cast<size_t>(nk)]) continue;
          AttackVector v;
          v.px = xc;
          v.py = 0;
          v.pz = zc;
          v.vx = static_cast<float>(16 + 32 * nk) - xc;
          v.vy = 0;
          v.vz = static_cast<float>(16 + 32 * nr) - zc;
          st.vectors.push_back(v);
        }
    }
  if (Dbg())
    Logf(LogLevel::Info, "aibrain: tick %u army %d attack vectors to army %d: %zu (%zu units)", sim.tick(),
         b->army ? b->army->index : 0, st.enemy->index, st.vectors.size(), units.size());
}

// SetUpAttackVectorsToArmy([category]) 0x58ebc0: a category (not nil) becomes brain+0x70, else STRUCTURE - MOBILE
// does; then ProcessAttackVectors.
int l_SetUpAttackVectorsToArmy(lua_State* L) {
  Args(L, "CAiBrain:SetUpAttackVectorsToArmy()", 1, 2);
  AiBrain* b = B(L);
  BrainState& st = StateOf(b);
  if (lua_gettop(L) > 1 && lua_type(L, 2) != LUA_TNIL) st.vectorCat = CategoryArg(L, 2);
  else st.vectorCat = StructureNotMobile(*S(L));
  ProcessAttackVectors(*S(L), b, st);
  return 0;
}

// SCR_ToLua<SPointVector> 0x4d0350: a new table (LuaObject::AssignNewTable(L, 0, 6) 0x909940 = luaH_new with
// lnhash luaO_log2(6) + 1 = 3), then px py pz vx vy vz in this order.
void PushVector(lua_State* L, const AttackVector& v) {
  lua_newtablesized(L, 0, 3);
  const char* const k[6] = {"px", "py", "pz", "vx", "vy", "vz"};
  const float f[6] = {v.px, v.py, v.pz, v.vx, v.vy, v.vz};
  for (int i = 0; i < 6; ++i) {
    lua_pushstring(L, k[i]);
    lua_pushnumber(L, f[i]);
    lua_settable(L, -3);
  }
}
// GetAttackVectors 0x58ede0: nil when there are none, else an array of them (AssignNewTable(L, n, 0), Insert).
int l_GetAttackVectors(lua_State* L) {
  Args(L, "CAiBrain:GetAttackVectors()", 1, 1);
  const std::vector<AttackVector> v = StateOf(B(L)).vectors;  // (0x585a40 copies them)
  if (v.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtablesized(L, static_cast<int>(v.size()), 0);
  int n = 0;
  for (const AttackVector& a : v) {
    PushVector(L, a);
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// ---- PickBestAttackVector ----------------------------------------------------------------------------------------

// CSquad::GetCenter 0x724020: the mean of the squad's unit positions (all of them; summed in set order, times
// 1/n); (0,0,0) for an empty squad.
Vec3 SquadCenter(const Squad* s) {
  Vec3 c{0, 0, 0};
  if (s->units.empty()) return c;
  for (const Unit* u : s->units) {
    c.x = u->position.x + c.x;
    c.y = u->position.y + c.y;
    c.z = u->position.z + c.z;
  }
  const float inv = 1.0f / static_cast<float>(s->units.size());
  return Vec3{c.x * inv, c.y * inv, c.z * inv};
}

// CSquad::FitsAt 0x724550: every live unit's footprint (Entity::GetFootprint 0x678880 = UnitFootprint: its
// AltFootprint while it uses that) fits with its origin cell at (round(x - sizeX / 2), round(z - sizeZ / 2)) (fistp:
// to nearest even; kept as int16). By the blueprint's motion type (bp+0x290, jump table 0x724724): Land and Biped
// need the land bit of OCCUPY_FootprintFits 0x7209e0 (with the caps of OCCUPY_MobileCheck 0x564ab0), Water and
// SurfacingSub its water bit, Amphibious, Hover and AmphibiousFloating any bit (OCCUPY_HoverFootprintFits 0x720b20);
// the Sub bit is dropped first for a unit in the Water layer (unit+0x120 == 8); None, Air and the rest always fit.
// FootprintFits clears Land and Seabed when a structure occupies the land grid there (unless the footprint's flag 1
// says to ignore structures) and Water when one occupies the water grid. Our occupancy grid does not tell land
// structures from water ones: a structure there clears both (inferred to be the same answer for the units the
// classic AI moves).
bool SquadFitsAt(Sim& sim, const Squad* s, const Vec3& pos) {
  for (const Unit* u : s->units) {
    if (!u || u->dead) continue;
    const MotionBlueprint* mb = u->motion.bp;
    if (!mb) continue;  // (no motion blueprint: MotionType None)
    int need;
    switch (mb->motionType) {
      case kMotionLand:
      case kMotionBiped: need = kLayerLand; break;
      case kMotionWater:
      case kMotionSurfacingSub: need = kLayerWater; break;
      case kMotionAmphibious:
      case kMotionHover:
      case kMotionAmphibiousFloating: need = 0xff; break;
      default: continue;
    }
    const NamedFootprint& fp = UnitFootprint(u);
    const float hz = static_cast<float>(fp.sizeZ) * 0.5f, hx = static_cast<float>(fp.sizeX) * 0.5f;
    const int cz = static_cast<int16_t>(static_cast<int>(std::nearbyint(pos.z - hz)));
    const int cx = static_cast<int16_t>(static_cast<int>(std::nearbyint(pos.x - hx)));
    const PathGrid* g = UnitFootprintGrid(sim, u);
    if (!g) continue;
    unsigned caps = g->Caps(cx, cz);
    if (u->layer == "Water") caps &= ~static_cast<unsigned>(kLayerSub);
    caps = FitsCaps(static_cast<uint8_t>(caps), (fp.flags & 1) != 0, g->BlockedLand(cx, cz), g->BlockedWater(cx, cz));
    if (!(caps & static_cast<unsigned>(need))) return false;
  }
  return true;
}

// sub_57B620 0x57b620 in the two modes PickBestAttackVector uses (0 nearest, 1 farthest): the brain's blip sources
// that are not dead (with Ally also the army's own units in the category, army vt+0x9c), each one whose army stands
// in `alliance` with ours and whose blueprint is NOT in the category (the test at 0x57b7d1 skips the members), by
// squared distance to `pos` (3D): nearest, strictly (mode 0), or farthest, strictly (mode 1); the first one always
// (the start is -1). The set is in ascending id order.
Unit* FindUnit(Sim& sim, AiBrain* b, int mode, int alliance, const std::vector<uint64_t>& cat, const Vec3& pos) {
  std::vector<Unit*> set;
  if (alliance == kAlly) set = ArmyUnitsIn(b->army, cat);
  const int a = b->army ? b->army->index - 1 : -1;
  ForBlipSources(sim, a, [&](Unit* u) {
    if (!u->dead) set.push_back(u);
  });
  std::sort(set.begin(), set.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  set.erase(std::unique(set.begin(), set.end()), set.end());
  Unit* best = nullptr;
  float bestVal = -1.0f;  // (0xe4f6e8)
  for (Unit* u : set) {
    if (AllianceWith(b->army, u->army) != alliance) continue;
    if (InCat(cat, u)) continue;
    const float dx = u->position.x - pos.x, dy = u->position.y - pos.y, dz = u->position.z - pos.z;
    float d = dz * dz + dy * dy;
    d = d + dx * dx;
    const bool better = mode == 0 ? bestVal > d : d > bestVal;
    if (better || 0.0f > bestVal) {
      best = u;
      bestVal = d;
    }
  }
  return best;
}

// func_GetUnitsAroundPoint 0x57b290 (brain, category, pos, radius, alliance): the units within the radius of pos
// (func_EntitiesAroundPoint 0x722350: x and z only, d^2 < r^2) that are not killed (+0x99) or queued for
// destruction (+0x1b9), whose army stands in `alliance` with ours (3: any), that are our own or that we hold a blip
// of (ReconGetBlip 0x5c15a0), and whose blueprint is in the category; ascending id.
std::vector<Unit*> UnitsAroundPoint(Sim& sim, AiBrain* b, const std::vector<uint64_t>& cat, const Vec3& pos, float r,
                                    int alliance) {
  std::vector<Unit*> out;
  const float r2 = r * r;
  const int a = b->army ? b->army->index - 1 : -1;
  sim.ForUnitsInRect(pos.x - r, pos.z - r, pos.x + r, pos.z + r, [&](Unit* u) {
    const float dx = u->position.x - pos.x, dz = u->position.z - pos.z;
    if (!(r2 > dx * dx + dz * dz)) return;
    if (u->dead || u->destroyQueued) return;
    if (alliance != 3 && AllianceWith(b->army, u->army) != alliance) return;
    if (u->army != b->army && !ArmyHasBlip(u, a)) return;
    if (!InCat(cat, u)) return;
    out.push_back(u);
  });
  std::sort(out.begin(), out.end(), [](const Unit* x, const Unit* y) { return x->id < y->id; });
  return out;
}

// A unit's value for HighestValue: Economy.BuildCostMass (bp+0x4ec) + BuildCostEnergy (bp+0x4e8).
float BpValue(Sim& sim, lua_State* L, const BlueprintInfo* bp) {
  if (!bp) return 0.0f;
  const int top = lua_gettop(L);
  sim.blueprints().PushTable(L, *bp);
  float e = 0, m = 0;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Economy");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "BuildCostEnergy");
      lua_gettable(L, -2);
      e = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
      lua_pushstring(L, "BuildCostMass");
      lua_gettable(L, -2);
      m = static_cast<float>(lua_tonumber(L, -1));
      lua_pop(L, 1);
    }
  }
  lua_settop(L, top);
  return m + e;
}

// CAiBrain::PickBestAttackVector 0x57c290 (platoon, squad, alliance, compare type, category, module, function).
// The squad is the platoon's first of that class. Without an enemy, that squad or a unit in it: (0,0,0,0,0,0).
// With a function name: import(module)[name] (an error is warned, 0xe19830, and the calls below then fail). From the
// squad's centre, each vector whose end (point + direction) the squad fits at, and, with the function, for which
// fn(centre.x, centre.z, end.x, end.z) is true (an error is warned, 0xe19880, and the vector stays in), is rated
// by the compare type (jump table 0x57c9cc):
//   Closest / Furthest: the squared distance (3D) from its end to the centre (the category empty) or to the nearest /
//     farthest unit sub_57B620 finds from the centre (+inf when there is none); the smallest / largest wins.
//   HighestValue: the summed BuildCostMass + BuildCostEnergy of the units within 32 of its end; the largest or an
//     equal one whose end is nearer the centre than the best one's POINT (as the original compares) wins.
//   LeastDefended: the number of units within 32 of its end; it wins when it is above 0 and the best so far is
//     larger or 0, or when there is no best yet (so a vector with none only wins as the first).
//   3 and anything above 4: nothing.
// The best so far starts at -1 and the first rated vector always wins. HighestValue and LeastDefended that end with
// a best of exactly 0 start over as Closest.
AttackVector PickBest(Sim& sim, lua_State* L, AiBrain* b, Platoon* p, int squadCls, int alliance, int cmp,
                      const std::vector<uint64_t>& cat, const char* module, const char* fn) {
  AttackVector best;  // (0xf3d21c: zeros)
  float bestVal = -1.0f;
  BrainState& st = StateOf(b);
  Squad* squad = nullptr;
  for (auto& s : p->squads)
    if (s->cls == squadCls) {
      squad = s.get();
      break;
    }
  if (!st.enemy || !squad || squad->units.empty()) return best;
  int fnRef = LUA_NOREF;
  if (fn) {
    // SCR_Import 0x4d3250, then LuaObject::GetByName 0x908f60 (a raw get: luaH_get; a type error for a module that
    // is no table). Either error is caught (0x57c3d3) and warned; the function then stays nil.
    const int top = lua_gettop(L);
    lua_getglobal(L, "import");
    lua_pushstring(L, module ? module : "");
    if (lua_pcall(L, 1, 1, 0) != 0) {  // (the warning shows the error's text, as the original's e.what())
      Logf(LogLevel::Warning, "Error loading user-supplied callback in CAiBrain::PickBestAttackVector: %s",
           lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    } else if (!lua_istable(L, -1)) {
      Logf(LogLevel::Warning, "Error loading user-supplied callback in CAiBrain::PickBestAttackVector: %s",
           (std::string("attempt to index a ") + lua_typename(L, lua_type(L, -1)) + " value").c_str());
    } else {
      lua_pushstring(L, fn);
      lua_rawget(L, -2);
      fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_settop(L, top);
  }
  const Vec3 c = SquadCenter(squad);
  if (Dbg() && cmp < 2 && !CatEmpty(cat)) {
    const Unit* u = FindUnit(sim, b, cmp, alliance, cat, c);
    Logf(LogLevel::Info, "aibrain: tick %u army %d PickBestAttackVector: squad centre (%g %g %g), unit %u (army %d) at "
         "(%g %g %g)", sim.tick(), b->army ? b->army->index : 0, c.x, c.y, c.z, u ? u->id : 0u,
         u && u->army ? u->army->index : 0, u ? u->position.x : 0.0f, u ? u->position.y : 0.0f,
         u ? u->position.z : 0.0f);
  }
  const std::vector<AttackVector> vectors = st.vectors;
  for (const AttackVector& v : vectors) {
    const Vec3 t{v.px + v.vx, v.vy + v.py, v.vz + v.pz};
    if (!SquadFitsAt(sim, squad, t)) continue;
    if (fn) {
      // LuaFunction::Call_Num4_bool 0x581c00
      const int top = lua_gettop(L);
      if (fnRef != LUA_NOREF) lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
      else lua_pushnil(L);
      lua_pushnumber(L, c.x);
      lua_pushnumber(L, c.z);
      lua_pushnumber(L, t.x);
      lua_pushnumber(L, t.z);
      if (lua_pcall(L, 4, 1, 0) != 0) {
        Logf(LogLevel::Warning, "Error running user-supplied callback in CAiBrain::PickBestAttackVector: %s",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_settop(L, top);
      } else {
        const bool ok = lua_toboolean(L, -1) != 0;
        lua_settop(L, top);
        if (!ok) continue;
      }
    }
    bool take = false;
    float val = 0;
    switch (cmp) {
      case 0:
      case 1: {
        float d;
        if (CatEmpty(cat)) {
          const float dx = t.x - c.x, dy = t.y - c.y, dz = t.z - c.z;
          d = dz * dz + dy * dy;
          d = d + dx * dx;
        } else if (Unit* u = FindUnit(sim, b, cmp, alliance, cat, c)) {
          const float dx = t.x - u->position.x, dy = t.y - u->position.y, dz = t.z - u->position.z;
          d = dz * dz + dy * dy;
          d = d + dx * dx;
        } else {
          d = std::numeric_limits<float>::infinity();  // (0x10ad480 = 0xf3f7d0)
        }
        take = (cmp == 0 ? bestVal > d : d > bestVal) || 0.0f > bestVal;
        val = d;
        break;
      }
      case 2: {
        float sum = 0;
        for (Unit* u : UnitsAroundPoint(sim, b, cat, t, 32.0f, alliance)) sum = BpValue(sim, L, u->blueprint) + sum;
        if (sum >= bestVal || 0.0f > bestVal) {
          if (sum != bestVal) {
            take = true;
          } else {
            const float bx = best.px - c.x, by = best.py - c.y, bz = best.pz - c.z;
            float db = bx * bx + by * by;
            db = db + bz * bz;
            const float tx = t.x - c.x, ty = t.y - c.y, tz = t.z - c.z;
            float dt = tx * tx + ty * ty;
            dt = dt + tz * tz;
            take = db > dt;
          }
        }
        val = sum;
        break;
      }
      case 4: {
        const float n = static_cast<float>(UnitsAroundPoint(sim, b, cat, t, 32.0f, alliance).size());
        take = ((bestVal > n || bestVal == 0.0f) && n > 0.0f) || 0.0f > bestVal;
        val = n;
        break;
      }
      default: break;
    }
    if (take) {
      best = v;
      bestVal = val;
    }
  }
  if (fnRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
  if ((cmp == 4 || cmp == 2) && bestVal == 0.0f) return PickBest(sim, L, b, p, squadCls, alliance, 0, cat, module, fn);
  return best;
}

// PickBestAttackVector(platoon, squad, alliance, compareType, category [, module, function]) 0x58efe0: 6 or 8
// arguments (7 is warned, 0xe1a9a0, and taken as 6). The platoon must exist (SCR_FromLua_CPlatoon 0x593af0). The
// vector, or nil when its point is (0,0,0) (memcmp of 12 bytes with zeros, 0x4f0a50).
int l_PickBestAttackVector(lua_State* L) {
  Args(L, "CAiBrain:PickBestAttackVector()", 6, 8);
  AiBrain* b = B(L);
  Platoon* p = CheckObject<Platoon>(L, 2);
  const int squad = EnumArg(L, 3, "SQUADCLASS_", kSquadNames, 6);
  const int alliance = EnumArg(L, 4, "ALLIANCE_", kAllianceNames, 3);
  const int cmp = EnumArg(L, 5, "COMPARE_", kCompareNames, 4);
  const std::vector<uint64_t> cat = CategoryArg(L, 6);
  const char* module = nullptr;
  const char* fn = nullptr;
  const int n = lua_gettop(L);
  if (n == 8) {
    module = lua_tostring(L, 7);
    if (!module) luaL_typerror(L, 7, "string");
    fn = lua_tostring(L, 8);
    if (!fn) luaL_typerror(L, 8, "string");
  } else if (n == 7) {
    Logf(LogLevel::Warning, "Error in CAiBrain::PickBestAttackVector: Expected 6 or 8 arguments, got 7 instead.");
  }
  const AttackVector v = PickBest(*S(L), L, b, p, squad, alliance, cmp, cat, module, fn);
  if (Dbg())
    Logf(LogLevel::Info, "aibrain: tick %u army %d PickBestAttackVector -> (%g %g %g) (%g %g %g)", S(L)->tick(),
         b->army ? b->army->index : 0, v.px, v.py, v.pz, v.vx, v.vy, v.vz);
  const uint32_t zero[3] = {0, 0, 0};
  if (std::memcmp(&v.px, zero, sizeof zero) == 0) {
    lua_pushnil(L);
    return 1;
  }
  PushVector(L, v);
  return 1;
}

// The army ctor's NoRushOption (0x6ff37d..0x6ff47d with FAF's patch at 0x12917de): when the option is not nil, its
// string's characters c each make acc = (10 * acc) & 0xffff and then add (c - '0') to acc's LOW BYTE only (add bl,
// dl); the timer is (600 * acc) & 0xffff beats; then 'Off' (exactly) makes it 0. (The patch's loop runs at least once,
// so an empty string reads one byte past it: taken here as 0.)
int NoRushTicksOf(const char* s) {
  if (!s) return 0;
  const size_t len = std::strlen(s);
  if (len == 0) return 0;
  uint32_t acc = 0;
  for (size_t i = 0; i < len; ++i) {
    acc = (10u * (acc & 0xffffu)) & 0xffffu;
    const uint8_t d = static_cast<uint8_t>(static_cast<uint8_t>(s[i]) - 0x30u);
    acc = (acc & 0xff00u) | static_cast<uint8_t>((acc & 0xffu) + d);
  }
  int ticks = static_cast<int>((600u * acc) & 0xffffu);
  if (std::strcmp(s, "Off") == 0) ticks = 0;
  return ticks;
}

}  // namespace

void InitBrainState(Sim& sim) {
  g_state.assign(sim.armies().size(), BrainState());
  lua_State* L = sim.L();
  if (!L) return;
  const int top = lua_gettop(L);
  int ticks = 0;
  bool option = false;
  lua_getglobal(L, "ScenarioInfo");
  const int si = lua_gettop(L);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "Options");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "NoRushOption");
      lua_gettable(L, -2);
      if (!lua_isnil(L, -1)) {
        option = true;
        ticks = NoRushTicksOf(lua_tostring(L, -1));  // (LuaObject::GetString)
      }
    }
  }
  // With the option given, the ctor also reads (0x6ff47d..0x6ff643) ScenarioInfo.norushradius (100 when nil) and
  // ScenarioInfo.norushoffsetX_<army name> / norushoffsetY_<army name> (both 0 unless both are given). Without it
  // the three are never written (taken as 0 here; the circle is drawn only while the timer runs).
  auto field = [&](const std::string& k, float* out) {
    if (!lua_istable(L, si)) return false;
    lua_pushstring(L, k.c_str());
    lua_gettable(L, si);
    const bool given = !lua_isnil(L, -1);
    if (given) *out = static_cast<float>(lua_tonumber(L, -1));  // LuaObject::GetNumber 0x907a30
    lua_pop(L, 1);
    return given;
  };
  float radius = 0.0f;
  if (option && !field("norushradius", &radius)) radius = 100.0f;  // 0xe4f744
  for (size_t i = 0; i < g_state.size(); ++i) {
    BrainState& st = g_state[i];
    st.noRushStart = ticks;
    if (!option) continue;
    st.noRushRadius = radius;
    float ox = 0.0f, oz = 0.0f;
    const std::string& n = sim.armies()[i]->name;
    if (!field("norushoffsetX_" + n, &ox) || !field("norushoffsetY_" + n, &oz)) ox = oz = 0.0f;
    st.noRushOffX = ox;
    st.noRushOffZ = oz;
  }
  lua_settop(L, top);
}

bool NoRushOf(const Sim& sim, int army, int* ticks, float* radius, float* offX, float* offZ) {
  if (army < 0 || static_cast<size_t>(army) >= g_state.size()) return false;
  const BrainState& st = g_state[static_cast<size_t>(army)];
  long long v = st.noRushStart;  // as GetNoRushTicks counts it between beats
  if (v > 0) v = std::max<long long>(v - static_cast<long long>(sim.tick()), 0);
  *ticks = static_cast<int>(v);
  *radius = st.noRushRadius;
  *offX = st.noRushOffX;
  *offZ = st.noRushOffZ;
  return true;
}

void RegisterBrainBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "IsOpponentAIRunning", l_IsOpponentAIRunning);
  SetMethod(L, "CAiBrain", "SetCurrentPlan", l_SetCurrentPlan);
  SetMethod(L, "CAiBrain", "SetCurrentEnemy", l_SetCurrentEnemy);
  SetMethod(L, "CAiBrain", "GetCurrentEnemy", l_GetCurrentEnemy);
  SetMethod(L, "CAiBrain", "GetNoRushTicks", l_GetNoRushTicks);
  SetMethod(L, "CAiBrain", "SetGreaterOf", l_SetGreaterOf);
  SetMethod(L, "CAiBrain", "FindClosestArmyWithBase", l_FindClosestArmyWithBase);
  SetMethod(L, "CAiBrain", "SetUpAttackVectorsToArmy", l_SetUpAttackVectorsToArmy);
  SetMethod(L, "CAiBrain", "GetAttackVectors", l_GetAttackVectors);
  SetMethod(L, "CAiBrain", "PickBestAttackVector", l_PickBestAttackVector);
}

}  // namespace moho
