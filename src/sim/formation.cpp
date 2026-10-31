// Formations: engine-ref/specs/formations.md (CAiFormationInstance 0x5694b0.., CAiFormationDBImpl 0x59c030..,
// Unit::UpdateInfoCache 0x6a9810).
#include "sim/formation.h"
#include "sim/combat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>

#include "core/dmath.h"
#include "core/log.h"
#include "script/script_state.h"
#include "sim/air.h"
#include "sim/blueprints.h"
#include "sim/collision.h"
#include "sim/commands.h"
#include "sim/entity_grid.h"
#include "sim/landnav.h"
#include "sim/motion.h"
#include "sim/sim.h"
#include "sim/sim_cache.h"
#include "sim/slot_sort.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {
namespace {

constexpr float kInfF = std::numeric_limits<float>::infinity();

Unit* UnitOf(Sim& sim, uint32_t h) {
  Entity* e = h ? sim.FindEntity(h) : nullptr;
  return e && e->kind == Entity::Kind::Unit ? static_cast<Unit*>(e) : nullptr;
}
bool Mobile(const Unit* u) { return u->motion.bp && u->motion.bp->mobile(); }
bool Gone(const Unit* u) { return !u || u->dead || u->destroyQueued; }
// What the rebuild and the slot assignment read of a unit (0x568ca0, 0x567300) is IUnit::GetPosition / GetOrientation
// (vt+0x14 0x6a49b0 = unit+0xb4, vt+0x18 0x6a49c0 = unit+0xa4): the entity's committed transform, which motion only
// replaces in Entity::AdvanceCoords (beat_order.md step 13). A formation built inside the motion loop (a guard
// formation, made in the guarded unit's own MotionTick) sees the units that have had their turn where the beat
// found them, like those still to come.
const Vec3& CommittedPos(const Sim& sim, const Unit* u) {
  return sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
}
const Quat& CommittedOri(const Sim& sim, const Unit* u) {
  return sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastOrientation : u->orientation;
}
// the rebuild's unit filter (0x568ca0): mobile, alive, complete
bool ValidMember(const Unit* u) { return !Gone(u) && Mobile(u) && !u->beingBuilt && !u->parentId; }
bool IsAirUnit(const Unit* u) { return u->motion.bp && u->motion.bp->motionType == kMotionAir; }
int LayerIndex(const Unit* u) { return IsAirUnit(u) ? 1 : 0; }
// Entity::GetFootprint (COORDS_CanMoveAt 0x720fdb): the AltFootprint while the unit uses it
int FpSizeX(const Unit* u) { return u->motion.bp ? std::max<int>(1, UnitFootprint(u).sizeX) : 1; }
int FpSizeZ(const Unit* u) { return u->motion.bp ? std::max<int>(1, UnitFootprint(u).sizeZ) : 1; }
// F+0x320: air (SizeX+SizeZ)/2, others max(SizeX, SizeZ)
int FormFootprint(const Unit* u) {
  if (!u->motion.bp) return 1;
  const NamedFootprint& f = u->motion.bp->footprint;
  return IsAirUnit(u) ? (f.sizeX + f.sizeZ) / 2 : std::max<int>(f.sizeX, f.sizeZ);
}
bool CanFly(const Unit* u) { return u->motion.air != nullptr; }
float TopSpeed(Sim& sim, const Unit* u) {
  if (CanFly(u)) return GetAirBp(sim.L(), *u->blueprint).maxAirspeed;
  return u->motion.bp ? u->motion.bp->maxSpeed : 0.0f;
}
// the formation's orientation (+0x2d4) and its assignment rotation (+0x2e4) against the zero quaternion 0x10c7a58 by
// the 16-byte memcmp 0x4f0b40 (ComputeRunScriptOffset 0x566aa8, RunScript 0x567613, UpdateFormation 0x568f7b, the
// ctor 0x569755): a -0.0 part is not zero
bool QuatZero(const Formation& f) { return vm::BitsZero4(f.qw, f.qx, f.qy, f.qz); }
bool AssignZero(const Formation& f) { return vm::BitsZero4(f.aw, f.ax, f.ay, f.az); }
// MultQuadVec with (w, x, y, z)
// Every rotation of the formation code is MultQuadVec 0x452d40 (formations.md: the layout's 0x452d40 at 0x59b6d9 and the
// assignment's): through the quaternion's matrix (QuatToMatrix 0x452fd0) and its row sums ((0 + x m0) + y m1) + z m2,
// not the quaternion product, which rounds the last bit differently. Seen: MP1 2372, the guard formation slot of army
// 1's interceptor 0x100018 one float step off in x (130.686478 for the original's 130.686493).
Vec3 RotW(float w, float x, float y, float z, Vec3 v) { return vm::RotateM(Quat{x, y, z, w}, v); }
float Wrap(float a) {
  if (a > 3.14159274f) a -= 6.28318548f;
  else if (a < -3.14159274f) a += 6.28318548f;
  return a;
}

// ---- the formation script ------------------------------------------------------------------------

// 0x62ee40: 1 Air (every unit flies), 2 Combo, 0 Surface
int FormationType(const std::vector<Unit*>& units) {
  bool air = false, other = false;
  for (Unit* u : units) (IsAirUnit(u) ? air : other) = true;
  if (air && !other) return 1;
  if (air && other) return 2;
  return 0;
}
const char* kTypeTables[] = {"SurfaceFormations", "AirFormations", "ComboFormations"};

// pushes the formations module (or nothing); returns the stack index or 0
int PushModule(lua_State* L) {
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/formations.lua");
  if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }
  return lua_gettop(L);
}

// GetScriptName 0x575bd0
std::string ScriptName(lua_State* L, int index, const std::vector<Unit*>& units) {
  int top = lua_gettop(L);
  std::string out;
  if (int m = PushModule(L)) {
    lua_pushstring(L, kTypeTables[FormationType(units)]);
    lua_gettable(L, m);
    if (lua_istable(L, -1)) {
      int t = lua_gettop(L);
      int n = luaL_getn(L, t);  // LuaObject::GetN 0x907e50 (luaL_getn), not GetCount
      lua_rawgeti(L, t, index < n ? index + 1 : 1);
      if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return out;
}

struct ScriptSlot {
  float x = 0, z = 0;
  const uint64_t* cats = nullptr;
  float row = 0;
};

// FORMATION_RunScript 0x576690: <name>(units, 0-based); entries {x, z, filter, order, rotate}. The result is walked
// with LuaTableIterator (0x457a40 / Next 0x457ba0: lua_next order, every key); a value that is no table or whose
// GetCount (0x576944) is not 5 is left out with "Error in the formation slot! Either not a table or num params != 5."
bool RunScript(lua_State* L, const std::string& name, const std::vector<Unit*>& units, std::vector<ScriptSlot>* out,
               bool* rotate) {
  int top = lua_gettop(L);
  int m = PushModule(L);
  if (!m) return false;
  lua_pushstring(L, name.c_str());
  lua_gettable(L, m);
  if (!lua_isfunction(L, -1)) {
    Logf(LogLevel::Warning, "Could not find lua create formation function %s.", name.c_str());
    lua_settop(L, top);
    return false;
  }
  lua_newtable(L);
  for (size_t i = 0; i < units.size(); ++i) {
    PushObject(L, units[i]);
    lua_rawseti(L, -2, static_cast<int>(i));
  }
  // table.getn gives the unit count (FAF's scripts loop 0 .. getn-1): the size is set, not a field
  luaL_setn(L, lua_gettop(L), static_cast<int>(units.size()));
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, -3);
  if (lua_pcall(L, 1, 1, -3) != 0) {
    Logf(LogLevel::Warning, "Formation script %s threw an error: %s", name.c_str(),
         lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    lua_settop(L, top);
    return false;
  }
  if (!lua_istable(L, -1)) {
    Logf(LogLevel::Warning, "Formation script did not retuan a table!");
    lua_settop(L, top);
    return false;
  }
  int r = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, r)) {
    int e = lua_gettop(L);
    if (!lua_istable(L, e) || ObjectCount(L, e) != 5) {
      Logf(LogLevel::Warning, "Error in the formation slot! Either not a table or num params != 5.");
      lua_pop(L, 1);
      continue;
    }
    ScriptSlot s;
    lua_rawgeti(L, e, 1);
    s.x = static_cast<float>(lua_tonumber(L, -1));
    lua_rawgeti(L, e, 2);
    s.z = static_cast<float>(lua_tonumber(L, -1));
    lua_rawgeti(L, e, 3);
    s.cats = ToCategory(L, -1);
    lua_rawgeti(L, e, 4);
    s.row = static_cast<float>(lua_tonumber(L, -1));
    lua_rawgeti(L, e, 5);
    *rotate = lua_toboolean(L, -1) != 0;  // one result-wide byte: the last entry decides
    out->push_back(s);
    lua_settop(L, r + 1);  // the key, for lua_next
  }
  lua_settop(L, top);
  return true;
}

