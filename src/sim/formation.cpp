// Formations: engine-ref/specs/formations.md (CAiFormationInstance 0x5694b0.., CAiFormationDBImpl 0x59c030..,
// Unit::UpdateInfoCache 0x6a9810).
#include "sim/formation.h"

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
#include "sim/commands.h"
#include "sim/landnav.h"
#include "sim/motion.h"
#include "sim/sim.h"
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
// the rebuild's unit filter (0x568ca0): mobile, alive, complete
bool ValidMember(const Unit* u) { return !Gone(u) && Mobile(u) && !u->beingBuilt && !u->parentId; }
bool IsAirUnit(const Unit* u) { return u->motion.bp && u->motion.bp->motionType == kMotionAir; }
int LayerIndex(const Unit* u) { return IsAirUnit(u) ? 1 : 0; }
int FpSizeX(const Unit* u) { return u->motion.bp ? std::max<int>(1, u->motion.bp->footprint.sizeX) : 1; }
int FpSizeZ(const Unit* u) { return u->motion.bp ? std::max<int>(1, u->motion.bp->footprint.sizeZ) : 1; }
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
bool QuatZero(const Formation& f) { return f.qw == 0 && f.qx == 0 && f.qy == 0 && f.qz == 0; }
bool AssignZero(const Formation& f) { return f.aw == 0 && f.ax == 0 && f.ay == 0 && f.az == 0; }
// MultQuadVec with (w, x, y, z)
Vec3 RotW(float w, float x, float y, float z, Vec3 v) { return vm::Rotate(Quat{x, y, z, w}, v); }
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
      int n = luaL_getn(L, t);
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

