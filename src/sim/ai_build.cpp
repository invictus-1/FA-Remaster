// CAiBrain's build functions and the unit queries of the classic AI scripts. See sim/ai_build.h.
//
// Read in the FA exe (build 3839), 2026-10-15 (cfunc = the Lua binding, then what it calls):
// - GetUnitBlueprint 0x586950: rules (sim+0x8c8) vt+0x34 FindUnitBlueprint(StringInitFilename(name)) 0x51e2e0, the
//   blueprint's Lua table (RBlueprint::GetLuaBlueprint 0x50df90) or nil.
// - FindUnit 0x588a20, FindUpgradeBP 0x588c90, FindUnitToUpgrade 0x588f30 (CUnitIterAllArmies 0x6b69d0 over the
//   army's id range of the sim's entity map; Unit::IsIdleState 0x6a7d60 = an empty command queue).
// - DecideWhatToBuild 0x589400 -> CAiBrain::CanBuildUnit 0x57a6d0 -> Unit::CanBuild 0x6a9e50.
// - CreateUnitNearSpot 0x589910, CreateResourceBuildingNearest 0x589e30 (Sim::CreateUnit 0x7489e0).
// - FindPlaceToBuild 0x58a4c0 (the deposit list sorted by MSVC std::sort 0x594890 / 0x5955c0 / 0x5958c0;
//   CInfluenceMap::GetThreatRect 0x715ff0; CAiBrain::CanBuildStructureAt 0x57cbb0).
// - BuildStructure 0x58b690 -> func_OrderBuildStructure 0x57a790 (FindFreeSpot 0x62b200, STIMap::IsWithin 0x62ca60,
//   UNIT_IssueCommand 0x6f12c0) and func_ScheduleBuildStructure 0x57ca20 (the brain's plan map brain+0x98:
//   operator[] 0x57d910, RebindBuildReservation 0x57cb30).
// - NumCurrentlyBuilding 0x58bac0, GetAvailableFactories 0x58bd30 -> 0x57aec0 (Unit::IsBusy 0x6a7d10, the builder's
//   IsBusy CAiBuilderImpl vt+0x24 0x59f480), CanBuildPlatoon 0x58c000 -> FindAvailableFactory 0x57ac30,
//   BuildPlatoon 0x58c510 / BuildUnit 0x58c8c0 -> CAiBrain::BuildUnit 0x57b1e0 -> 0x57b0c0, IsAnyEngineerBuilding
//   0x58cac0, GetNumUnitsAroundPoint 0x58e1c0 -> func_GetNumUnitsAroundPoint 0x57b480 (func_EntitiesAroundPoint
//   0x722350: the strict circle r^2 > dx^2 + dz^2), DebugGetSelection 0x75e320.
// - CPlatoon:LoadUnits 0x730970 -> CPlatoon::LoadUnits 0x728a70, CPlatoon:UnloadUnitsAtLocation 0x730b90 ->
//   CPlatoon::UnloadUnitsAtLocation 0x7291c0 (no script of the game or of the lab's mods calls either).
// What is inferred and not read is said where it is used.
#include "sim/ai_build.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/ai_platoon.h"
#include "sim/ai_threat.h"
#include "sim/blueprints.h"
#include "sim/build.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/enum_arg.h"
#include "sim/intel.h"
#include "sim/landnav.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/transport.h"
#include "sim/units.h"

namespace moho {

// One pending plan (brain+0x98 map value, 0x10 bytes): weak refs to the builder (+0x14 in the node) and to the order
// BuildStructure gave it (+0x1c), keyed by the (int) location the script asked for.
struct AiBuildPlan {
  uint32_t builder = 0;  // EntityRef (a weak ref: gone with the unit)
  std::weak_ptr<UnitCommand> order;
};
struct AiBuildPlans {
  std::map<std::pair<int, int>, AiBuildPlan> plans;  // (the key order only decides which plan is looked at first)
};

namespace {

Sim* S(lua_State* L) { return Sim::From(L); }
AiBrain* B(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

// "%s\n  expected %d args, but got %d" (0xe0a220) / "... between %d and %d args ..." (0xe0a270), every cfunc's first
// test.
void Args(lua_State* L, const char* doc, int lo, int hi) {
  const int n = lua_gettop(L);
  if (lo == hi && n != lo) luaL_error(L, "%s\n  expected %d args, but got %d", doc, lo, n);
  if (lo != hi && (n < lo || n > hi)) luaL_error(L, "%s\n  expected between %d and %d args, but got %d", doc, lo, hi, n);
}

const char* StrArg(lua_State* L, int idx) {  // lua_tostring + TypeError("string")
  const char* s = lua_tostring(L, idx);
  if (!s) luaL_typerror(L, idx, "string");
  return s;
}
float NumArg(lua_State* L, int idx) {  // lua_type == number + lua_tonumber
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "number");
  return static_cast<float>(lua_tonumber(L, idx));
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

bool EqualNoCase(const char* a, const char* b) {  // __stricmp == 0
  if (!a || !b) return false;
  for (;; ++a, ++b) {
    const int ca = std::tolower(static_cast<unsigned char>(*a)), cb = std::tolower(static_cast<unsigned char>(*b));
    if (ca != cb) return false;
    if (!ca) return true;
  }
}

// The list readers of the original's bindings: LuaObject::operator[](int) 0x9091e0 on a value that is no table, and
// LuaObject::GetString 0x907a90 on one that is no string (a number neither), are luaG_typeerror 0x9133a0: a Lua error.
void IndexableArg(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) luaL_error(L, "attempt to index a %s value", lua_typename(L, lua_type(L, idx)));
}
const char* ObjString(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TSTRING)
    luaL_error(L, "attempt to get as string a %s value", lua_typename(L, lua_type(L, idx)));
  return lua_tostring(L, idx);
}

// EAlliance (0x509e20: Neutral 0, Ally 1, Enemy 2) by name (sim/enum_arg.h, SCR_GetEnum 0x4d2550: 0x58a79b,
// 0x58e367). (3 is also our "no alliance given"; a script that passes 3 or 'Ally|Enemy' gets that here, where the
// exe compares 3 with GetAllianceWith and so counts no unit.)
int AllianceEnumArg(lua_State* L, int idx) { return EnumValue(L, kEnumAlliance, StrArg(L, idx)); }
// IArmy::GetAllianceWith 0x579430 as an EAlliance; an army is its own ally. (Our Army::alliance: 0 enemy, 1 neutral,
// 2 ally.)
int AllianceOf(const Army* a, const Army* b) {
  if (!a || !b) return 0;
  if (a == b) return 1;
  const size_t i = static_cast<size_t>(b->index - 1);
  const int rel = i < a->alliance.size() ? a->alliance[i] : 1;
  return rel == 2 ? 1 : rel == 0 ? 2 : 0;
}

// The tests the brain functions make through IUnit's vtable: +0x28 dead, +0x2c destroy queued, +0x34 being built,
// +0x30 mobile.
bool Dead(const Unit* u) { return u->dead; }
bool Gone(const Unit* u) { return u->dead || u->destroyQueued; }
bool Mobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }
bool InCat(const BlueprintInfo* bp, const uint64_t* cat) {
  return cat && bp && bp->entityIndex >= 0 && CategoryHas(cat, bp->entityIndex);
}

// The rules' FindUnitBlueprint (vt+0x34) after StringInitFilename 0x51e2e0: a UNIT blueprint by its id, any case.
const BlueprintInfo* FindUnitBp(Sim& sim, const char* name) {
  if (!name) return nullptr;
  const BlueprintInfo* bp = sim.blueprints().Find(name);
  return bp && bp->kind == BpKind::Unit ? bp : nullptr;
}

// CArmyImpl vt+0x9c (the army's units in a category, an EntitySet: ascending entity id). The callers test dead /
// destroy-queued themselves.
std::vector<Unit*> ArmyUnitsIn(Army* a, const uint64_t* cat) {
  std::vector<Unit*> out;
  if (!a) return out;
  for (Unit* u : a->units)
    if (InCat(u->blueprint, cat)) out.push_back(u);
  return out;
}
std::vector<Unit*> ArmyUnitsInNamed(Sim& sim, Army* a, const char* name, const char* minus = nullptr) {
  std::vector<Unit*> out;
  if (!a) return out;
  for (Unit* u : a->units)
    if (BpInCategory(sim, u->blueprint, name) && !(minus && BpInCategory(sim, u->blueprint, minus))) out.push_back(u);
  return out;
}