// ---- rebuild and slot assignment (0x568ca0, 0x567300; spec 4.0, 8) -------------------------------

void AssignGroup(Formation& F, const std::vector<Unit*>& units, int k) {
  Sim& sim = *F.sim;
  lua_State* L = sim.L();
  std::vector<ScriptSlot> slots;
  bool rotate = false;
  if (!RunScript(L, F.script, units, &slots, &rotate) || slots.empty()) return;
  FormGroup G;
  // unit side (8.2): the sums in single precision, times 1 / count (0x567480..0x5674fd; no units: FLT_MAX)
  float sx = 0, sz = 0;
  for (Unit* u : units) {
    sx = CommittedPos(sim, u).x + sx;
    sz = CommittedPos(sim, u).z + sz;
  }
  const float invU = 1.0f / static_cast<float>(units.size());
  G.cx = invU * sx;
  G.cz = sz * invU;
  struct URec {
    Unit* u;
    float rx, rz;
  };
  std::vector<URec> U;
  for (Unit* u : units) {
    const Vec3& p = CommittedPos(sim, u);
    Vec3 rel{p.x - G.cx, 0, p.z - G.cz};
    if (rotate && !AssignZero(F)) rel = RotW(F.aw, F.ax, F.ay, F.az, rel);
    U.push_back({u, rel.x, rel.z});
  }
  // slot side (8.3)
  struct SRec {
    float ox, oz, key, row;
    const uint64_t* cats;
  };
  std::vector<SRec> S;
  float minX = kInfF, maxX = -kInfF, minZ = kInfF, maxZ = -kInfF;
  float mx = 0, mz = 0;  // single precision too (0x5678a6..0x5678cb), times 1 / count (0x56796d..0x567982)
  const float unit = static_cast<float>(F.largest + 2);
  for (const ScriptSlot& s : slots) {
    Vec3 v{s.x * F.scale, 0 * F.scale, s.z * F.scale};
    if (!QuatZero(F)) v = RotW(F.qw, F.qx, F.qy, F.qz, v);
    SRec r{v.x * unit, v.z * unit, 0, s.row, s.cats};
    mx += r.ox;
    mz += r.oz;
    minX = std::min(minX, r.ox);
    maxX = std::max(maxX, r.ox);
    minZ = std::min(minZ, r.oz);
    maxZ = std::max(maxZ, r.oz);
    S.push_back(r);
  }
  const float invS = 1.0f / static_cast<float>(S.size());
  float mOffX = invS * mx, mOffZ = mz * invS;
  G.extX = std::max(0.0f, maxX - minX);
  G.extZ = std::max(0.0f, maxZ - minZ);
  float E = std::max(G.extX, G.extZ);
  for (size_t i = 0; i < S.size(); ++i) {
    float dx = slots[i].x - slots[0].x, dz = slots[i].z - (slots[0].z + E);
    S[i].key = dx * dx + dz * dz;
  }
  // 8.4: the original's sort 0x572350 (sim/slot_sort.h), the larger key first. For up to 32 slots it is stable;
  // above that equal keys can move. Seen: MP3 4424, a Move for 35 of army 1's units in AttackFormation: two slots of
  // one key in the stable order gave tanks 0x100073 and 0x100071 each other's slot (the probe mp3_thr: the original's
  // goals at 4425 are (302, 278) for 0x100071 and (282, 264) for 0x100073), and each drove to the other's cell.
  slotsort::Sort(S, [](const SRec& a, const SRec& b) { return a.key > b.key; });
  // 8.5: matching
  int order = 1;
  G.speed = kInfF;
  for (const SRec& s : S) {
    if (U.empty()) break;
    // the slot's point (cU + off) - mOff (0x567c98..0x567cbc: addss, then subss)
    float tx = (G.cx + s.ox) - mOffX, tz = (G.cz + s.oz) - mOffZ;
    int best = -1;
    float bestD = kInfF;
    for (size_t j = 0; j < U.size(); ++j) {
      Unit* u = U[j].u;
      if (s.cats && !(u->blueprint && u->blueprint->entityIndex >= 0 && CategoryHas(s.cats, u->blueprint->entityIndex)))
        continue;
      float ddx = tx - (G.cx + U[j].rx), ddz = tz - (G.cz + U[j].rz);
      float d = std::sqrt(ddx * ddx + ddz * ddz);
      if (d < bestD) {
        bestD = d;
        best = static_cast<int>(j);
      }
    }
    if (best < 0) continue;
    Unit* u = U[static_cast<size_t>(best)].u;
    FormNode n;
    n.ref = EntityRef(u);
    n.order = order++;
    n.offX = s.ox;
    n.offZ = s.oz;
    n.row = s.row;
    n.cats = s.cats;
    G.nodes[u->id] = n;
    // 3.1: group speed
    float v = TopSpeed(sim, u) * 0.85f;
    if (v <= G.speed) G.speed = v;
    U.erase(U.begin() + best);
  }
  for (const URec& r : U)
    Logf(LogLevel::Warning, "Failed to assaign unit %u a slot in the formation %s (units=%zu, formation slots=%zu)",
         r.u->id, F.script.c_str(), units.size(), slots.size());
  G.speedSet = true;
  F.groups[k].push_back(std::move(G));
}

bool BoxesOverlap(const FormGroup& a, const FormGroup& b) {  // (inferred: 0x5688c0)
  return std::fabs(a.cx - b.cx) <= (a.extX + b.extX) * 0.5f && std::fabs(a.cz - b.cz) <= (a.extZ + b.extZ) * 0.5f;
}