// FORMATION_RunScript 0x576690: <name>(units, 0-based); entries {x, z, filter, order, rotate}
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
    lua_settop(L, top);
    return false;
  }
  int r = lua_gettop(L);
  for (int i = 1;; ++i) {
    lua_rawgeti(L, r, i);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    int e = lua_gettop(L);
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
    lua_settop(L, r);
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
  // unit side (8.2)
  double sx = 0, sz = 0;
  for (Unit* u : units) {
    sx += u->position.x;
    sz += u->position.z;
  }
  G.cx = static_cast<float>(sx / units.size());
  G.cz = static_cast<float>(sz / units.size());
  struct URec {
    Unit* u;
    float rx, rz;
  };
  std::vector<URec> U;
  for (Unit* u : units) {
    Vec3 rel{u->position.x - G.cx, 0, u->position.z - G.cz};
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
  double mx = 0, mz = 0;
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
  float mOffX = static_cast<float>(mx / S.size()), mOffZ = static_cast<float>(mz / S.size());
  G.extX = std::max(0.0f, maxX - minX);
  G.extZ = std::max(0.0f, maxZ - minZ);
  float E = std::max(G.extX, G.extZ);
  for (size_t i = 0; i < S.size(); ++i) {
    float dx = slots[i].x - slots[0].x, dz = slots[i].z - (slots[0].z + E);
    S[i].key = dx * dx + dz * dz;
  }
  // 8.4: stable descending sort (the <= 32 insertion-sort path)
  std::stable_sort(S.begin(), S.end(), [](const SRec& a, const SRec& b) { return a.key > b.key; });
  // 8.5: matching
  int order = 1;
  G.speed = kInfF;
  for (const SRec& s : S) {
    if (U.empty()) break;
    float tx = G.cx + (s.ox - mOffX), tz = G.cz + (s.oz - mOffZ);
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
    Vec3 f = vm::Forward(u->orientation);
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

void EnsureBuilt(Formation& F) {
  if (F.dirty) Rebuild(F);
}

FormGroup* GroupOf(Formation& F, const Unit* u) {
  EnsureBuilt(F);
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

// vt+0x20 GetRawPosition
void RawPosition(Formation& F, Unit* u, FormGroup* g, float* x, float* z) {
  *x = u->position.x;
  *z = u->position.z;
  if (!g) g = GroupOf(F, u);
  if (!g || !Mobile(u)) return;
  auto it = g->nodes.find(u->id);
  if (it == g->nodes.end()) return;
  *x = F.cx + it->second.offX;
  *z = F.cz + it->second.offZ;
}

// COORDS_CanMoveAt with strict 0 (air_extra 3.6): no standing, completed, non-air unit of another formation
bool CanMoveAt(Sim& sim, Unit* u, int cx, int cz) {
  float x0 = static_cast<float>(cx), z0 = static_cast<float>(cz);
  float x1 = x0 + FpSizeX(u), z1 = z0 + FpSizeZ(u);
  bool blocked = false;
  sim.ForUnitsInRect(x0 - 8, z0 - 8, x1 + 8, z1 + 8, [&](Unit* e) {
    if (blocked || e == u || Gone(e) || e->beingBuilt || e->layer == "Air") return;
    if (u->layer == "Sub" && e->layer != "Sub") return;
    bool moving = e->position.x != e->lastPosition.x || e->position.z != e->lastPosition.z;
    if (moving) return;
    if (e->form && e->form == u->form && !e->unitStates.count("Attacking") && !u->unitStates.count("Attacking")) return;
    if (e->unitStates.count("Attached")) return;
    const MotionBlueprint* b = e->motion.bp;
    float hx = b ? b->sizeX * 0.5f : 0.5f, hz = b ? b->sizeZ * 0.5f : 0.5f;
    if (e->position.x + hx < x0 || e->position.x - hx > x1 || e->position.z + hz < z0 || e->position.z - hz > z1) return;
    blocked = true;
  });
  return !blocked;
}

bool IsWithinMap(Sim& sim, float x, float z, float size) {
  const TerrainMap* map = sim.map();
  if (!map) return true;
  return x - size >= 0 && z - size >= 0 && x + size <= map->width() && z + size <= map->height();
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

int CellOfX(const Unit* u, float x) { return static_cast<int>(std::nearbyint(x - FpSizeX(u) * 0.5f)); }
int CellOfZ(const Unit* u, float z) { return static_cast<int>(std::nearbyint(z - FpSizeZ(u) * 0.5f)); }

// vt+0x64 AdjustPosition (7.1)
void AdjustPosition(Formation& F, Unit* u, float* px, float* pz) {
  Sim& sim = *F.sim;
  if (Gone(u) || F.type == 15 || F.scale < 1.0f || LayerIndex(u) == 1) return;
  int size = std::max(FpSizeX(u), FpSizeZ(u));
  int layer = LayerIndex(u);
  auto spot = [&](float x, float z) {
    int cx = CellOfX(u, x), cz = CellOfZ(u, z);
    return LandCellFits(sim, u, cx, cz) && CanMoveAt(sim, u, cx, cz) && IsWithinMap(sim, x, z, static_cast<float>(size)) &&
           IsFree(F, x, z, size, layer);
  };
  float x = *px, z = *pz;
  if (spot(x, z)) {
    F.reservations.push_back({x, z, size, layer});
    return;
  }
  int count = 0;
  for (int r = 1;; ++r) {
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
  if (!u->commands.empty()) {
    CommandType t = u->commands.front()->type;
    if (t == CommandType::Move || t == CommandType::Attack || t == CommandType::Patrol || t == CommandType::FormMove ||
        t == CommandType::FormAttack || t == CommandType::FormPatrol || t == CommandType::Guard)
      return;
  }
  *px = u->position.x;
  *pz = u->position.z;
}

// vt+0x18 GetAdjustedPosition (cached per unit until the next rebuild)
void AdjustedPosition(Formation& F, Unit* u, FormGroup* g, float* x, float* z) {
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
    if (m->unitStates.count("Refueling")) return;
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
    if (!nextMove && (F.fwd.x != 0 || F.fwd.y != 0 || F.fwd.z != 0)) {
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
      float ll = std::sqrt(lax * lax + laz * laz);
      if (ll > 0) {
        float s = std::min(ll, 20.0f) / ll;
        lax *= s;
        laz *= s;
      }
      // how far the leader is from its own slot cell
      Vec3 lc = FormationCell(F, L, &g);
      int dcx = CellOfX(L, lc.x) - CellOfX(L, L->position.x), dcz = CellOfZ(L, lc.z) - CellOfZ(L, L->position.z);
      bool formed = std::sqrt(static_cast<float>(dcx * dcx + dcz * dcz)) < 5.0f && F.type != 15;
      // reference point
      float maxR = 0.001f;
      double scx = 0, scz = 0;
      int cnt = 0;
      for (auto& [id, n] : g.nodes) {
        Unit* m = UnitOf(sim, n.ref);
        if (!m) continue;
        maxR = std::max(maxR, Dist2D(m->position.x, m->position.z, L->position.x, L->position.z));
        Vec3 c = FormationCell(F, m, &g);
        scx += c.x;
        scz += c.z;
        ++cnt;
      }
      float Cx, Cz;
      if (F.type == 15) {
        Cx = cnt ? static_cast<float>(scx / cnt) : L->position.x;
        Cz = cnt ? static_cast<float>(scz / cnt) : L->position.z;
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
            float hF = dmath::Atan2(F.fwd.x, F.fwd.z);
            float diff = Wrap(hL - hF);
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
        minD = std::min(minD, n.d);
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

std::vector<std::weak_ptr<Formation>>& Registry(Sim& sim) {
  static std::map<const Sim*, std::vector<std::weak_ptr<Formation>>> m;
  return m[&sim];
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
  static std::map<const BlueprintInfo*, std::string> cache;
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
  if (u->unitStates.count("TransportLoading") || u->unitStates.count("Refueling") || !u->form) return {};
  if (IsAirUnit(u) && u->formLeader) {
    Unit* L = UnitOf(sim, u->formLeader);
    if (L && !IsAirUnit(L)) {
      Vec3 f = vm::Forward(L->orientation);
      float l = std::sqrt(f.x * f.x + f.z * f.z);
      return l > 0 ? Vec3{f.x / l, 0, f.z / l} : Vec3{};
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
      int n = luaL_getn(L, t);
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
  if (getenv("MOHO64_DEBUG_FORM")) Logf(LogLevel::Info, "form index %s -> %d (type %d)", name.c_str(), idx, FormationType(units));
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
  Vec3 pos = c.pos;
  if (c.targetId)
    if (Entity* e = sim.FindEntity(c.targetId)) pos = e->position;
  auto F = NewFormation(sim, units, name, pos.x, pos.z, c.formQw, c.formQx, c.formQy, c.formQz,
                        static_cast<int>(c.type), c.formScale);
  c.form = F;
  if (getenv("MOHO64_DEBUG_FORM"))
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
  if (u->form == &F) u->form = nullptr;
  if (F.units.empty()) {
    F.released = true;
    c.form.reset();
  }
}

bool FormationGoal(Sim& sim, UnitCommand& c, Unit* u, Vec3* out) {
  (void)sim;
  if (!c.form || c.units.size() == 1 || !Contains(*c.form, u, true)) return false;
  *out = FormationCell(*c.form, u, nullptr);
  return true;
}

bool FormationGroupAtGoal(Sim& sim, UnitCommand& c, Unit* u) {
  (void)sim;
  if (!c.form) return false;
  FormGroup* g = GroupOf(*c.form, u);
  return g && g->atGoal;
}

Formation* GetFormation(Sim& sim, Unit* u) {
  if (u->guardedId) {  // the guarded unit's guard formation (G+0x520), not while GuardBusy
    if (u->unitStates.count("GuardBusy")) return nullptr;
    Unit* G = UnitOf(sim, u->guardedId);
    return G && G->guardForm ? G->guardForm.get() : nullptr;
  }
  if (u->commands.empty()) return nullptr;
  UnitCommand& c = *u->commands.front();
  if (c.form && Contains(*c.form, u, true)) return c.form.get();
  return nullptr;
}

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
  Formation* F = GetFormation(sim, u);
  FormGroup* g = nullptr;
  if (F && FormationIsForm(*F) && Contains(*F, u, false)) {
    g = GroupOf(*F, u);
    u->form = F;
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
  static const long dbg = getenv("MOHO64_DEBUG_FORMU") ? atol(getenv("MOHO64_DEBUG_FORMU")) : -1;
  if (dbg >= 0 && static_cast<long>(u->id) == dbg)
    Logf(LogLevel::Info, "formu %u self %u F %p g %p slot %.3f %.3f leader %u cap %.4f mult %.4f mid %.4f pos %.3f %.3f follow %d",
         sim.tick(), EntityRef(u), static_cast<void*>(F), static_cast<void*>(g), u->formSlot.x, u->formSlot.z, u->formLeader, ms, mult,
         g ? g->mid : -1.0f, u->position.x, u->position.z, LandNavFollowingSlot(u) ? 1 : 0);
}

}  // namespace moho