// Unit::IsBusy 0x6a7d10: a mobile unit whose navigator is not idle (unit+0x54c vt+0x24: GetStatus, inferred), or a
// builder (unit+0x554) whose command queue's head is a BuildFactory (7), BuildMobile (8), Upgrade (27) or Script (28)
// order (CAiBuilderImpl vt+0x24 0x59f480: queue unit+0x4b4, the head's type at cmd+0x98).
bool UnitIsBusy(Sim& sim, const Unit* u) {
  if (Mobile(u)) {
    int st = 0;  // (as Lua's navigator:GetStatus: commands.cpp l_nav_GetStatus)
    if (u->motion.navDriven) st = LandNavStatus(u);
    else if (u->motion.hasGoal) st = sim.tick() < u->motion.driveTick ? 1 : 2;
    if (st != 0) return true;
  }
  if (u->bpData && u->bpData->hasBuilder && !u->commands.empty() && u->commands.front()) {
    const CommandType t = u->commands.front()->type;
    if (t == CommandType::BuildFactory || t == CommandType::BuildMobile || t == CommandType::Script ||
        t == CommandType::Upgrade)
      return true;
  }
  return false;
}

// floor of a float as the cfuncs take it: frndint, minus one when that rounded up (0x58c6f9).
int FloorF(float x) {
  const float r = std::nearbyint(x);
  return static_cast<int>(r) + (x < r ? -1 : 0);
}

// ---- MSVC std::sort (VS2005/2008 <algorithm>), as FindPlaceToBuild and CreateResourceBuildingNearest sort their
// deposit lists (_Sort 0x594890, _Unguarded_partition 0x5955c0, _Insertion_sort 0x5958c0, the heap 0x595bd0 /
// 0x595c40): not stable, so deposits at the same distance come out in the original's order only with its algorithm.
struct Spot {
  float x, z, d;  // the deposit's centre and its distance squared (the sort key, +8)
};
bool SpotLess(const Spot& a, const Spot& b) { return a.d < b.d; }

void Med3(Spot* first, Spot* mid, Spot* last) {
  if (SpotLess(*mid, *first)) std::swap(*mid, *first);
  if (SpotLess(*last, *mid)) std::swap(*last, *mid);
  if (SpotLess(*mid, *first)) std::swap(*mid, *first);
}
void Median(Spot* first, Spot* mid, Spot* last) {
  if (40 < last - first) {
    const ptrdiff_t step = (last - first + 1) / 8;
    Med3(first, first + step, first + 2 * step);
    Med3(mid - step, mid, mid + step);
    Med3(last - 2 * step, last - step, last);
    Med3(first + step, mid, last - step);
  } else {
    Med3(first, mid, last);
  }
}
std::pair<Spot*, Spot*> UnguardedPartition(Spot* first, Spot* last) {
  Spot* mid = first + (last - first) / 2;
  Median(first, mid, last - 1);
  Spot* pfirst = mid;
  Spot* plast = pfirst + 1;
  while (first < pfirst && !SpotLess(*(pfirst - 1), *pfirst) && !SpotLess(*pfirst, *(pfirst - 1))) --pfirst;
  while (plast < last && !SpotLess(*plast, *pfirst) && !SpotLess(*pfirst, *plast)) ++plast;
  Spot* gfirst = plast;
  Spot* glast = pfirst;
  for (;;) {
    for (; gfirst < last; ++gfirst) {
      if (SpotLess(*pfirst, *gfirst)) continue;
      if (SpotLess(*gfirst, *pfirst)) break;
      std::swap(*plast++, *gfirst);
    }
    for (; first < glast; --glast) {
      if (SpotLess(*(glast - 1), *pfirst)) continue;
      if (SpotLess(*pfirst, *(glast - 1))) break;
      std::swap(*--pfirst, *(glast - 1));
    }
    if (glast == first && gfirst == last) return {pfirst, plast};
    if (glast == first) {
      if (plast != gfirst) std::swap(*pfirst, *plast);
      ++plast;
      std::swap(*pfirst++, *gfirst++);
    } else if (gfirst == last) {
      if (--glast != --pfirst) std::swap(*glast, *pfirst);
      std::swap(*pfirst, *--plast);
    } else {
      std::swap(*gfirst++, *--glast);
    }
  }
}
void InsertionSort(Spot* first, Spot* last) {
  if (first == last) return;
  for (Spot* next = first + 1; next != last; ++next) {
    const Spot val = *next;
    if (SpotLess(val, *first)) {
      std::copy_backward(first, next, next + 1);
      *first = val;
    } else {
      Spot* hole = next;
      for (Spot* prev = hole - 1; SpotLess(val, *prev); --prev) {
        *hole = *prev;
        hole = prev;
      }
      *hole = val;
    }
  }
}
void PushHeap(Spot* first, ptrdiff_t hole, ptrdiff_t top, Spot val) {
  for (ptrdiff_t idx = (hole - 1) / 2; top < hole && SpotLess(first[idx], val); idx = (hole - 1) / 2) {
    first[hole] = first[idx];
    hole = idx;
  }
  first[hole] = val;
}
void AdjustHeap(Spot* first, ptrdiff_t hole, ptrdiff_t bottom, Spot val) {
  const ptrdiff_t top = hole;
  ptrdiff_t idx = 2 * hole + 2;
  for (; idx < bottom; idx = 2 * idx + 2) {
    if (SpotLess(first[idx], first[idx - 1])) --idx;
    first[hole] = first[idx];
    hole = idx;
  }
  if (idx == bottom) {
    first[hole] = first[bottom - 1];
    hole = bottom - 1;
  }
  PushHeap(first, hole, top, val);
}
void MakeHeap(Spot* first, Spot* last) {
  const ptrdiff_t bottom = last - first;
  for (ptrdiff_t hole = bottom / 2; 0 < hole;) {
    --hole;
    AdjustHeap(first, hole, bottom, first[hole]);
  }
}
void SortHeap(Spot* first, Spot* last) {
  for (; 1 < last - first; --last) {
    const Spot val = *(last - 1);
    *(last - 1) = *first;
    AdjustHeap(first, 0, (last - 1) - first, val);
  }
}
void MsvcSort(Spot* first, Spot* last, ptrdiff_t ideal) {
  ptrdiff_t count;
  for (; 32 < (count = last - first) && 0 < ideal;) {
    const std::pair<Spot*, Spot*> mid = UnguardedPartition(first, last);
    ideal /= 2, ideal += ideal / 2;
    if (mid.first - first < last - mid.second) {
      MsvcSort(first, mid.first, ideal);
      first = mid.second;
    } else {
      MsvcSort(mid.second, last, ideal);
      last = mid.first;
    }
  }
  if (32 < count) {
    MakeHeap(first, last);
    SortHeap(first, last);
  } else if (1 < count) {
    InsertionSort(first, last);
  }
}
void SortSpots(std::vector<Spot>& v) {
  if (!v.empty()) MsvcSort(v.data(), v.data() + v.size(), static_cast<ptrdiff_t>(v.size()));
}

// The deposits of the kind the blueprint goes on (HYDROCARBON: hydrocarbon deposits, type 2; else mass, type 1),
// centre ((x0 + x1) * 0.5, (z0 + z1) * 0.5) and its distance squared to (px, pz), in the resources' list order
// (CSimResources vt+0xc), then sorted.
std::vector<Spot> DepositSpots(Sim& sim, const BlueprintInfo* bp, float px, float pz) {
  const int want = BpInCategory(sim, bp, "HYDROCARBON") ? 2 : 1;
  std::vector<Spot> v;
  for (const ResourceDeposit& d : sim.deposits) {
    if (d.type != want) continue;
    const float cx = static_cast<float>(d.x1 + d.x0) * 0.5f, cz = static_cast<float>(d.z1 + d.z0) * 0.5f;
    const float dx = px - cx, dz = pz - cz;
    v.push_back(Spot{cx, cz, dx * dx + dz * dz});
  }
  SortSpots(v);
  return v;
}

bool RectsOverlap(const float a[4], const float b[4]) {  // Rect2<float>::Overlaps 0x416c90: strict, both non-empty
  return b[2] > a[0] && a[2] > b[0] && b[3] > a[1] && a[3] > b[1] && a[2] > a[0] && a[1] < a[3] && b[2] > b[0] &&
         b[1] < b[3];
}