void Rebuild(Formation& F) {
  Sim& sim = *F.sim;
  // 0x5691e0: purge
  F.units.erase(std::remove_if(F.units.begin(), F.units.end(), [&](uint32_t h) { return Gone(UnitOf(sim, h)); }),
                F.units.end());
  F.groups[0].clear();
  F.groups[1].clear();
  F.adjusted.clear();
  F.reservations.clear();
  std::vector<Unit*> valid;
  double fx = 0, fz = 0;
  F.largest = 0;
  for (uint32_t h : F.units) {
    Unit* u = UnitOf(sim, h);
    if (!ValidMember(u)) continue;
    valid.push_back(u);
    Vec3 f = vm::Forward(CommittedOri(sim, u));
    fx += f.x;
    fz += f.z;
    F.largest = std::max(F.largest, FormFootprint(u));
  }
  F.aw = F.ax = F.ay = F.az = 0;
  if (!QuatZero(F)) {
    float hF = dmath::Atan2(2 * (F.qz * F.qx + F.qw * F.qy), 1 - 2 * (F.qx * F.qx + F.qy * F.qy));
    float hU = dmath::Atan2(static_cast<float>(fx), static_cast<float>(fz));
    float d = Wrap(hF - hU);
    if (std::fabs(d) < 1.8849558f) {
      F.aw = dmath::Cos(d * 0.5f);
      F.ay = dmath::Sin(d * 0.5f);
    }
  }
  for (int k = 0; k < 2; ++k) {
    std::vector<Unit*> list;
    for (Unit* u : valid)
      if (LayerIndex(u) == k) list.push_back(u);
    if (!list.empty()) AssignGroup(F, list, k);
  }
  // 0x568980: air escorts of a land formation travel at the land speed
  if (FormationIsForm(F) && F.type != 15)
    for (FormGroup& a : F.groups[1])
      for (FormGroup& l : F.groups[0])
        if (BoxesOverlap(a, l)) {
          float s = std::min(a.speed, l.speed);
          a.speed = l.speed = s;
          a.cx = l.cx;
          a.cz = l.cz;
          float hx = std::max(10.0f, std::max(a.extX, l.extX)), hz = std::max(10.0f, std::max(a.extZ, l.extZ));
          a.extX = l.extX = hx;
          a.extZ = l.extZ = hz;
          break;
        }
  F.dirty = false;
  ++F.rebuilds;
}

// The rebuild of a changed formation (its flag F+0x31c, set by AddUnit 0x56a2d8, SetScale 0x56a50f and
// SetOrientation 0x56a637 / 0x56a673): only the ctor 0x5694b0 and the three slot queries that test the flag first
// rebuild, vt+0x18 0x569cb0, vt+0x20 0x569f70 and Update vt+0x44 0x59ae80 (the formation DB at the beat's step 11);
// vt+0x1c 0x569ea0 through vt+0x18 (0x569eda). The group lookup vt+0x14 0x5669a0, Contains vt+0x40 0x56a440, the
// leader, the speed and the rest read the groups as they are. Nothing in a unit's turn (Unit::MotionTick 0x6a9010:
// UpdateInfoCache 0x6a9810 calls vt+0x4c, +0x40, +0x14, +0x2c, +0x34, +0x24, +0x28, +0x50, +0x30) asks a rebuilding
// one, so a formation changed in the command stage is rebuilt at step 11 at the latest, not in the entity pass.
void EnsureBuilt(Formation& F) {
  if (F.dirty) Rebuild(F);
}

FormGroup* GroupOf(Formation& F, const Unit* u) {
  for (FormGroup& g : F.groups[LayerIndex(u)])
    if (g.nodes.count(u->id)) return &g;
  return nullptr;
}

bool InRawList(const Formation& F, const Unit* u) {
  uint32_t h = EntityRef(u);
  return std::find(F.units.begin(), F.units.end(), h) != F.units.end();
}

// vt+0x40 Contains(u, includeRaw)
bool Contains(Formation& F, const Unit* u, bool raw) {
  if (!u) return false;
  if (GroupOf(F, u)) return true;
  return raw && InRawList(F, u);
}

// 0x59a300: the cached leader, else the member with the largest order
Unit* GroupLeader(Formation& F, FormGroup& g) {
  Sim& sim = *F.sim;
  if (Unit* c = UnitOf(sim, g.leaderCache)) return c;
  const FormNode* best = nullptr;
  for (auto& [id, n] : g.nodes)
    if (!best || n.order > best->order) best = &n;
  if (!best) return nullptr;
  g.leaderCache = best->ref;
  return UnitOf(sim, best->ref);
}

// vt+0x34 GetLeader(u, group)
Unit* GetLeader(Formation& F, Unit* u, FormGroup* g) {
  Sim& sim = *F.sim;
  if (F.type == 15) return u->guardedId ? UnitOf(sim, u->guardedId) : nullptr;
  if (!g || Gone(u)) return nullptr;
  if (LayerIndex(u) == 1)
    for (FormGroup& l : F.groups[0])
      if (BoxesOverlap(*g, l)) return GroupLeader(F, l);
  return GroupLeader(F, *g);
}

// vt+0x20 GetRawPosition (rebuilds a changed formation first, 0x569f7d..0x569f9f)
void RawPosition(Formation& F, Unit* u, FormGroup* g, float* x, float* z) {
  if (F.dirty) {
    Rebuild(F);
    g = nullptr;  // (the groups are new)
  }
  *x = u->position.x;
  *z = u->position.z;
  if (!g) g = GroupOf(F, u);
  if (!g || !Mobile(u)) return;
  auto it = g->nodes.find(u->id);
  if (it == g->nodes.end()) return;
  *x = F.cx + it->second.offX;
  *z = F.cz + it->second.offZ;
}

// The test 0x721231..0x72124f: the unit's GetVelocity (entity vt+0x3c, Unit::GetVelocity 0x6a9c90: the motion's
// vector m+0x38 off the Air layer) against (0, 0, 0) with 0x4f0a50, a memcmp of its 12 bytes: a -0.0 part counts as
// moving. A ship turning on the spot has such a part (the motion writes -0.0 where its step is a negative zero).
bool VelocityBitsSet(const Unit* e) {
  const Vec3& v = UnitVelocity(e);
  uint32_t b[3];
  std::memcpy(&b[0], &v.x, 4);
  std::memcpy(&b[1], &v.y, 4);
  std::memcpy(&b[2], &v.z, 4);
  return (b[0] | b[1] | b[2]) != 0;
}

// COORDS_CanMoveAt with strict 0 (air_extra 3.6): no standing, completed, non-air unit of another formation
bool CanMoveAt(Sim& sim, Unit* u, int cx, int cz) {
  // the box is a square of the footprint's larger side (S = max(SizeX, SizeZ)), y from -1000 to 1000
  const float S = static_cast<float>(std::max(FpSizeX(u), FpSizeZ(u)));
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  float x1 = x0 + S, z1 = z0 + S;
  bool blocked = false;
  const bool selfTransport = u->transport != nullptr;
  // (candidates come by their centre: the margin has to cover half the longest hull, a battleship lying askew
  // reaches 15 and more from its middle)
  static const long dbgSpot = getenv("MOHO64_DEBUG_FORMSPOT") ? atol(getenv("MOHO64_DEBUG_FORMSPOT")) : -1;
  sim.ForUnitsInRect(x0 - 32, z0 - 32, x1 + 32, z1 + 32, [&](Unit* e) {
    if (dbgSpot >= 0 && static_cast<long>(u->id) == dbgSpot && e != u) {  // every unit within 2 of the box
      WorldShape ds;
      if (GetWorldShape(e, &ds)) {
        Vec3 mn, mx;
        ShapeBounds(ds, &mn, &mx);
        if (!(mx.x < x0 - 2 || mn.x > x1 + 2 || mx.z < z0 - 2 || mn.z > z1 + 2))
          Logf(LogLevel::Info,
               "formspot   box (%g,%g)-(%g,%g) near unit %u %s layer %s box x %.4f..%.4f z %.4f..%.4f dead %d built %d moving %d "
               "attached %d transport %d cells x %d+%d z %d+%d",
               x0, z0, x1, z1, e->id, e->blueprint ? e->blueprint->id.c_str() : "?", e->layer.c_str(), mn.x, mx.x, mn.z, mx.z,
               static_cast<int>(Gone(e)), static_cast<int>(!e->beingBuilt), static_cast<int>(VelocityBitsSet(e)),
               static_cast<int>(e->unitStates.Has(US("Attached"))), static_cast<int>(e->transport != nullptr), e->gridX0,
               e->gridW, e->gridZ0, e->gridH);
      }
    }
    if (blocked || e == u || Gone(e) || e->beingBuilt || e->layer == "Air") return;
    if (u->layer == "Sub" && e->layer != "Sub") return;
    const bool sameForm =
        e->form && e->form == u->form && !e->unitStates.Has(US("Attacking")) && !u->unitStates.Has(US("Attacking"));
    if (e->transport && !BpInCategory(sim, e->blueprint, "PODSTAGINGPLATFORM")) {
      // (0x720f70: a unit with a transport object, a carrier too, is in the way whether it moves or not)
      if (sameForm) return;
    } else {
      // its velocity, all three parts (memcmp of 12 bytes with zero): a submarine diving on the spot moves, and so
      // does a ship turning on the spot whose velocity holds a -0.0 (VelocityBitsSet). Seen: MP1 6512 / 6513, army
      // 1's frigate 0x10009a (uas0103) turns where it stands, velocity (-0.0, 0, 0) / (0, 0, -0.0); army 2's frigate
      // 0x200011 is given formation spots of the player's Move next to it: the original takes the cell (530,167)
      // (probe mp1_6527: its SetGoal 0x5a3ed0 gets (530,167) twice), ours took 0x10009a for a standing unit in the
      // way and went to (530,169) and (530,168); from 6527 its path ran elsewhere. (Before: the position against the
      // one before the last step, which a -0.0 does not tell.)
      const bool moving = VelocityBitsSet(e);
      if (selfTransport && (moving || u->unitStates.Has(US("TransportLoading")))) return;
      if (moving) return;
      if (sameForm) return;
    }
    if (e->unitStates.Has(US("Attached"))) return;
    // GatherUnmarkedUnitsInBox 0x721c00: the unit's collision AABB (Entity+0x240, the axis-aligned bounds of its
    // collision shape as it stands: a turned or tilted hull has a wider one) touches the box, touching counts
    WorldShape ws;
    if (GetWorldShape(e, &ws)) {
      Vec3 mn, mx;
      ShapeBounds(ws, &mn, &mx);
      if (mx.x < x0 || mn.x > x1 || mx.z < z0 || mn.z > z1) return;
      // (and before that the entity grid's cells, the unit's as the grid has them: skirt-wide once it has moved,
      // so a submerged sub half a unit wide that ends on a multiple of 4 is still found by the box beside it)
      if (!GridFindsInBox(sim, e, x0, z0, x1, z1)) return;
    } else {
      const MotionBlueprint* b = e->motion.bp;
      float hx = b ? b->sizeX * 0.5f : 0.5f, hz = b ? b->sizeZ * 0.5f : 0.5f;
      if (e->position.x + hx < x0 || e->position.x - hx > x1 || e->position.z + hz < z0 || e->position.z - hz > z1) return;
    }
    blocked = true;
  });
  return !blocked;
}

// STIMap::IsWithin as FindSlotFor 0x59aa20 asks it (0x59ab70 / 0x59ad48): the point (x, 0, z), margin = the
// footprint's larger side, the unit's army's whole-map flag (vt+0xb8 at 0x59aaea)
bool IsWithinMap(Sim& sim, const Unit* u, float x, float z, float size) {
  if (!sim.map()) return true;
  return sim.IsWithin(x, z, size, Sim::UseWholeMap(u->army));
}

// vt+0x68 IsFree
bool IsFree(const Formation& F, float x, float z, int size, int layer) {
  for (const auto& r : F.reservations) {
    if (r.layer != layer) continue;
    int s = std::max(r.size, size);
    if (static_cast<float>(s) > std::fabs(x - r.x) && static_cast<float>(s) > std::fabs(z - r.z)) return false;
  }
  return true;
}

// fistp into the int16 cell GetFormationCell 0x569ea0 stores (0x569f21 / 0x569f48, then the word stores at 0x569f4c /
// 0x569f51): a NaN place (a formation centred on a gone target) gives the indefinite 0x80000000, whose low word is 0,
// so cell 0. (Converting the NaN in C++ is undefined; with this, the cell's world centre is (SizeX/2, SizeZ/2), as the
// original's 0x50b0f0 makes of cell (0, 0).)
int FormCellCoord(float v) { return std::isnan(v) ? 0 : static_cast<int>(std::nearbyint(v)); }
int CellOfX(const Unit* u, float x) { return FormCellCoord(x - FpSizeX(u) * 0.5f); }
int CellOfZ(const Unit* u, float z) { return FormCellCoord(z - FpSizeZ(u) * 0.5f); }

// vt+0x64 AdjustPosition (7.1)
void AdjustPosition(Formation& F, Unit* u, float* px, float* pz) {
  Sim& sim = *F.sim;
  if (Gone(u) || F.type == 15 || F.scale < 1.0f || LayerIndex(u) == 1) return;
  int size = std::max(FpSizeX(u), FpSizeZ(u));
  int layer = LayerIndex(u);
  // MOHO64_DEBUG_FORMSPOT=<unit id>: every spot asked for that unit and which test refused it
  static const long dbgSpot = getenv("MOHO64_DEBUG_FORMSPOT") ? atol(getenv("MOHO64_DEBUG_FORMSPOT")) : -1;
  auto spot = [&](float x, float z) {
    int cx = CellOfX(u, x), cz = CellOfZ(u, z);
    if (dbgSpot >= 0 && static_cast<long>(u->id) == dbgSpot)
      Logf(LogLevel::Info, "formspot %u unit %u at %.3f %.3f cell (%d,%d): fits %d can move %d within %d free %d (%zu reserved)",
           sim.tick(), u->id, x, z, cx, cz, LandCellFits(sim, u, cx, cz) ? 1 : 0, CanMoveAt(sim, u, cx, cz) ? 1 : 0,
           IsWithinMap(sim, u, x, z, static_cast<float>(size)) ? 1 : 0, IsFree(F, x, z, size, layer) ? 1 : 0,
           F.reservations.size());
    return LandCellFits(sim, u, cx, cz) && CanMoveAt(sim, u, cx, cz) && IsWithinMap(sim, u, x, z, static_cast<float>(size)) &&
           IsFree(F, x, z, size, layer);
  };
  float x = *px, z = *pz;
  // A NaN place (a formation centred on a gone target, GenerateFormation) fits nowhere: the original's first test
  // takes the cell fistp(NaN) = 0x80000000, outside the map, and so does every ring cell (NaN + d). Same outcome
  // without converting a NaN to an int: no spot, then the fallback below.
  const bool nan = std::isnan(x) || std::isnan(z);
  if (!nan && spot(x, z)) {
    F.reservations.push_back({x, z, size, layer});
    return;
  }
  int count = 0;
  for (int r = 1; !nan; ++r) {
    for (int dx = -r; dx <= r; ++dx) {
      int step = (dx == -r || dx == r) ? 1 : 2 * r;
      for (int dz = -r; dz <= r; dz += step) {
        ++count;
        float cx = x + dx, cz = z + dz;
        if (spot(cx, cz)) {
          F.reservations.push_back({cx, cz, size, layer});
          *px = cx;
          *pz = cz;
          return;
        }
      }
    }
    if (count >= 2000) break;
  }
  // No spot found (0x59adc0..0x59ade0): the order AFTER the current one decides, CUnitCommandQueue::GetNextCommand
  // 0x6edc20 (the queue's second command, null with fewer than two). One of Move, Attack, Patrol, FormMove, FormAttack,
  // FormPatrol, Guard (0x552c10) keeps the formation position as it is (not reserved); with none the unit is given
  // its own position. Seen (probe mp1_4987): MP1 4986, army 7's 13-unit Move from the stream: the Thaams 0x700053 /
  // 0x70005f, each holding only that Move, have slots in the water (395,493) / (395,490) where the rings find no
  // spot; the original's SetGoal gets their own cells (482,515) / (483,512), so they stand and search nothing. Ours
  // read the head command (the Move), kept the water cells and searched them with the unreachable cap
  // (ProblemGettingToGoal at 4987), and 0x700063's search waited a beat behind them.
  if (u->commands.size() >= 2) {
    CommandType t = u->commands[1]->type;
    if (t == CommandType::Move || t == CommandType::Attack || t == CommandType::Patrol || t == CommandType::FormMove ||
        t == CommandType::FormAttack || t == CommandType::FormPatrol || t == CommandType::Guard)
      return;
  }
  *px = u->position.x;
  *pz = u->position.z;
}