// func_GetUnitsAroundPoint 0x57b290 / func_GetNumUnitsAroundPoint 0x57b480 (brain, category, point, radius,
// alliance): the units around the point (func_EntitiesAroundPoint 0x722350: r^2 > dx^2 + dz^2) that are not dead or
// destroy-queued (entity +0x99 / +0x1b9, taken as ours), of that alliance (3: any), that the brain's army knows of
// (its own, or a blip: the recon DB vt+0x4c), in the category. In id order (the original's order is its spatial
// hash's; only the count and FindPlaceToBuild's emptiness test use it here).
template <class InCategory>
std::vector<Unit*> BrainUnitsAroundIf(Sim& sim, Army* army, const Vec3& p, float r, int alliance, InCategory&& inCat) {
  std::vector<Unit*> out;
  const int ai = army ? army->index - 1 : -1;
  const float r2 = r * r;
  sim.ForUnitsInRect(p.x - r, p.z - r, p.x + r, p.z + r, [&](Unit* u) {
    const float dx = u->position.x - p.x, dz = u->position.z - p.z;
    if (!(r2 > dx * dx + dz * dz)) return;
    if (Gone(u)) return;
    if (alliance != 3 && AllianceOf(army, u->army) != alliance) return;
    if (u->army != army && !ArmyHasBlip(u, ai)) return;
    if (!inCat(u)) return;
    out.push_back(u);
  });
  std::sort(out.begin(), out.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  return out;
}
std::vector<Unit*> BrainUnitsAround(Sim& sim, Army* army, const uint64_t* cat, const Vec3& p, float r, int alliance) {
  return BrainUnitsAroundIf(sim, army, p, r, alliance, [&](const Unit* u) { return InCat(u->blueprint, cat); });
}
// ... in the rules' STRUCTURE category (rules vt+0x58 "STRUCTURE", 0x57ccc0)
std::vector<Unit*> BrainStructuresAround(Sim& sim, Army* army, const Vec3& p, float r, int alliance) {
  return BrainUnitsAroundIf(sim, army, p, r, alliance,
                            [&](const Unit* u) { return BpInCategory(sim, u->blueprint, "STRUCTURE"); });
}

AiBuildPlans& PlansOf(AiBrain* b) {
  if (!b->buildPlans) b->buildPlans = std::make_shared<AiBuildPlans>();
  return *b->buildPlans;
}

}  // namespace

// ---- CAiBrain::CanBuildStructureAt 0x57cbb0 -----------------------------------------------------------------------

bool AiBuildPlansBlock(Sim& sim, AiBrain* brain, const BlueprintInfo& bp, float x, float z) {
  if (!brain || !brain->buildPlans) return false;
  auto& plans = brain->buildPlans->plans;
  float mine[4];
  bool haveMine = false;
  for (auto it = plans.begin(); it != plans.end();) {
    // 0x57d026..0x57d051: a plan whose order or builder is gone, or whose builder is dead, is erased (sub_57DA50).
    // (Our order object lives while a unit holds it; one no unit holds any more is taken as gone, inferred: the
    // original's CUnitCommand dies with its last queue.)
    std::shared_ptr<UnitCommand> c = it->second.order.lock();
    Entity* e = it->second.builder ? sim.FindEntity(it->second.builder) : nullptr;
    Unit* b = e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
    if (!c || c->units.empty() || !b || Dead(b)) {
      it = plans.erase(it);
      continue;
    }
    // the order's blueprint (cmd+0x60) at its target (CAiTarget::GetTargetPosGun 0x5e2a90 of cmd+0x11c): its skirt
    // (GetSkirtRect: a unit blueprint's vt+0x14 is not null; the footprint-sized fallback 0x57d0f1 is for others)
    const BlueprintInfo* pb = sim.blueprints().Find(c->blueprintId);
    if (pb && pb->kind == BpKind::Unit) {
      float pr[4];
      BlueprintSkirtRect(sim, *pb, c->pos.x, c->pos.z, pr);
      if (!haveMine) {
        BlueprintSkirtRect(sim, bp, x, z, mine);
        haveMine = true;
      }
      if (RectsOverlap(mine, pr)) return true;  // 0x57d1a5..0x57d1e3 -> 0x57d275
    }
    ++it;
  }
  return false;
}

bool AiBrainCanBuildStructureAt(Sim& sim, AiBrain* brain, const BlueprintInfo& bp, const Vec3& pos, int alliance) {
  Army* army = brain ? brain->army : nullptr;
  // FootprintFitsStructure 0x720d90 (ours: LocationIsFree, structure_placement.md 4)
  bool ok = LocationIsFree(sim, bp, pos.x, pos.z);
  // 0x57cda3..0x57cf9f run when it passed too; for a structure they find nothing new, for a mobile blueprint they
  // are its only skirt test.
  if (ok && !GetUnitBpData(sim.L(), bp).structure &&
      LiveStructureSkirtOverlaps(sim, bp, pos.x, pos.z, [&](const Unit* u) { return alliance != 3 && AllianceOf(army, u->army) == alliance; }))
    return false;
  if (!ok) {
    if (alliance == 3) return false;  // 0x57cc34
    // 0x57cc3d..0x57cd68: the structures around the point within the skirt's larger side: of the other side's
    // alliance (Ally when Enemy may stand there, else Enemy) and Neutral ones; Enemy: none of the first, else none of
    // either.
    float sk[4];
    BlueprintSkirtRect(sim, bp, pos.x, pos.z, sk);
    float size = sk[2] - sk[0];
    if (sk[3] - sk[1] > size) size = sk[3] - sk[1];
    const int other = alliance != 2 ? 2 : 1;
    const std::vector<Unit*> a = BrainStructuresAround(sim, army, pos, size, other);
    const std::vector<Unit*> n = BrainStructuresAround(sim, army, pos, size, 0);
    ok = alliance == 2 ? a.empty() : (a.empty() && n.empty());
    if (!ok) return false;
    // 0x57cda3..0x57cf9f: the units in the skirt's box (GatherUnmarkedUnitsInBox 0x721c00, y -1000..1000) that are
    // alive, not of the alliance that may stand there, not mobile, and whose skirt (0x6a7600) overlaps the new skirt.
    // (When the location test passed this finds nothing: FootprintFitsStructure tested every structure's skirt
    // already. Here the box query is ours: the units by position within 32 of the box, inferred to cover the
    // original's spatial hash.)
    bool hit = false;
    sim.ForUnitsInRect(sk[0] - 32, sk[1] - 32, sk[2] + 32, sk[3] + 32, [&](Unit* u) {
      if (hit || Gone(u) || !u->blueprint || !u->bpData) return;
      if (AllianceOf(army, u->army) == alliance) return;
      if (Mobile(u)) return;
      float ur[4];
      UnitSkirtRect(u, ur);
      if (RectsOverlap(sk, ur)) hit = true;
    });
    if (hit) return false;
  }
  return !AiBuildPlansBlock(sim, brain, bp, pos.x, pos.z);
}