// vt+0x18 GetAdjustedPosition (cached per unit until the next rebuild; rebuilds a changed formation first,
// 0x569cbe..0x569cdf)
void AdjustedPosition(Formation& F, Unit* u, FormGroup* g, float* x, float* z) {
  if (F.dirty) {
    Rebuild(F);
    g = nullptr;  // (the groups are new)
  }
  auto it = F.adjusted.find(u->id);
  if (it != F.adjusted.end()) {
    *x = it->second.first;
    *z = it->second.second;
    return;
  }
  RawPosition(F, u, g, x, z);
  AdjustPosition(F, u, x, z);
  F.adjusted[u->id] = {*x, *z};
}

// vt+0x1c GetFormationCell, as the cell's world centre
Vec3 CellCentre(Sim& sim, Unit* u, float x, float z) {
  float wx = CellOfX(u, x) + FpSizeX(u) * 0.5f, wz = CellOfZ(u, z) + FpSizeZ(u) * 0.5f;
  const TerrainMap* map = sim.map();
  return {wx, map ? map->TerrainHeight(wx, wz) : 0.0f, wz};
}
Vec3 FormationCell(Formation& F, Unit* u, FormGroup* g) {
  float x, z;
  AdjustedPosition(F, u, g, &x, &z);
  return CellCentre(*F.sim, u, x, z);
}

// 0x59a970: the group's travel leader
Unit* TravelLeader(Formation& F, int k, FormGroup& g) {
  Unit* L = nullptr;
  if (k == 1) {
    for (FormGroup& l : F.groups[0])
      if (BoxesOverlap(g, l)) {
        L = GroupLeader(F, l);
        break;
      }
  }
  if (!L) L = GroupLeader(F, g);
  if (L && F.type == 15) L = L->guardedId ? UnitOf(*F.sim, L->guardedId) : nullptr;
  return L;
}

float Dist2D(float ax, float az, float bx, float bz) {
  float dx = ax - bx, dz = az - bz;
  return std::sqrt(dx * dx + dz * dz);
}

bool NavIgnoring(const Unit* u) { return u->navIgnoreFormation || (u->navigator && u->navigator->ignoreFormation); }

// 4.3 at-goal check
void AtGoalCheck(Formation& F, FormGroup& g) {
  Sim& sim = *F.sim;
  Unit* GL = GroupLeader(F, g);
  if (!GL) return;
  bool st = GL->motion.passThrough;
  float R = std::max(static_cast<float>(F.largest) * 2.0f, g.speed * (st ? 0.67f : 0.25f));
  static const bool dbg = getenv("MOHO64_DEBUG_ATGOAL") != nullptr;
  if (dbg) {
    std::string s;
    for (auto& [id, n] : g.nodes) {
      Unit* m = UnitOf(sim, n.ref);
      if (!m) continue;
      float px, pz;
      AdjustedPosition(F, m, &g, &px, &pz);
      Vec3 f = vm::Forward(m->orientation);
      char b[160];
      snprintf(b, sizeof b, " %u:d%.2f,dot%.3f", id, Dist2D(m->position.x, m->position.z, px, pz),
               f.x * F.fwd.x + f.y * F.fwd.y + f.z * F.fwd.z);
      s += b;
    }
    Logf(LogLevel::Info, "atgoal %u %s R %.2f st %d GL %u:%s", sim.tick(), F.script.c_str(), R, st ? 1 : 0, GL->id, s.c_str());
  }
  for (auto& [id, n] : g.nodes) {
    Unit* m = UnitOf(sim, n.ref);
    if (!m) continue;
    if (m->unitStates.Has(US("Refueling"))) return;
    if (st || (FormationIsForm(F) && IsAirUnit(m))) {
      if (m != GL) continue;
    } else if (NavIgnoring(m)) {
      continue;
    }
    float px, pz;
    AdjustedPosition(F, m, &g, &px, &pz);
    if (Dist2D(m->position.x, m->position.z, px, pz) > R) return;
    bool nextMove = false;
    if (m->commands.size() >= 2) {
      CommandType t = m->commands[1]->type;
      nextMove = t == CommandType::Move || t == CommandType::Attack || t == CommandType::Patrol ||
                 t == CommandType::FormMove || t == CommandType::FormAttack || t == CommandType::FormPatrol ||
                 t == CommandType::Guard;
    }
    if (!nextMove && !vm::BitsZero(F.fwd)) {  // (0x59bbf1..0x59bc05: +0x2c8 against zero, memcmp 0x4f0a50)
      Vec3 f = vm::Forward(m->orientation);
      if (f.x * F.fwd.x + f.y * F.fwd.y + f.z * F.fwd.z < 0.95f) return;
    }
  }
  g.atGoal = true;
}

// vt+0x44 Update (4.1)
void Update(Formation& F) {
  Sim& sim = *F.sim;
  EnsureBuilt(F);
  if (!FormationIsForm(F) || F.units.empty()) return;
  for (int k = 0; k < 2; ++k)
    for (FormGroup& g : F.groups[k]) {
      Unit* L = TravelLeader(F, k, g);
      if (!L) continue;
      g.mid = 0;
      if (g.nodes.empty()) continue;
      float minD = F.type == 15 ? 0.0f : kInfF, maxD = 0;
      // the leader's look-ahead
      Vec3 T = L->position;
      if (F.type != 15) {
        if (IsAirUnit(L)) {
          T = FormationCell(F, L, &g);
        } else if (LandNavStatus(L) == 2) {
          Vec3 t;
          if (LandNavTarget(L, &t)) T = t;
        }
      }
      float lax = T.x - L->position.x, laz = T.z - L->position.z;
      float ll = std::sqrt(laz * laz + lax * lax);
      if (ll > 0) {
        // 0x59b0c6..0x59b11b: normalised by the reciprocal (1 / |la|, then times it), then scaled by min(|la|, 20); not
        // by the one factor min / |la|. For a look-ahead under 20 the two differ in the last bit (the length's sum is
        // z*z + x*x). Seen in MP1: with this the air units' first difference moves from 2731 to 2923.
        const float m = std::min(20.0f, ll);
        const float inv = 1.0f / ll;
        lax = m * (lax * inv);
        laz = m * (laz * inv);
      }
      // how far the leader is from its own slot cell
      Vec3 lc = FormationCell(F, L, &g);
      int dcx = CellOfX(L, lc.x) - CellOfX(L, L->position.x), dcz = CellOfZ(L, lc.z) - CellOfZ(L, L->position.z);
      bool formed = std::sqrt(static_cast<float>(dcx * dcx + dcz * dcz)) < 5.0f && F.type != 15;
      // reference point: the members' formation CELLS (GetFormationCell vt+0x1c gives the int16 cell, 0x59b2a7;
      // each converted and added in single precision, 0x59b2a9..0x59b2cd), times 1 / count (0x59b328..0x59b34b):
      // the cells' corners, not their centres. Seen: MP2 1768, army 1's tank 0x10001a guarding engineer 0x100019 (a
      // one-unit GuardFormation): the original's slot is 0.5 further on both axes than ours was.
      float maxR = 0.001f;
      float scx = 0, scz = 0;
      int cnt = 0;
      for (auto& [id, n] : g.nodes) {
        Unit* m = UnitOf(sim, n.ref);
        if (!m) continue;
        maxR = std::max(maxR, Dist2D(m->position.x, m->position.z, L->position.x, L->position.z));
        float ax, az;
        AdjustedPosition(F, m, &g, &ax, &az);
        scx = static_cast<float>(CellOfX(m, ax)) + scx;
        scz = static_cast<float>(CellOfZ(m, az)) + scz;
        ++cnt;
      }
      float Cx, Cz;
      if (F.type == 15) {
        const float inv = cnt ? 1.0f / static_cast<float>(cnt) : 0.0f;
        Cx = cnt ? inv * scx : L->position.x;
        Cz = cnt ? inv * scz : L->position.z;
      } else {
        RawPosition(F, L, &g, &Cx, &Cz);
      }
      for (auto& [id, n] : g.nodes) {
        Unit* m = UnitOf(sim, n.ref);
        if (!m) continue;
        Vec3 p;
        if (NavIgnoring(m)) {
          float ax, az;
          AdjustedPosition(F, m, &g, &ax, &az);
          const TerrainMap* map = sim.map();
          p = {ax, map ? map->TerrainHeight(ax, az) : 0.0f, az};
        } else if (formed) {
          p = FormationCell(F, m, &g);
          n.pos = p;
          n.posSet = true;
          n.angleSet = false;
        } else {
          float rx, rz;
          RawPosition(F, m, &g, &rx, &rz);
          Vec3 o{rx - Cx, 0, rz - Cz};
          float a = 0;
          if (n.angleSet) {
            const Quat& q = L->orientation;
            float hL = dmath::Atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
            // 0x59b5e3..0x59b5ef: the formation's heading (fpatan 0x4e9df0) stays on the x87 stack and is taken from the
            // leader's heading, which was stored to a float, in extended precision; only the difference is rounded to a
            // float. Seen: MP1 2372, the guard formation of army 1's transport 0x100025: ours rounded the formation's
            // heading first, the smoothed angle drifted, and the slot of interceptor 0x100018 came out one float step
            // off in x (130.686478 for the original's 130.686493); its desired velocity then differed in the last bits
            // until 2627, where an exact 15.0 against ours's 14.999999 flipped the air damping factor
            // (CalcAirMovementDampingFactor 0x6bca10: KMove at full length, max speed / length below it).
            const double hFx = std::atan2(static_cast<double>(F.fwd.x), static_cast<double>(F.fwd.z));
            float diff = Wrap(static_cast<float>(static_cast<double>(hL) - hFx));
            float t = Dist2D(m->position.x, m->position.z, L->position.x, L->position.z) / maxR * 0.05f + 0.94f;
            a = (1 - t) * diff + n.angle * t;
          }
          Vec3 o2 = RotW(dmath::Cos(a * 0.5f), 0, dmath::Sin(a * 0.5f), 0, o);
          p = {L->position.x + o2.x + lax, L->position.y + o2.y, L->position.z + o2.z + laz};
          if (!n.posSet) {
            n.pos = p;
            n.posSet = true;
          } else {
            n.pos = {n.pos.x * 0.9f + p.x * 0.1f, n.pos.y * 0.9f + p.y * 0.1f, n.pos.z * 0.9f + p.z * 0.1f};
          }
          n.angle = a;
          n.angleSet = true;
        }
        n.d = Dist2D(m->position.x, m->position.z, p.x, p.z);
        if (!(n.d > minD)) minD = n.d;  // (0x59b8fe..0x59b90d: fcompi / ja: a NaN distance is taken)
        maxD = std::max(maxD, n.d);
        float rx, rz;
        RawPosition(F, m, &g, &rx, &rz);
        n.rank = Dist2D(Cx, Cz, rx, rz);
      }
      g.mid = (maxD + minD) * 0.5f;
      AtGoalCheck(F, g);
    }
}

// vt+0x30 GetFormationSpeed (3.2)
float FormationSpeed(Formation& F, Unit* u, float* mult, FormGroup* g) {
  if (!FormationIsForm(F)) return 0;
  Unit* L = GetLeader(F, u, g);
  if (L && !Contains(F, L, false) && !Mobile(L)) return 0;
  if (NavIgnoring(u)) return 0;
  if (!g) return 0;
  *mult = 0.85f;
  bool follow = CanFly(u) ? true : (LandNavFollowingSlot(u) || L == u);
  if (L == u) follow = true;
  if (g->mid > 0 && follow) {
    auto it = g->nodes.find(u->id);
    if (it != g->nodes.end()) {
      float k = CanFly(u) ? 1.5f : 4.0f;
      float x = (it->second.d - g->mid) * k;
      x = std::min(x, 20.0f);
      x = std::max(x, -5.0f);
      *mult = x * 0.1f + 1.0f;
    }
  }
  return g->speed;
}

}  // namespace

// The sim's formations, in creation order (Sim::formationState). Multicore step 1: these were in a process-wide
// map by the sim's address (RegistryMap).
struct FormationState {
  std::vector<std::weak_ptr<Formation>> registry;
};

namespace {

std::vector<std::weak_ptr<Formation>>& Registry(Sim& sim) {
  if (!sim.formationState) sim.formationState = std::make_shared<FormationState>();
  return sim.formationState->registry;
}

// CAiFormationDBImpl::NewFormation 0x59c120 (+ SetScale)
std::shared_ptr<Formation> NewFormation(Sim& sim, const std::vector<Unit*>& units, const std::string& name, float cx,
                                        float cz, float qw, float qx, float qy, float qz, int type, float scale) {
  auto F = std::make_shared<Formation>();
  F->sim = &sim;
  F->type = type;
  F->script = name;
  for (Unit* x : units) F->units.push_back(EntityRef(x));
  F->cx = cx;
  F->cz = cz;
  F->qw = qw;
  F->qx = qx;
  F->qy = qy;
  F->qz = qz;
  if (!QuatZero(*F) && F->type != 2)
    F->fwd = {2 * (F->qw * F->qy + F->qx * F->qz), 2 * (F->qy * F->qz - F->qw * F->qx),
              1 - 2 * (F->qx * F->qx + F->qy * F->qy)};
  F->scale = scale;
  Rebuild(*F);
  Registry(sim).push_back(F);
  return F;
}

// AI.GuardFormationName (ubp+0x470, default "GuardFormation")
const std::string& GuardFormationName(Sim& sim, const Unit* g) {
  static const SimCache<std::map<const BlueprintInfo*, std::string>> cacheSlot;
  auto& cache = cacheSlot.For(sim);
  auto it = cache.find(g->blueprint);
  if (it != cache.end()) return it->second;
  std::string name = "GuardFormation";
  lua_State* L = sim.L();
  int top = lua_gettop(L);
  sim.blueprints().PushTable(L, *g->blueprint);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "AI");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "GuardFormationName");
      lua_rawget(L, -2);
      if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
    }
  }
  lua_settop(L, top);
  return cache.emplace(g->blueprint, name).first->second;
}

}  // namespace