namespace {

// ---- FindFreeSpot 0x62b200 for a structure (flags 0, 0: OrderBuildStructure) ----------------------------------

// STIMap::IsWithin 0x62ca60: the point with a margin inside the playable rect (map+8..+0x14), or with wholeMap the
// map (x + m < cells): Sim::IsWithin.
bool IsWithin(Sim& sim, float x, float z, float m, bool wholeMap) {
  if (!sim.map()) return false;
  return sim.IsWithin(x, z, m, wholeMap);
}

// The point itself, then square rings stepping by `size` (i over x outside, j over z inside; the ring's sides step
// by 2r), up to 900 candidates (the ring that passes 900 is finished): the first place where the structure fits
// (FootprintFitsStructure), written back snapped (its out.pos, COORDS_ToWorldPos). The IsWithin test is made on the
// ORIGINAL point every time (0x62b4ed passes the unchanged point), so it either fails at once or never.
bool StructureFreeSpot(Sim& sim, const BlueprintInfo& bp, int size, bool wholeMap, float* x, float* z) {
  const float m = static_cast<float>(size);
  if (!IsWithin(sim, *x, *z, m, wholeMap)) return false;
  auto take = [&](float cx, float cz) {
    const Vec3 s = SnapStructurePosition(sim, bp, Vec3{cx, 0, cz});
    *x = s.x;
    *z = s.z;
  };
  if (LocationIsFree(sim, bp, *x, *z)) {
    take(*x, *z);
    return true;
  }
  int tested = 0;
  for (int r = 1;; ++r) {
    for (int i = -r; i <= r; ++i) {
      const int jstep = (i == -r || i == r) ? 1 : 2 * r;
      const float xoff = static_cast<float>(i * size);
      for (int j = -r; j <= r; j += jstep) {
        ++tested;
        const float cx = xoff + *x, cz = static_cast<float>(j * size) + *z;
        if (LocationIsFree(sim, bp, cx, cz)) {
          take(cx, cz);
          return true;
        }
      }
    }
    if (tested >= 900) return false;
  }
}

// ---- Lua ---------------------------------------------------------------------------------------------------------

// GetUnitBlueprint(bpName) 0x586950
int l_GetUnitBlueprint(lua_State* L) {
  Args(L, "blueprint = brain:GetUnitBlueprint(bpName)", 2, 2);
  B(L);
  const char* name = StrArg(L, 2);
  const BlueprintInfo* bp = FindUnitBp(*S(L), name);
  if (bp) S(L)->blueprints().PushTable(L, *bp);
  else lua_pushnil(L);
  return 1;
}

// GetNumUnitsAroundPoint(category, position, radius [, alliance]) 0x58e1c0: alliance 3 (any) when not given.
int l_GetNumUnitsAroundPoint(lua_State* L) {
  Args(L, "CAiBrain:GetNumUnitsAroundPoint()", 4, 5);
  AiBrain* b = B(L);
  const uint64_t* cat = ToCategory(L, 2);
  const Vec3 p = VecArg(L, 3);
  const float r = NumArg(L, 4);
  const int alliance = lua_gettop(L) == 5 ? AllianceEnumArg(L, 5) : 3;
  const int n = static_cast<int>(BrainUnitsAround(*S(L), b->army, cat, p, r, alliance).size());
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}

// FindUnit(category, needToBeIdle) 0x588a20: the first of the army's units in the category (ascending id) that is
// not dead, destroy-queued or being built, and idle (an empty queue) when asked; nil if none.
int l_FindUnit(lua_State* L) {
  Args(L, "brain:FindUnit(unitCategory, needToBeIdle) -- Return an unit that matches the unit name (can specify idle or not)", 3, 3);
  AiBrain* b = B(L);
  const uint64_t* cat = ToCategory(L, 2);
  const bool idle = lua_toboolean(L, 3) != 0;
  for (Unit* u : ArmyUnitsIn(b->army, cat)) {
    if (Gone(u) || u->beingBuilt) continue;
    if (idle && !u->commands.empty()) continue;  // Unit::IsIdleState 0x6a7d60
    PushObject(L, u);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

// FindUpgradeBP(unitName, upgradeList) 0x588c90: the [2] of the first entry whose [1] is the name (any case).
// i = 1 .. GetCount (0x588d70, read once); an entry that is no table (a nil hole included) is skipped.
int l_FindUpgradeBP(lua_State* L) {
  Args(L, "brain:FindUpgradeBP(unitName, upgradeList) -- Return an upgrade blueprint for the unit passed in", 3, 3);
  B(L);
  const char* name = StrArg(L, 2);
  const int n = lua_istable(L, 3) ? ObjectCount(L, 3) : 0;
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 3, i);
    const int e = lua_gettop(L);
    if (lua_istable(L, e)) {
      lua_rawgeti(L, e, 1);
      const bool match = EqualNoCase(name, lua_tostring(L, -1));
      lua_pop(L, 1);
      if (match) {
        lua_rawgeti(L, e, 2);
        const char* to = lua_tostring(L, -1);
        lua_pushstring(L, to ? to : "");
        return 1;
      }
    }
    lua_pop(L, 1);
  }
  lua_pushnil(L);
  return 1;
}

// FindUnitToUpgrade(upgradeList) 0x588f30: per entry {from, to} whose two blueprints exist, the first of the army's
// units (CUnitIterAllArmies 0x6b69d0: the army's id range, ascending) that is not dead or being built, is idle and
// is a `from` (its blueprint id, any case); returns the unit and the `to` blueprint's id. nil, nil if none.
// i = 1 .. GetCount (0x588ffb, read once); an entry that is no table (a nil hole included) is skipped.
int l_FindUnitToUpgrade(lua_State* L) {
  Args(L, "brain:FindUnitToUpgrade(upgradeList) -- Return a unit and it's upgrade blueprint", 2, 2);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const int n = lua_istable(L, 2) ? ObjectCount(L, 2) : 0;
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    const int e = lua_gettop(L);
    if (lua_istable(L, e)) {
      lua_rawgeti(L, e, 1);
      const BlueprintInfo* from = FindUnitBp(sim, lua_tostring(L, -1));
      lua_rawgeti(L, e, 2);
      const BlueprintInfo* to = FindUnitBp(sim, lua_tostring(L, -1));
      lua_pop(L, 2);
      if (from && to && b->army) {
        for (Unit* u : b->army->units) {
          if (Dead(u) || u->beingBuilt || !u->commands.empty() || !u->blueprint) continue;
          if (!EqualNoCase(from->id.c_str(), u->blueprint->id.c_str())) continue;
          PushObject(L, u);
          lua_pushstring(L, to->id.c_str());
          return 2;
        }
      }
    }
    lua_pop(L, 1);
  }
  lua_pushnil(L);
  lua_pushnil(L);
  return 2;
}

// DecideWhatToBuild(builder, type, buildingTypes) 0x589400: in the first entries whose [1] is the type (any case),
// the first of [2..] the builder can build (CAiBrain::CanBuildUnit 0x57a6d0: the unit blueprint exists and
// Unit::CanBuild); the entry's value itself. nil if none (or the list is no table / empty). Both loops run to
// GetCount (0x58954c / 0x589630, read again each pass; nothing here changes the lists): an entry that is no table, or
// whose [1] is no string, and a [j] that is no string (a nil hole included) are Lua errors (IndexableArg, ObjString).
int l_DecideWhatToBuild(lua_State* L) {
  Args(L, "brain:DecideWhatToBuild(builder, type, buildingTypes)", 4, 4);
  B(L);
  Sim& sim = *S(L);
  Unit* builder = CheckObject<Unit>(L, 2);
  const char* type = StrArg(L, 3);
  if (!lua_istable(L, 4) || ObjectCount(L, 4) < 1) {
    lua_pushnil(L);
    return 1;
  }
  const int n = ObjectCount(L, 4);
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 4, i);
    const int e = lua_gettop(L);
    IndexableArg(L, e);
    lua_rawgeti(L, e, 1);
    const bool match = EqualNoCase(ObjString(L, -1), type);
    lua_pop(L, 1);
    if (match) {
      const int m = ObjectCount(L, e);
      for (int j = 2; j <= m; ++j) {
        lua_rawgeti(L, e, j);
        const BlueprintInfo* bp = FindUnitBp(sim, ObjString(L, -1));
        if (bp && UnitCanBuild(builder, *bp)) return 1;  // the entry's value
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  lua_pushnil(L);
  return 1;
}

// FindPlaceToBuild(type, structureName, buildingTypes, relative, builder [, ignoreAlliance [, overrideX, overrideZ
// [, ignoreThreatOver]]]) 0x58a4c0.
int l_FindPlaceToBuild(lua_State* L) {
  Args(L, "brain:FindPlaceToBuild(type, structureName, buildingTypes, relative, builder, optIgnoreAlliance, "
          "optOverridePosX, optOverridePosZ, optIgnoreThreatOver)", 6, 10);
  if (lua_isnil(L, 2) || !lua_isstring(L, 2)) luaL_error(L, "Invalid Type name passed in");
  if (lua_isnil(L, 3) || !lua_isstring(L, 3)) luaL_error(L, "Invalid Structure name passed in");
  if (lua_isnil(L, 4) || !lua_istable(L, 4)) luaL_error(L, "Invalid buildingTypes passed in. Not a lua table!");
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const char* type = StrArg(L, 2);
  const char* name = StrArg(L, 3);
  const bool relative = lua_toboolean(L, 5) != 0;
  Unit* builder = !lua_isnil(L, 6) ? CheckObject<Unit>(L, 6) : nullptr;
  int alliance = 3;
  if (lua_gettop(L) >= 7 && !lua_isnil(L, 7)) alliance = AllianceEnumArg(L, 7);
  bool hasOverride = false;
  float ox = 0, oz = 0;
  if (lua_gettop(L) >= 9 && !lua_isnil(L, 8) && !lua_isnil(L, 9)) {
    hasOverride = true;
    oz = static_cast<float>(lua_tonumber(L, 9));
    ox = static_cast<float>(lua_tonumber(L, 8));
  }
  int ignoreThreat = 0;
  if (lua_gettop(L) >= 10 && !lua_isnil(L, 10)) ignoreThreat = static_cast<int>(lua_tonumber(L, 10));
  Army* army = b->army;
  const BlueprintInfo* bp = FindUnitBp(sim, name);
  if (!bp || !army) {  // (the original goes on with a null blueprint and would fault in its tests)
    lua_pushnil(L);
    return 1;
  }
  const int ai = army->index - 1;
  float sx = army->startX, sz = army->startZ;  // army vt+0x30 (GetArmyStartPos)
  // the threat AntiSurface (11) of all armies in the point's cell of the army's own map (army vt+0x18), playable only
  auto threatAt = [&](float x, float z) {
    return ThreatRectAt(sim, ai, static_cast<float>(static_cast<int>(x)), static_cast<float>(static_cast<int>(z)), 0,
                        true, 11, -1);
  };
  const bool resource = EqualNoCase(type, "Resource") || EqualNoCase(type, "T1Resource") ||
                        EqualNoCase(type, "T2Resource") || EqualNoCase(type, "T3Resource") ||
                        EqualNoCase(type, "T1HydroCarbon");
  if (resource) {  // (not with the command line's "/nomass", 0x58a949: not kept)
    // the deposits of the blueprint's kind, nearest the start (or the override) first; the first not too threatened
    // where the brain can build: {x, z, 0} (relative to that point when asked)
    if (hasOverride) {
      sx = ox;
      sz = oz;
    }
    for (const Spot& s : DepositSpots(sim, bp, sx, sz)) {
      if (ignoreThreat > 0 && threatAt(s.x, s.z) >= static_cast<float>(ignoreThreat)) continue;
      if (!AiBrainCanBuildStructureAt(sim, b, *bp, Vec3{s.x, 0, s.z}, alliance)) continue;
      float x = s.x, z = s.z;
      if (relative) {
        x = x - sx;
        z = z - sz;
      }
      lua_newtable(L);
      lua_pushnumber(L, x);
      lua_rawseti(L, -2, 1);
      lua_pushnumber(L, z);
      lua_rawseti(L, -2, 2);
      lua_pushnumber(L, 0);
      lua_rawseti(L, -2, 3);
      return 1;
    }
    lua_pushnil(L);
    return 1;
  }
  // The base templates { { {types...}, {x, z, ...}, {x, z, ...}, ... }, ... }: over every template whose type list
  // holds the type, the location nearest the builder (3D, y 0; the start without a builder; the override point if
  // given) where the brain can build, whose skirt touches no deposit (strict) and, when asked, whose threat is below
  // the limit. The location's own table is returned (relative locations stay relative).
  Vec3 pos{sx, 0, sz};
  if (builder) pos = builder->position;  // vt+0x14
  if (hasOverride) {
    pos = Vec3{ox, 0, oz};
    sx = ox;
    sz = oz;
  }
  const float big = std::numeric_limits<float>::max();  // 0x10ad7b4 (runtime-set; taken as FLT_MAX, inferred)
  float best = big;
  lua_pushnil(L);
  const int bestIdx = lua_gettop(L);
  const float dy = -0.0f - pos.y;  // 0xe4f748 = -0.0
  const float dy2 = dy * dy;
  // Every loop runs to GetCount (0x58ae19 / 0x58aec9 / 0x58af4d, read again each pass; nothing here changes the
  // lists): a type name that is no string and a location that is no table (a nil hole included) are Lua errors
  // (GetString 0x907a90; the original takes GetCount of the location 0x58af96 and indexes it).
  const int nt = ObjectCount(L, 4);
  for (int i = 1; i <= nt; ++i) {
    lua_rawgeti(L, 4, i);
    const int tmpl = lua_gettop(L);
    if (!lua_istable(L, tmpl)) luaL_error(L, "Error parsing building types. Missing type list lua table!");
    lua_rawgeti(L, tmpl, 1);
    const int types = lua_gettop(L);
    if (!lua_istable(L, types)) luaL_error(L, "Error parsing building types. Missing type match lua table!");
    bool match = false;
    const int nm = ObjectCount(L, types);
    for (int j = 1; j <= nm && !match; ++j) {
      lua_rawgeti(L, types, j);
      match = EqualNoCase(ObjString(L, -1), type);
      lua_pop(L, 1);
    }
    if (match) {
      const int nl = ObjectCount(L, tmpl);
      for (int k = 2; k <= nl; ++k) {
        lua_rawgeti(L, tmpl, k);
        const int loc = lua_gettop(L);
        IndexableArg(L, loc);
        lua_rawgeti(L, loc, 2);
        const float lz = static_cast<float>(lua_tonumber(L, -1));
        lua_rawgeti(L, loc, 1);
        const float lx = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 2);
        float x = lx, z = lz;
        if (relative) {
          x = lx + sx;
          z = sz + lz;
        }
        const float ddx = x - pos.x, ddz = z - pos.z;
        const float d = (ddx * ddx + ddz * ddz) + dy2;
        if (best > d && AiBrainCanBuildStructureAt(sim, b, *bp, Vec3{x, 0, z}, alliance)) {
          bool ok = true;
          float sk[4];
          BlueprintSkirtRect(sim, *bp, x, z, sk);
          for (const ResourceDeposit& dep : sim.deposits) {  // every deposit, of either kind
            const float dr[4] = {static_cast<float>(dep.x0), static_cast<float>(dep.z0), static_cast<float>(dep.x1),
                                 static_cast<float>(dep.z1)};
            if (RectsOverlap(sk, dr)) {
              ok = false;
              break;
            }
          }
          bool calm = true;
          if (ignoreThreat > 0) calm = static_cast<float>(ignoreThreat) > threatAt(x, z);
          if (ok && calm) {
            lua_pushvalue(L, loc);
            lua_replace(L, bestIdx);
            best = d;
          }
        }
        lua_settop(L, loc - 1);
      }
    }
    lua_settop(L, tmpl - 1);
  }
  if (big > best) lua_pushvalue(L, bestIdx);
  else lua_pushnil(L);
  return 1;
}

// func_OrderBuildStructure 0x57a790 (end, brain, builder, name, pos, angle): the structure goes to the first free spot
// at or around the point (FindFreeSpot: rings of max(8, footprint size), the playable rect unless the army may build
// outside it: the brain's army's UseWholeMap, vt+0xb8 at 0x57a8f8); the builder gets a BuildMobile order there without clear
// (UNIT_IssueCommand 0x6f12c0): the snapped spot at the terrain's elevation (STIMap::GetElevation 0x44fb90), the end
// point as its second target, the footprint origin as its cell, an orientation of `angle` degrees about y. nullptr
// when the blueprint or the spot is not found.
std::shared_ptr<UnitCommand> OrderBuildStructure(Sim& sim, Unit* builder, const char* name, Vec3 pos, float angle,
                                                 const Vec3& end) {
  const float h = static_cast<float>(static_cast<double>(angle) * static_cast<double>(0.01745329238474369f) * -0.5);
  const float sn = std::sin(h), cs = std::cos(h);  // sinf 0x5734e0 / cosf 0x5734d0
  const BlueprintInfo* bp = FindUnitBp(sim, name);
  if (!bp || !builder) return nullptr;
  const NamedFootprint fp = bp->hasFootprint ? bp->footprint : NamedFootprint{"", 1, 1, 1, 0, 0, 0, 0};
  int size = std::max(fp.sizeX, fp.sizeZ);
  size = std::max(size, 8);
  float x = pos.x, z = pos.z;
  if (!StructureFreeSpot(sim, *bp, size, Sim::UseWholeMap(builder->army), &x, &z)) return nullptr;
  const float y = sim.map() ? sim.map()->TerrainHeight(x, z) : 0.0f;
  StreamIssueData d;
  d.type = static_cast<int>(CommandType::BuildMobile);
  d.target.type = 2;
  d.target.pos = Vec3{x, y, z};
  d.target2.type = 2;
  d.target2.pos = end;
  d.qw = cs;
  d.qx = 0;
  d.qy = sn;
  d.qz = 0;
  d.blueprint = bp->id;
  const int16_t cx = static_cast<int16_t>(std::nearbyint(x - static_cast<float>(fp.sizeX) * 0.5f));  // fistp
  const int16_t cz = static_cast<int16_t>(std::nearbyint(z - static_cast<float>(fp.sizeZ) * 0.5f));
  d.cells.push_back({cx, cz});
  return IssueFromStream(sim, {builder}, d, false);
}