Vec3 FormationVector(Sim& sim, Unit* u) {
  if (u->unitStates.Has(US("TransportLoading")) || u->unitStates.Has(US("Refueling")) || !u->form) return {};
  if (IsAirUnit(u) && u->formLeader) {
    Unit* L = UnitOf(sim, u->formLeader);
    if (L && !IsAirUnit(L)) {
      // the leader's COMMITTED facing: its vt+0x18 is IUnit::GetTransform 0x6a49c0 (unit+0xa4, entity+0x9c),
      // 0x6a8cb7..0x6a8cc5, so a leader that has already moved in this beat is followed as the beat began with it
      Vec3 f = vm::Forward(L->shapeHeld ? L->shapeOri : L->orientation);
      float l = std::sqrt(f.x * f.x + f.z * f.z);
      if (!(l > 0)) return Vec3{};
      const float inv = 1.0f / l;  // the normalised copy 0x44f7e0 (0x6a8cf3): times 1 / length
      return Vec3{f.x * inv, inv * 0.0f, inv * f.z};
    }
  }
  if (FormationIsForm(*u->form)) return u->form->fwd;
  return {};
}

void UpdateGuardFormation(Sim& sim, Unit* G) {
  if (G->guardForm || G->guarders.empty()) return;
  const std::string& name = GuardFormationName(sim, G);
  if (name.empty()) return;
  std::vector<Unit*> guards;
  for (uint32_t h : G->guarders)
    if (Unit* x = UnitOf(sim, h)) guards.push_back(x);
  std::sort(guards.begin(), guards.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  if (guards.empty()) return;
  const Quat& q = G->orientation;
  bool mob = Mobile(G);
  G->guardForm = NewFormation(sim, guards, name, G->position.x, G->position.z, mob ? q.w : 0, mob ? q.x : 0,
                              mob ? q.y : 0, mob ? q.z : 0, 15, 1.0f);
}

void ReleaseGuardFormation(Unit* G) {
  if (!G || !G->guardForm) return;
  G->guardForm->released = true;
  G->guardForm.reset();
}

bool FormationIsForm(const Formation& f) {
  return f.type == 4 || f.type == 36 || f.type == 18 || f.type == 11 || f.type == 15;
}

int FormationScriptIndex(lua_State* L, const std::string& name, const std::vector<Unit*>& units) {
  int top = lua_gettop(L);
  int idx = -1;
  if (int m = PushModule(L)) {
    lua_pushstring(L, kTypeTables[FormationType(units)]);
    lua_gettable(L, m);
    if (lua_istable(L, -1)) {
      int t = lua_gettop(L);
      int n = ObjectCount(L, t);  // FORMATION_GetScriptIndex 0x575db0: GetCount 0x575f09, read once; no string skipped
      for (int i = 1; i <= n && idx < 0; ++i) {
        lua_rawgeti(L, t, i);
        if (lua_isstring(L, -1)) {
          std::string s = lua_tostring(L, -1);
          if (s.size() == name.size() && std::equal(s.begin(), s.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
              }))
            idx = i - 1;
        }
        lua_pop(L, 1);
      }
    }
  }
  static const bool dbgForm = getenv("MOHO64_DEBUG_FORM") != nullptr;  // (read once)
  if (dbgForm) Logf(LogLevel::Info, "form index %s -> %d (type %d)", name.c_str(), idx, FormationType(units));
  lua_settop(L, top);
  return idx;
}

void GenerateFormation(Sim& sim, UnitCommand& c, Unit* u) {
  if (c.units.size() <= 1 || c.formIndex <= -1) return;
  if (c.form) {
    if (!Contains(*c.form, u, true) && !Gone(u)) {
      c.form->units.push_back(EntityRef(u));
      c.form->dirty = true;
    }
    return;
  }
  std::vector<Unit*> units(c.units.begin(), c.units.end());
  std::sort(units.begin(), units.end(), [](const Unit* a, const Unit* b) { return a->id < b->id; });
  std::string name = ScriptName(sim.L(), c.formIndex, units);
  if (name.empty()) return;
  // CUnitCommand::GenerateFormation 0x6e88d0 centres a new formation on GetTargetPosGun(cmd+0x11c) with the centre
  // flag clear (bl = 0, 0x6e897f): for an entity target its target POINT (Unit::GetTargetPoint 0x6ab340: the bone of
  // the order's pick, or the collision centre), for a recon blip its source unit's (ReconBlip::GetTargetPoint 0x5bf5f0
  // plus the blip's offset +0x27c; taken as zero for a real blip, inferred). Not the unit's origin. Seen: MP2 6047, army 0's four
  // artillery on one Attack of a radar blip: the original's formation is laid out 0.04 / -0.03 from ours (each
  // adjusted point, probe mp2_6047), so 0x104's place lands in another cell and its square is one cell off.
  // An entity target whose entity is gone (a blip deleted since the order was given) has no point: GetTargetPosGun
  // 0x5e2a90 takes the type-1 branch (0x5e2ac5), finds the weak entity null and returns the invalid vector (0x5e2ba3:
  // the static 0x10c7aa4.. copied from 0x10a99a4, the "no target" vector, NaN). The formation is centred on it, so every
  // raw slot is NaN, AdjustPosition 0x59aa20 finds no spot in its rings and, with no next order of a moving kind, gives
  // each unit its own position (AdjustPosition below). Seen (probe mp2_10886): MP2 10886, army 1's four units start
  // their second queued formation Attack, on blip 0x30000033 of army 0's dead structure 0xe0 (deleted at 10885): the
  // original's Update puts lastPos at each unit's own cell centre ((285.5, 399.5) for 0x100015), the lost-target
  // branch finds it in reach and ends the task in its first beat; ours centred the formation on the order's stored
  // point, put each unit at its slot 18-28 away, and kept the order.
  const Vec3 pos = c.targetId ? (sim.FindEntity(c.targetId) ? CommandPosGun(sim, c) : vm::NaNVec()) : c.pos;
  auto F = NewFormation(sim, units, name, pos.x, pos.z, c.formQw, c.formQx, c.formQy, c.formQz,
                        static_cast<int>(c.type), c.formScale);
  c.form = F;
  static const bool dbgForm = getenv("MOHO64_DEBUG_FORM") != nullptr;  // (read once)
  if (dbgForm)
    for (int k = 0; k < 2; ++k)
      for (auto& g : F->groups[k])
        for (auto& [id, n] : g.nodes)
          Logf(LogLevel::Info, "form %s type %d unit %u order %d off %.3f %.3f speed %.4f", name.c_str(), F->type, id,
               n.order, n.offX, n.offZ, g.speed);
}

bool FormationHas(const UnitCommand& c, const Unit* u) { return c.form && Contains(*c.form, u, true); }

void FormationRemoveUnit(UnitCommand& c, Unit* u) {
  if (!c.form) return;
  Formation& F = *c.form;
  for (int k = 0; k < 2; ++k)
    for (FormGroup& g : F.groups[k]) {
      auto it = g.nodes.find(u->id);
      if (it == g.nodes.end()) continue;
      if (g.leaderCache == it->second.ref) g.leaderCache = 0;
      g.nodes.erase(it);
    }
  uint32_t h = EntityRef(u);
  F.units.erase(std::remove(F.units.begin(), F.units.end(), h), F.units.end());
  // (u->form, u+0x580, stays: only Unit::UpdateInfoCache 0x6a9810 writes it, in the unit's MotionTick. Seen (MP1 1778):
  // army 6's four engineers on a Guard with a formation get a Move with clear from the command stream; the
  // original's record of that beat already lists the Move but still has u+0x580 set; it is null from the next
  // beat, after their UpdateInfoCache. Ours had cleared it with the queue. u->formHold keeps the object alive.)
  if (F.units.empty()) {
    F.released = true;
    c.form.reset();
  }
}