// BuildStructure(builder, structureName, locationInfo [, relative]) 0x58b690: locationInfo {x, z, angle [, endX,
// endZ]}; relative adds the army's start. The order (above) is then kept as the brain's plan at the (int) location
// asked for (func_ScheduleBuildStructure 0x57ca20: the map's operator[], so a plan at the same key is replaced).
// Returns nothing of its own (the cfunc answers 1 without a push: the top argument).
int l_BuildStructure(lua_State* L) {
  Args(L, "brain:BuildStructure(builder, structureName, locationInfo)", 4, 5);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  Army* army = b->army;
  Unit* builder = CheckObject<Unit>(L, 2);
  Vec3 pos, end;
  float angle = 0;
  if (lua_istable(L, 4)) {
    lua_rawgeti(L, 4, 2);
    lua_rawgeti(L, 4, 1);
    pos.x = static_cast<float>(lua_tonumber(L, -1));
    pos.z = static_cast<float>(lua_tonumber(L, -2));
    lua_rawgeti(L, 4, 3);
    angle = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 3);
    if (ObjectCount(L, 4) > 3) {  // GetCount 0x58b8a1
      lua_rawgeti(L, 4, 5);
      lua_rawgeti(L, 4, 4);
      end.x = static_cast<float>(lua_tonumber(L, -1));
      end.z = static_cast<float>(lua_tonumber(L, -2));
      lua_pop(L, 2);
    }
  }
  if (lua_gettop(L) > 4 && lua_toboolean(L, 5) && army) {
    pos.x = army->startX + pos.x;
    pos.z = army->startZ + pos.z;
  }
  const char* name = lua_tostring(L, 3);  // LuaObject::GetString
  std::shared_ptr<UnitCommand> c = OrderBuildStructure(sim, builder, name, pos, angle, end);
  AiBuildPlan& p = PlansOf(b).plans[{static_cast<int>(pos.x), static_cast<int>(pos.z)}];  // cvttss2si
  p.builder = EntityRef(builder);
  p.order = c;
  static const bool dbg = getenv("MOHO64_DEBUG_AIBUILD") != nullptr;
  if (dbg)
    Logf(LogLevel::Info, "aibuild %u BuildStructure army %d unit %u %s at (%.2f, %.2f) -> %s", sim.tick(),
         army ? army->index : 0, builder->id, name ? name : "?", pos.x, pos.z, c ? "ordered" : "no spot");
  return 1;
}

// CAiBrain::BuildUnit 0x57b1e0 -> 0x57b0c0: `count` BuildFactory orders (one each) in the factory's own queue
// (UNIT_IssueCommand without clear), when the unit is a builder (unit+0x554) and the blueprint exists.
bool BrainBuildUnit(Sim& sim, Unit* factory, const char* id, int count) {
  const BlueprintInfo* bp = FindUnitBp(sim, id);
  if (!bp || !factory || !factory->bpData || !factory->bpData->hasBuilder) return false;
  for (int i = count; i > 0; --i) {
    StreamIssueData d;
    d.type = static_cast<int>(CommandType::BuildFactory);
    d.blueprint = bp->id;
    IssueFromStream(sim, {factory}, d, false);
  }
  return true;
}

// BuildUnit(unit, blueprintId, count) 0x58c8c0 (only when the id is a string). Answers 1 without a push.
int l_BuildUnit(lua_State* L) {
  Args(L, "brain:BuildUnit()", 4, 4);
  B(L);
  Unit* factory = CheckObject<Unit>(L, 2);
  if (lua_type(L, 3) == LUA_TSTRING) {  // LuaObject::IsString
    BrainBuildUnit(*S(L), factory, lua_tostring(L, 3), static_cast<int>(lua_tonumber(L, 4)));
    static const bool dbg = getenv("MOHO64_DEBUG_AIBUILD") != nullptr;
    if (dbg)
      Logf(LogLevel::Info, "aibuild %u BuildUnit unit %u %s x%d", S(L)->tick(), factory->id, lua_tostring(L, 3),
           static_cast<int>(lua_tonumber(L, 4)));
  }
  return 1;
}

// BuildPlatoon(template, factories, count) 0x58c510: the template's table entries {id, min, max, ...} go to the
// factories in turn (wrapping), floor(max * count) of each. Returns nothing.
int l_BuildPlatoon(lua_State* L) {
  Args(L, "brain:BuildPlatoon()", 4, 4);
  B(L);
  Sim& sim = *S(L);
  if (!lua_istable(L, 2) || !lua_istable(L, 3)) return 0;
  const int n = ObjectCount(L, 2);  // GetCount 0x58c633 / 0x58c640, both read once
  const int nf = ObjectCount(L, 3);
  int f = 1;
  static const bool dbg = getenv("MOHO64_DEBUG_AIBUILD") != nullptr;
  for (int i = 1; i <= n; ++i) {
    if (f > nf) f = 1;
    lua_rawgeti(L, 2, i);
    const int e = lua_gettop(L);
    if (lua_istable(L, e)) {
      lua_rawgeti(L, e, 3);
      const int num = static_cast<int>(lua_tonumber(L, -1));  // LuaObject::GetInteger
      lua_pop(L, 1);
      const float mult = static_cast<float>(lua_tonumber(L, 4));
      const int count = FloorF(static_cast<float>(static_cast<double>(mult) * static_cast<double>(static_cast<float>(num))));
      lua_rawgeti(L, 3, f);
      Unit* factory = CheckObject<Unit>(L, lua_gettop(L));  // SCR_FromLua_Unit
      lua_pop(L, 1);
      if (factory) {
        lua_rawgeti(L, e, 1);
        const char* id = lua_tostring(L, -1);
        BrainBuildUnit(sim, factory, id, count);
        if (dbg) Logf(LogLevel::Info, "aibuild %u BuildPlatoon unit %u %s x%d", sim.tick(), factory->id, id ? id : "?", count);
        lua_pop(L, 1);
      }
      ++f;
    }
    lua_settop(L, e - 1);
  }
  return 0;
}

// Moho::FindAvailableFactory 0x57ac30 (brain, id, suggested): of the suggested factories, or without any of the army's
// FACTORY - MOBILE units, the first not dead, destroy-queued or being built, not busy (Unit::IsBusy), that can build
// the blueprint. A bad id: the warning, and none.
Unit* FindAvailableFactory(Sim& sim, Army* army, const char* id, const std::vector<Unit*>& suggested) {
  const BlueprintInfo* bp = FindUnitBp(sim, id);
  if (!bp) {
    if (id) Logf(LogLevel::Warning, "Passed in a bad unit blueprint name (%s) to FindAvailableFactory!", id);
    return nullptr;
  }
  const std::vector<Unit*> list = suggested.empty() ? ArmyUnitsInNamed(sim, army, "FACTORY", "MOBILE") : suggested;
  for (Unit* u : list) {
    if (!u || Gone(u) || u->beingBuilt || UnitIsBusy(sim, u)) continue;
    if (UnitCanBuild(u, *bp)) return u;
  }
  return nullptr;
}

// CanBuildPlatoon(template [, factories]) 0x58c000: for every table entry of the template a factory that can take it
// (FindAvailableFactory, the same factory may serve several); the list of them, or nil if one entry finds none (or
// the suggested list holds no unit).
int l_CanBuildPlatoon(lua_State* L) {
  Args(L, "brain:CanBuildPlatoon()", 2, 3);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  if (!lua_istable(L, 2)) {
    lua_pushnil(L);
    return 1;
  }
  std::vector<Unit*> suggested;
  if (lua_gettop(L) > 2) {
    if (!lua_istable(L, 3)) luaL_error(L, "Suggested factory list is not a table!");
    // i = 1 .. GetCount (0x58c15c, read again each pass; nothing here changes the list) through func_GetUnitOpt
    // 0x593970: a nil hole or a value that is no game object is a Lua error, a destroyed unit is left out.
    const int n = ObjectCount(L, 3);
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, 3, i);
      Unit* u = OptObject<Unit>(L, -1);
      if (u) suggested.push_back(u);
      lua_pop(L, 1);
    }
    if (suggested.empty()) {
      lua_pushnil(L);
      return 1;
    }
  }
  std::vector<Unit*> found;
  const int n = ObjectCount(L, 2);  // GetCount 0x58c22f, read once; an entry that is no table is skipped
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, 2, i);
    if (lua_istable(L, -1)) {
      lua_rawgeti(L, -1, 1);
      Unit* f = FindAvailableFactory(sim, b->army, lua_tostring(L, -1), suggested);
      lua_pop(L, 1);
      if (!f) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
      }
      found.push_back(f);
    }
    lua_pop(L, 1);
  }
  lua_newtablesized(L, static_cast<int>(found.size()), 0);
  int k = 0;
  for (Unit* u : found) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++k);
  }
  return 1;
}