// vt+0x18 GetAdjustedPosition for the command's formation (false: u has no place in one)
bool FormationAdjusted(Sim& sim, UnitCommand& c, Unit* u, float* x, float* z) {
  (void)sim;
  if (!c.form || c.units.size() == 1 || !Contains(*c.form, u, true)) return false;
  AdjustedPosition(*c.form, u, nullptr, x, z);
  return true;
}

// vt+0x08 SetCentre 0x569a30: a new centre (different from the old one, 0x569a44..0x569a6b, and neither x nor z a NaN:
// _isnan 0x569a86 / 0x569aa0) moves the formation; the adjusted-position cache and the reservations go, the slots
// stay (no rebuild)
void FormationSetCentre(UnitCommand& c, float x, float z) {
  if (!c.form) return;
  Formation& F = *c.form;
  if (F.cx == x && F.cz == z) return;
  if (std::isnan(x) || std::isnan(z)) return;
  F.cx = x;
  F.cz = z;
  F.adjusted.clear();
  F.reservations.clear();
}

bool FormationGoal(Sim& sim, UnitCommand& c, Unit* u, Vec3* out) {
  (void)sim;
  if (!c.form || c.units.size() == 1 || !Contains(*c.form, u, true)) return false;
  *out = FormationCell(*c.form, u, nullptr);
  return true;
}

// The attack task asks the formation it holds (task+0x58) itself: vt+0x18 (0x5f3204) and vt+0x1c (0x5f3163,
// 0x5f3255, 0x5f33c4), with no test of how many units the command still has. A formation outlives all but its
// last unit: one left alone in it keeps its place.
bool FormationAdjustedOfForm(UnitCommand& c, Unit* u, float* x, float* z) {
  if (!c.form || !Contains(*c.form, u, true)) return false;
  AdjustedPosition(*c.form, u, nullptr, x, z);
  return true;
}
bool FormationGoalOfForm(UnitCommand& c, Unit* u, Vec3* out) {
  if (!c.form || !Contains(*c.form, u, true)) return false;
  *out = FormationCell(*c.form, u, nullptr);
  return true;
}

bool FormationGroupAtGoal(Sim& sim, UnitCommand& c, Unit* u) {
  (void)sim;
  if (!c.form) return false;
  FormGroup* g = GroupOf(*c.form, u);
  return g && g->atGoal;
}

namespace {
// Unit::GetFormation 0x6a9720 (read): a guarding unit that is not an ENGINEER (u+0x689) has the guarded unit's guard
// formation (G+0x520; none while GuardBusy, IsUnitState 0x1f); anything else its head command's formation (cmd+0x118)
// when that holds it (vt+0x40(u, 1)). So a commander walking to the factory it assists is not in that factory's guard
// formation.
std::shared_ptr<Formation> FormationOf(Sim& sim, Unit* u) {
  if (u->guardedId && !u->isEngineer) {
    if (u->unitStates.Has(US("GuardBusy"))) return nullptr;
    Unit* G = UnitOf(sim, u->guardedId);
    return G ? G->guardForm : nullptr;
  }
  if (u->commands.empty()) return nullptr;
  UnitCommand& c = *u->commands.front();
  if (c.form && Contains(*c.form, u, true)) return c.form;
  return nullptr;
}
}  // namespace

Formation* GetFormation(Sim& sim, Unit* u) { return FormationOf(sim, u).get(); }

int FormationPathDelay(Sim& sim, Unit* u) {
  Formation* F = GetFormation(sim, u);
  if (!F || u->guardedId) return 1;
  FormGroup* g = GroupOf(*F, u);
  if (!g) return 1;
  auto it = g->nodes.find(u->id);
  if (it == g->nodes.end() || !(it->second.row > 0)) return 1;
  return std::max(1, static_cast<int>(it->second.row) * 10);
}

void FormationsTick(Sim& sim) {
  auto& reg = Registry(sim);
  for (size_t i = 0; i < reg.size(); ++i) {
    auto f = reg[i].lock();
    if (!f || f->released) continue;
    Update(*f);
  }
  reg.erase(std::remove_if(reg.begin(), reg.end(),
                           [](const std::weak_ptr<Formation>& w) {
                             auto f = w.lock();
                             return !f || f->released;
                           }),
            reg.end());
}

void UpdateInfoCache(Sim& sim, Unit* u) {
  if (!Mobile(u)) return;
  std::shared_ptr<Formation> Fs = FormationOf(sim, u);
  Formation* F = Fs.get();
  FormGroup* g = nullptr;
  if (F && FormationIsForm(*F) && Contains(*F, u, false)) {
    g = GroupOf(*F, u);
    u->form = F;
    u->formHold = Fs;
    auto it = g->nodes.find(u->id);
    u->formPathDelay = 1;
    if (!u->guardedId && it != g->nodes.end() && it->second.row > 0)
      u->formPathDelay = std::max(1, static_cast<int>(it->second.row) * 10);
    Unit* L = GetLeader(*F, u, g);
    u->formLeader = L ? EntityRef(L) : 0;
    u->formSlot = (it != g->nodes.end() && it->second.posSet) ? it->second.pos : u->position;
    u->formRank = it != g->nodes.end() ? it->second.rank : 0;
    bool all = true;
    for (int k = 0; k < 2; ++k)
      for (FormGroup& x : F->groups[k]) all = all && x.atGoal;
    u->formAllAtGoal = all;
  } else {
    u->form = nullptr;
    u->formHold.reset();
    u->formPathDelay = 1;
    u->formLeader = 0;
    u->formSlot = u->position;
    u->formRank = 0;
    u->formAllAtGoal = false;
  }
  // 3.3: maxSpeed
  float base = CanFly(u) ? GetAirBp(sim.L(), *u->blueprint).maxAirspeed * u->motion.speedMult / 1.0f
                         : (u->motion.bp ? u->motion.bp->maxSpeed : 0.0f) * u->motion.speedMult;
  float mult = 1.0f, capped = base;
  if (F && g) {
    float s = FormationSpeed(*F, u, &mult, g);
    if (s > 0) capped = s > base ? base : s;
  }
  float ms = mult * capped > base ? base : mult * capped;
  u->motion.speedCap = (F && g) ? ms : 0.0f;
  u->motion.maxSpeedU594 = ms;
  static const long dbg = getenv("MOHO64_DEBUG_FORMU") ? atol(getenv("MOHO64_DEBUG_FORMU")) : -1;
  if (dbg >= 0 && static_cast<long>(u->id) == dbg)
    Logf(LogLevel::Info, "formu %u self %u F %p g %p slot %.3f %.3f leader %u cap %.4f mult %.4f mid %.4f pos %.3f %.3f follow %d",
         sim.tick(), EntityRef(u), static_cast<void*>(F), static_cast<void*>(g), u->formSlot.x, u->formSlot.z, u->formLeader, ms, mult,
         g ? g->mid : -1.0f, u->position.x, u->position.z, LandNavFollowingSlot(u) ? 1 : 0);
  if (dbg >= 0 && static_cast<long>(u->id) == dbg && F)
    Logf(LogLevel::Info, "formu %u   formation '%s' type %d is a form %d, %zu units, holds this unit %d, groups %zu + %zu, group speed %.4f",
         sim.tick(), F->script.c_str(), F->type, FormationIsForm(*F) ? 1 : 0, F->units.size(), Contains(*F, u, false) ? 1 : 0,
         F->groups[0].size(), F->groups[1].size(), g ? g->speed : -1.0f);
}

void FormationsForget(Sim& sim) { sim.formationState.reset(); }

}  // namespace moho