// GetAvailableFactories([position, radius]) 0x58bd30 -> CAiBrain::GetAvailableFactories 0x57aec0: the army's
// FACTORY - MOBILE units that are alive, not destroy-queued, factories (builder +8), not being built and not busy;
// with a radius above 0, within it (2D, sqrt 0x40eaf0 compared on the x87 at PC_24, so a float). Ascending id.
int l_GetAvailableFactories(lua_State* L) {
  Args(L, "brain:GetAvailableFactories()", 1, 3);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  Vec3 p;
  float r = 0;
  if (lua_gettop(L) > 2) {
    p = VecArg(L, 2);
    r = NumArg(L, 3);
  }
  std::vector<Unit*> out;
  for (Unit* u : ArmyUnitsInNamed(sim, b->army, "FACTORY", "MOBILE")) {
    if (Gone(u) || !IsFactoryBuilder(sim, u) || u->beingBuilt || UnitIsBusy(sim, u)) continue;
    if (r > 0.0f) {
      const float dz = p.z - u->position.z, dx = p.x - u->position.x;
      const float s = dz * dz + dx * dx;
      if (std::sqrt(s) > r) continue;  // 0x57b022..0x57b037
    }
    out.push_back(u);
  }
  lua_newtablesized(L, static_cast<int>(out.size()), 0);  // FillLuaTableWithEntities 0x6934e0
  int k = 0;
  for (Unit* u : out) {
    PushObject(L, u);
    lua_rawseti(L, -2, ++k);
  }
  return 1;
}

// NumCurrentlyBuilding(buildeeCategory, builderCategory) 0x58bac0: the army's units of the builder category, alive and
// not destroy-queued, in state Building (5) or Upgrading (6), whose focus (unit+0x4d0) is a unit of the buildee
// category.
int l_NumCurrentlyBuilding(lua_State* L) {
  Args(L, "brain:NumCurrentlyBuilding( entityCategoryOfBuildee, entityCategoryOfBuilder )", 3, 3);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const uint64_t* buildee = ToCategory(L, 2);
  const uint64_t* builderCat = ToCategory(L, 3);
  int n = 0;
  for (Unit* u : ArmyUnitsIn(b->army, builderCat)) {
    if (Gone(u)) continue;
    if (!u->unitStates.Has(US("Building")) && !u->unitStates.Has(US("Upgrading"))) continue;
    Entity* f = u->focusId ? sim.FindEntity(u->focusId) : nullptr;
    if (!f || f->kind != Entity::Kind::Unit) continue;
    if (InCat(f->blueprint, buildee)) ++n;
  }
  lua_pushnumber(L, static_cast<float>(n));
  return 1;
}

// IsAnyEngineerBuilding(category) 0x58cac0: one of the army's ENGINEER units, not dead, in state Building, whose
// focus entity's blueprint (+0x6c) is in the category.
int l_IsAnyEngineerBuilding(lua_State* L) {
  Args(L, "brain:IsAnyEngineerBuilding(category)", 2, 2);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const uint64_t* cat = ToCategory(L, 2);
  for (Unit* u : ArmyUnitsInNamed(sim, b->army, "ENGINEER")) {
    if (Dead(u) || !u->unitStates.Has(US("Building"))) continue;
    Entity* f = u->focusId ? sim.FindEntity(u->focusId) : nullptr;
    if (f && f->blueprint && InCat(f->blueprint, cat)) {
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}

// CreateResourceBuildingNearest(structureName, posX, posY) 0x589e30: on the deposits of the blueprint's kind, nearest
// the point first (the sorted list as FindPlaceToBuild's), the first where the brain can build (alliance 3) gets the
// structure, finished, at the terrain's elevation (the surface's unless the footprint may stand on the seabed,
// +0xda & 2), unturned, on the land layer. nil if none.
int l_CreateResourceBuildingNearest(lua_State* L) {
  Args(L, "brain:CreateResourceBuildingNearest(structureName, posX, posY)", 4, 4);
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const char* name = StrArg(L, 2);
  const float pz = NumArg(L, 4);
  const float px = NumArg(L, 3);
  const BlueprintInfo* bp = FindUnitBp(sim, name);
  if (!bp || !b->army) {
    lua_pushnil(L);
    return 1;
  }
  for (const Spot& s : DepositSpots(sim, bp, px, pz)) {
    if (!AiBrainCanBuildStructureAt(sim, b, *bp, Vec3{s.x, 0, s.z}, 3)) continue;
    float y = 0;
    if (const TerrainMap* m = sim.map()) {
      y = m->TerrainHeight(s.x, s.z);
      const bool seabed = bp->hasFootprint && (bp->footprint.caps & 2);
      if (!seabed && m->hasWater && m->waterElevation > y) y = m->waterElevation;
    }
    Unit* u = sim.CreateUnit(L, *bp, b->army, Vec3{s.x, y, s.z}, Quat{0, 0, 0, 1}, true, nullptr, true, 1);
    if (u) {
      PushObject(L, u);
      return 1;
    }
  }
  lua_pushnil(L);
  return 1;
}

// CreateUnitNearSpot(unitName, posX, posY) 0x589910: the army's start area (+-5 around its rounded start) is
// occupied for the search (COGrid::ExecuteOccupy 0x721a90 .. ReleaseOccupy 0x721b30), then FindFreeSpot (margin 1,
// the whole map, flags 1, 1: the spot must also be clear of units in its skirt grown by one, Sim::LocationIsFree
// 0x62da50); the unit is created there, finished, unturned. nil if no spot. (The occupied start area is ours as a
// rejected footprint overlap, inferred to equal the occupancy bits' test.)
int l_CreateUnitNearSpot(lua_State* L) {
  Args(L, "brain:CreateUnitNearSpot(unitName, posX, posY)", 4, 4);
  if (lua_isnil(L, 2) || !lua_isstring(L, 2)) luaL_error(L, "Invalid unit name passed in");
  if (lua_type(L, 3) != LUA_TNUMBER) luaL_error(L, "Invalid posX passed in");
  if (lua_type(L, 4) != LUA_TNUMBER) luaL_error(L, "Invalid posY passed in");
  AiBrain* b = B(L);
  Sim& sim = *S(L);
  const char* name = StrArg(L, 2);
  const float pz = NumArg(L, 4);
  const float px = NumArg(L, 3);
  const BlueprintInfo* bp = FindUnitBp(sim, name);
  Army* army = b->army;
  if (!bp || !army) {
    lua_pushnil(L);
    return 1;
  }
  const int ax = static_cast<int>(std::nearbyint(army->startX)), az = static_cast<int>(std::nearbyint(army->startZ));
  const int start[4] = {ax - 5, az - 5, ax + 5, az + 5};
  const NamedFootprint fp = bp->hasFootprint ? bp->footprint : NamedFootprint{"", 1, 1, 1, 0, 0, 0, 0};
  auto fits = [&](float x, float z) {
    if (!LocationIsFree(sim, *bp, x, z)) return false;
    const int fx = static_cast<int>(std::nearbyint(x - fp.sizeX * 0.5f)), fz = static_cast<int>(std::nearbyint(z - fp.sizeZ * 0.5f));
    if (fx < start[2] && start[0] < fx + fp.sizeX && fz < start[3] && start[1] < fz + fp.sizeZ) return false;
    // the skirt grown by 1 to whole cells (floor / ceil, 0x62b2a0..0x62b375), free of units
    float sk[4];
    BlueprintSkirtRect(sim, *bp, x, z, sk);
    const int r[4] = {static_cast<int>(std::floor(sk[0] - 1.0f)), static_cast<int>(std::floor(sk[1] - 1.0f)),
                      static_cast<int>(std::ceil(sk[2] + 1.0f)), static_cast<int>(std::ceil(sk[3] + 1.0f))};
    return RectFreeOfUnits(sim, r);
  };
  float x = px, z = pz;
  bool found = false;
  if (IsWithin(sim, x, z, 1.0f, true)) {
    if (fits(x, z)) {
      found = true;
    } else {
      int tested = 0;
      for (int r = 1; !found; ++r) {
        for (int i = -r; i <= r && !found; ++i) {
          const int jstep = (i == -r || i == r) ? 1 : 2 * r;
          for (int j = -r; j <= r; j += jstep) {
            ++tested;
            const float cx = static_cast<float>(i) + px, cz = static_cast<float>(j) + pz;
            if (fits(cx, cz)) {
              x = cx;
              z = cz;
              found = true;
              break;
            }
          }
        }
        if (!found && tested >= 900) break;
      }
    }
  }
  if (!found) {
    lua_pushnil(L);
    return 1;
  }
  const Vec3 s = SnapStructurePosition(sim, *bp, Vec3{x, 0, z});
  Unit* u = sim.CreateUnit(L, *bp, army, Vec3{s.x, 0, s.z}, Quat{0, 0, 0, 1}, true);
  if (u) PushObject(L, u);
  else lua_pushnil(L);
  return 1;
}

// ---- CPlatoon's transport orders ----

// The units of the platoon's first squad of each class Attack..Scout (classes 1..5, the first squad of a class in
// creation order: 0x728b48), a copy of each (copy_CSquadUnits_into_EntitySet 0x723a50).
template <class F>
void ForFirstSquads(Platoon* p, F&& f) {
  for (int cls = 1; cls < 6; ++cls)
    for (auto& s : p->squads)
      if (s->cls == cls) {
        const std::vector<Unit*> units = s->units;
        f(units);
        break;
      }
}
void IdSetAdd(std::vector<Unit*>& s, Unit* u) {  // EntitySetTemplate_Unit::Add 0x57ddd0 (ascending id, unique)
  auto it = std::lower_bound(s.begin(), s.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  if (it == s.end() || *it != u) s.insert(it, u);
}
bool IdSetHas(const std::vector<Unit*>& s, const Unit* u) {
  auto it = std::lower_bound(s.begin(), s.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  return it != s.end() && *it == u;
}
void PushCommandList(lua_State* L, const std::vector<std::shared_ptr<UnitCommand>>& cmds) {
  lua_newtablesized(L, static_cast<int>(cmds.size()), 0);
  int k = 0;
  for (auto& c : cmds) {
    PushUnitCommand(L, c);
    lua_rawseti(L, -2, ++k);
  }
}

// CPlatoon:LoadUnits(category) 0x730970 -> CPlatoon::LoadUnits 0x728a70: the squads' live, finished units in the
// category that are no TRANSPORTATION are the passengers, their TRANSPORTATION units the transports (ascending id).
// Each transport in turn takes the passengers not yet taken whose slot test passes (TransportAssignSlot with no
// bone: the reservations it makes are dropped again at once, vt+0x18), and they and the transport get one
// TransportLoadUnits order on it (UNIT_IssueCommand without clear). Without passengers or transports: the log line,
// no order. Returns the orders.
int l_PlatoonLoadUnits(lua_State* L) {
  Args(L, "CPlatoon:LoadUnits()", 2, 2);
  Platoon* p = CheckObject<Platoon>(L, 1);
  Sim& sim = *S(L);
  const uint64_t* cat = ToCategory(L, 2);
  std::vector<Unit*> passengers, transports;
  ForFirstSquads(p, [&](const std::vector<Unit*>& units) {
    for (Unit* u : units)
      if (!Dead(u) && !u->beingBuilt && !BpInCategory(sim, u->blueprint, "TRANSPORTATION") && InCat(u->blueprint, cat))
        IdSetAdd(passengers, u);
    for (Unit* u : units)
      if (!Dead(u) && !u->beingBuilt && BpInCategory(sim, u->blueprint, "TRANSPORTATION")) IdSetAdd(transports, u);
  });
  std::vector<std::shared_ptr<UnitCommand>> out;
  if (transports.empty() || passengers.empty()) {
    Logf(LogLevel::Info, "Cannot perform load operation. Attempted to load %d units onto %d transports",
         static_cast<int>(passengers.size()), static_cast<int>(transports.size()));
  } else {
    std::vector<Unit*> assigned;
    for (Unit* t : transports) {
      if (!t->transport) continue;  // unit+0x55c
      std::vector<Unit*> loaded;
      for (Unit* u : passengers) {
        if (IdSetHas(assigned, u)) continue;
        if (!TransportAssignSlotFor(sim, t, u, -1)) continue;
        IdSetAdd(loaded, u);
        IdSetAdd(assigned, u);
      }
      for (Unit* u : loaded) TransportRemoveReservationFor(sim, t, u);
      IdSetAdd(loaded, t);
      StreamIssueData d;
      d.type = static_cast<int>(CommandType::TransportLoadUnits);
      d.target.type = 1;
      d.target.entity = t->id;  // +0x70
      if (auto c = IssueFromStream(sim, loaded, d, false)) out.push_back(c);
    }
  }
  PushCommandList(L, out);
  return 1;
}

// CPlatoon:UnloadUnitsAtLocation(category, position) 0x730b90 -> CPlatoon::UnloadUnitsAtLocation 0x7291c0: the point
// goes to the surface (STIMap::GetElevation, the water's if higher); every live, finished TRANSPORTATION or CARRIER
// unit of the squads that carries units of the category (its attached children, entity vt+0x58) goes with those
// children into one TransportUnloadSpecificUnits order to the point (UNIT_IssueCommand without clear). Returns it.
int l_PlatoonUnloadUnitsAtLocation(lua_State* L) {
  Args(L, "CPlatoon:UnloadUnitsAtLocation()", 3, 3);
  Platoon* p = CheckObject<Platoon>(L, 1);
  Sim& sim = *S(L);
  const uint64_t* cat = ToCategory(L, 2);
  Vec3 pos = VecArg(L, 3);
  if (!(pos.x == pos.x && pos.y == pos.y && pos.z == pos.z))  // Moho::IsValid_Vector3f 0x5657f0 (inferred: no NaN)
    luaL_error(L, "Platoon:UnloadUnitsAtLocation Passed in an invalid target point");
  if (const TerrainMap* m = sim.map()) {
    float y = m->TerrainHeight(pos.x, pos.z);
    if (m->hasWater && m->waterElevation > y) y = m->waterElevation;
    pos.y = y;
  }
  std::vector<Unit*> set;
  ForFirstSquads(p, [&](const std::vector<Unit*>& units) {
    for (Unit* u : units) {
      if (Dead(u) || u->beingBuilt) continue;
      if (!BpInCategory(sim, u->blueprint, "TRANSPORTATION") && !BpInCategory(sim, u->blueprint, "CARRIER")) continue;
      bool any = false;
      const uint32_t ref = EntityRef(u);
      for (Unit* c : sim.units())
        if (c->parentId == ref && InCat(c->blueprint, cat)) {
          IdSetAdd(set, c);
          any = true;
        }
      if (any) IdSetAdd(set, u);
    }
  });
  std::vector<std::shared_ptr<UnitCommand>> out;
  if (!set.empty()) {
    StreamIssueData d;
    d.type = static_cast<int>(CommandType::TransportUnloadSpecificUnits);
    d.target.type = 2;
    d.target.pos = pos;
    if (auto c = IssueFromStream(sim, set, d, false)) out.push_back(c);
  }
  PushCommandList(L, out);
  return 1;
}

// DebugGetSelection() 0x75e320: the units of the sim's debug selection set (sub_758DB0, the entity ids the user's
// interface last sent). We keep no such set: always an empty table.
int l_DebugGetSelection(lua_State* L) {
  if (lua_gettop(L) != 0) luaL_error(L, "%s\n  expected %d args, but got %d", "Get DEBUG info for UI selection", 0, lua_gettop(L));
  lua_newtable(L);
  return 1;
}

}  // namespace

void RegisterAiBuildBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "GetUnitBlueprint", l_GetUnitBlueprint);
  SetMethod(L, "CAiBrain", "GetNumUnitsAroundPoint", l_GetNumUnitsAroundPoint);
  SetMethod(L, "CAiBrain", "FindUnit", l_FindUnit);
  SetMethod(L, "CAiBrain", "FindUpgradeBP", l_FindUpgradeBP);
  SetMethod(L, "CAiBrain", "FindUnitToUpgrade", l_FindUnitToUpgrade);
  SetMethod(L, "CAiBrain", "DecideWhatToBuild", l_DecideWhatToBuild);
  SetMethod(L, "CAiBrain", "FindPlaceToBuild", l_FindPlaceToBuild);
  SetMethod(L, "CAiBrain", "BuildStructure", l_BuildStructure);
  SetMethod(L, "CAiBrain", "BuildUnit", l_BuildUnit);
  SetMethod(L, "CAiBrain", "BuildPlatoon", l_BuildPlatoon);
  SetMethod(L, "CAiBrain", "CanBuildPlatoon", l_CanBuildPlatoon);
  SetMethod(L, "CAiBrain", "GetAvailableFactories", l_GetAvailableFactories);
  SetMethod(L, "CAiBrain", "NumCurrentlyBuilding", l_NumCurrentlyBuilding);
  SetMethod(L, "CAiBrain", "IsAnyEngineerBuilding", l_IsAnyEngineerBuilding);
  SetMethod(L, "CAiBrain", "CreateResourceBuildingNearest", l_CreateResourceBuildingNearest);
  SetMethod(L, "CAiBrain", "CreateUnitNearSpot", l_CreateUnitNearSpot);
  SetGlobal(L, "DebugGetSelection", l_DebugGetSelection);
  SetMethod(L, "CPlatoon", "LoadUnits", l_PlatoonLoadUnits);
  SetMethod(L, "CPlatoon", "UnloadUnitsAtLocation", l_PlatoonUnloadUnitsAtLocation);
}

}  // namespace moho
