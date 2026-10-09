// Intel grids and recon blips: see intel.h.
#include "sim/intel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/combat.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace combat {
int Relation(const Army* a, const Army* b);
}

namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

struct Grid {
  int cs = 4, W = 0, H = 0;
  std::vector<int8_t> c;
  bool Visible(Vec3 p) const {
    int x = static_cast<int>(std::floor(p.x * (1.0f / static_cast<float>(cs))));
    int z = static_cast<int>(std::floor(p.z * (1.0f / static_cast<float>(cs))));
    if (x < 0 || z < 0 || x >= W || z >= H) return false;
    return c[static_cast<size_t>(z) * W + x] != 0;
  }
  // CIntelGrid::Raster 0x507540 (r in cells; one cell short in +x and +z, as the original)
  void Raster(Vec3 p, int r, int delta) {
    int cx = static_cast<int>(std::floor(p.x * (1.0f / static_cast<float>(cs))));
    int cz = static_cast<int>(std::floor(p.z * (1.0f / static_cast<float>(cs))));
    int x0 = std::max(0, std::min(cx - r, W)), x1 = std::max(0, std::min(cx + r, W));
    for (int x = x0; x < x1; ++x) {
      int dx = cx - x;
      int h = static_cast<int>(std::sqrt(static_cast<float>(r * r - dx * dx)));
      int z0 = std::max(0, std::min(cz - h, H)), z1 = std::max(0, std::min(cz + h, H));
      for (int z = z0; z < z1; ++z) {
        int8_t& v = c[static_cast<size_t>(z) * W + x];
        v = static_cast<int8_t>(v + delta);
      }
    }
  }
};

// Per army: 0 vision, 1 water vision, 2 radar, 3 sonar, 4 omni, 5 RCI, 6 SCI, 7 VCI.
struct ArmyIntel {
  std::unique_ptr<Grid> g[8];
  bool fog = true;
};
std::vector<ArmyIntel> g_armies;
std::vector<Entity*> g_sources;     // entities with intel handles
std::vector<ReconBlip*> g_blips;    // live blip entities

const char* const kIntelNames[14] = {"None", "Vision", "WaterVision", "Radar", "Sonar", "Omni", "RadarStealthField",
                                     "SonarStealthField", "CloakField", "Jammer", "Spoof", "Cloak", "RadarStealth",
                                     "SonarStealth"};
int IntelType(const char* s) {
  for (int i = 1; i < 14; ++i)
    if (!std::strcmp(s, kIntelNames[i])) return i;
  return -1;
}

Grid* GridFor(int army, int slot) {  // pos handle slots 1..5
  if (army < 0 || army >= static_cast<int>(g_armies.size())) return nullptr;
  return g_armies[static_cast<size_t>(army)].g[slot - 1].get();
}

void Viz(IntelHandle& h, int slot, int delta) {
  if (!h.enabled || h.radius == 0) return;
  if (slot <= 5) {
    Grid* g = GridFor(h.army, slot);
    if (!g) return;
    g->Raster(h.pos, static_cast<int>(h.radius / static_cast<uint32_t>(g->cs)), delta);
  } else {  // counter fields paint into every other army's counter grid
    for (size_t a = 0; a < g_armies.size(); ++a) {
      if (static_cast<int>(a) == h.army) continue;
      Grid* g = g_armies[a].g[slot - 1].get();
      if (g) g->Raster(h.pos, static_cast<int>(h.radius / static_cast<uint32_t>(g->cs)), delta);
    }
  }
}

EntityIntel& IntelOf(Entity* e) {
  if (!e->intel) {
    e->intel = std::make_shared<EntityIntel>();
    g_sources.push_back(e);
  }
  return *e->intel;
}

Vec3 SourcePos(const Entity* e) { return {e->position.x, e->position.y + 1, e->position.z}; }

// CIntelPosHandle::UpdatePos 0x76f1e0 (exact: CIntel::Update 0x76e4c0)
void UpdatePos(IntelHandle& h, int slot, Vec3 p, uint32_t tick, bool exact) {
  if (!h.exists) return;
  if (!h.enabled) {
    h.pos = p;
    if (exact) h.lastTick = tick;
    return;
  }
  bool moved = p.x != h.pos.x || p.y != h.pos.y || p.z != h.pos.z;
  if (!exact) {
    float dx = p.x - h.pos.x, dy = p.y - h.pos.y, dz = p.z - h.pos.z;
    float t = static_cast<float>(h.radius) * 0.333f;
    if (!(dx * dx + dy * dy + dz * dz >= t * t || tick - h.lastTick > 5)) return;
  }
  h.lastTick = tick;
  if (moved) {
    Viz(h, slot, -1);
    h.pos = p;
    Viz(h, slot, +1);
  }
}

int ArmyIdx(const Army* a) { return a ? a->index - 1 : -1; }

bool Underwater(const Entity* e) {
  if (e->kind != Entity::Kind::Unit) return false;
  const std::string& l = static_cast<const Unit*>(e)->layer;
  return l == "Seabed" || l == "Sub";
}

uint8_t NewReconFor(const ArmyIntel& db, const Entity* e, Vec3 p, bool underwater) {
  uint8_t f = 0;
  if (!db.fog || !db.g[0]) f = 8;
  else if (underwater ? (db.g[1] && db.g[1]->Visible(p)) : db.g[0]->Visible(p)) f = 8;
  bool sonar = underwater;
  if (!sonar && e && e->kind == Entity::Kind::Unit) {
    const std::string& l = static_cast<const Unit*>(e)->layer;
    sonar = l == "Sub" || l == "Water";
  }
  if (sonar && db.g[3] && db.g[3]->Visible(p)) f |= 2;
  if (!underwater && db.g[2] && db.g[2]->Visible(p)) f |= 1;
  if (db.g[4] && db.g[4]->Visible(p)) f |= 4;
  return f;
}

bool FlagOn(const Entity* e, int type) {
  return e->intel && e->intel->has[type] && e->intel->on[type];
}

// ReconCanDetect 0x5c18f0 + GetReconFlags 0x5c9600 + ApplyReconCounters 0x5cb460 (mask 0xF)
uint8_t CanDetect(Sim& sim, int a, const Entity* e, Vec3 p) {
  const TerrainMap* m = sim.map();
  if (m && (p.x < 0 || p.z < 0 || p.x > m->width() || p.z > m->height())) return 0;
  const auto& armies = sim.armies();
  const Army* us = armies[static_cast<size_t>(a)].get();
  if (e->army && combat::Relation(e->army, us) == 2) return 0xF;
  bool under = Underwater(e);
  const ArmyIntel& db = g_armies[static_cast<size_t>(a)];
  uint8_t f = NewReconFor(db, e, p, under);
  for (size_t j = 0; j < armies.size() && j < g_armies.size(); ++j)
    if (static_cast<int>(j) != a && combat::Relation(armies[j].get(), us) == 2)
      f |= NewReconFor(g_armies[j], e, p, under);
  if (f == 0) return 0;
  if (f & 4) return f;
  bool unit = e->kind == Entity::Kind::Unit;
  if (db.g[5] && db.g[5]->Visible(p)) f &= static_cast<uint8_t>(~1);
  if (db.g[6] && db.g[6]->Visible(p)) f &= static_cast<uint8_t>(~2);
  if ((unit && FlagOn(e, 11)) || (db.g[7] && db.g[7]->Visible(p))) f &= static_cast<uint8_t>(~8);
  if (unit && !(f & 8)) {
    if (FlagOn(e, 12)) f &= static_cast<uint8_t>(~1);
    if (FlagOn(e, 13)) f &= static_cast<uint8_t>(~2);
  }
  return f;
}

// The blip Lua object: an instance of /lua/sim/Blip.lua's Blip.
ReconBlip* MakeBlip(Sim& sim, lua_State* L, Unit* u) {
  auto o = std::make_unique<ReconBlip>();
  ReconBlip* b = o.get();
  b->kind = Entity::Kind::Blip;
  b->source = u;
  b->mobile = u->motion.bp && u->motion.bp->mobile();
  b->blueprint = u->blueprint;
  b->army = u->army;
  b->id = sim.ReserveId(u->army, 0x3);
  b->position = u->position;
  b->orientation = u->orientation;
  int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_getglobal(L, "import");
  lua_pushstring(L, "/lua/sim/Blip.lua");
  bool ok = lua_pcall(L, 1, 1, top + 1) == 0 && lua_istable(L, -1);
  if (ok) {
    lua_pushstring(L, "Blip");
    lua_gettable(L, -2);
    lua_pushcfunction(L, ScriptTraceback);
    lua_insert(L, -2);
    ok = lua_pcall(L, 0, 1, lua_gettop(L) - 1) == 0 && lua_istable(L, -1);
  }
  if (!ok) {
    lua_settop(L, top);
    CreateObject(L, b, "ReconBlip");
  }
  BindObject(L, -1, b);
  lua_settop(L, top);
  sim.AddEntity(std::move(o));
  g_blips.push_back(b);
  return b;
}

void IntelEvent(Sim& sim, lua_State* L, int a, ReconBlip* b, int bit, bool val) {
  static const char* names[9] = {nullptr, "Radar", "Sonar", nullptr, "Omni", nullptr, nullptr, nullptr, "LOSNow"};
  Army* army = sim.armies()[static_cast<size_t>(a)].get();
  if (!army->brain || !army->brain->HasLuaObject() || !b || !b->HasLuaObject()) return;
  PushObject(L, b);
  lua_pushstring(L, names[bit]);
  lua_pushboolean(L, val);
  sim.CallMethod(L, army->brain, "OnIntelChange", 3);
}
void CheckIntelEvents(Sim& sim, lua_State* L, int a, ReconBlip* b, uint8_t old, uint8_t now) {
  const int bits[4] = {8, 1, 2, 4};
  for (int bit : bits)
    if ((old ^ now) & bit) IntelEvent(sim, L, a, b, bit, (now & bit) != 0);
}

void UpdateBlip(Sim& sim, lua_State* L, int a, Unit* u, uint8_t flags) {
  uint8_t old = u->recon[a] & 0x7f;
  flags |= old & 0x30;
  if (flags & 8) flags |= 0x10;
  u->recon[a] = static_cast<uint8_t>(kReconInUse | flags);
  CheckIntelEvents(sim, L, a, u->blip, old, flags);
}

void DeleteEntry(Sim& sim, lua_State* L, int a, Unit* u) {
  uint8_t old = u->recon[a] & 0x7f;
  CheckIntelEvents(sim, L, a, u->blip, old, 0);
  u->recon[a] = 0;
}

// ReconBlip::DestroyIfUnused 0x5bf6f0
void DestroyIfUnused(Sim& sim, ReconBlip* b) {
  Unit* u = b->source;
  if (u && !u->dead && !u->destroyQueued) return;
  if (u)
    for (int a = 0; a < 16; ++a)
      if (u->recon[a] & kReconInUse) return;
  if (u) u->blip = nullptr;
  b->source = nullptr;
  if (!b->destroyQueued) sim.QueueDestroy(b);
  g_blips.erase(std::remove(g_blips.begin(), g_blips.end(), b), g_blips.end());
}

std::vector<Unit*> g_goneUnits;  // destroyed units whose blips armies may still hold

// CAiReconDBImpl::ReconTick 0x5c0c40 for army a
void ReconTick(Sim& sim, lua_State* L, int a) {
  // blips whose unit is gone: mobile ones go, structures stay (MaybeDead) until seen again
  for (size_t i = 0; i < g_goneUnits.size(); ++i) {
    Unit* u = g_goneUnits[i];
    if (!(u->recon[a] & kReconInUse)) continue;
    ReconBlip* b = u->blip;
    bool mobile = b ? b->mobile : true;
    bool allied = u->army && combat::Relation(u->army, sim.armies()[static_cast<size_t>(a)].get()) == 2;
    Vec3 p = b ? b->position : u->position;
    if (mobile || allied || (CanDetect(sim, a, b ? static_cast<Entity*>(b) : u, p) & 8)) {
      DeleteEntry(sim, L, a, u);
      if (b) DestroyIfUnused(sim, b);
    } else {
      u->recon[a] |= 0x40;
    }
  }
  g_goneUnits.erase(std::remove_if(g_goneUnits.begin(), g_goneUnits.end(),
                                   [](Unit* u) {
                                     for (int k = 0; k < 16; ++k)
                                       if (u->recon[k] & kReconInUse) return false;
                                     return true;
                                   }),
                    g_goneUnits.end());
  // every unit of every other army
  struct Pending {
    Unit* u;
    uint8_t flags;
  };
  std::vector<Pending> pending;
  const Army* us = sim.armies()[static_cast<size_t>(a)].get();
  for (const auto& army : sim.armies()) {
    if (army.get() == us) continue;
    std::vector<Unit*> list = army->units;
    for (Unit* u : list) {
      if (u->destroyQueued) continue;
      if (!BpInCategory(sim, u->blueprint, "VISIBLETORECON")) continue;
      uint8_t f = CanDetect(sim, a, u, u->position);
      bool inUse = (u->recon[a] & kReconInUse) != 0;
      if (f && !inUse) {
        pending.push_back({u, f});
      } else if (f) {
        UpdateBlip(sim, L, a, u, f);
      } else if (inUse) {
        bool mobile = u->motion.bp && u->motion.bp->mobile();
        if ((u->recon[a] & 0x10) && !mobile) UpdateBlip(sim, L, a, u, 0);
        else {
          DeleteEntry(sim, L, a, u);
          if (u->blip) DestroyIfUnused(sim, u->blip);
        }
      }
    }
  }
  // GenerateNewBlips 0x5c0a70
  for (const Pending& p : pending) {
    Unit* u = p.u;
    if (u->destroyQueued) continue;
    if (!u->blip) u->blip = MakeBlip(sim, L, u);
    u->recon[a] = kReconInUse;
    UpdateBlip(sim, L, a, u, p.flags);
    if (u->HasLuaObject()) {
      lua_pushnumber(L, a + 1);
      sim.CallMethod(L, u, "OnDetectedBy", 1);
    }
  }
}

}  // namespace

bool ArmyHasBlip(const Unit* u, int a) { return a >= 0 && a < 16 && (u->recon[a] & kReconInUse) != 0; }

void InitIntelGrids(Sim& sim, lua_State* L) {
  g_armies.clear();
  g_sources.clear();
  g_blips.clear();
  g_goneUnits.clear();
  bool fog = true;
  int top = lua_gettop(L);
  lua_getglobal(L, "ScenarioInfo");
  int si = lua_gettop(L);
  int opts = lu::Sub(L, si, "Options");
  std::string f = lu::Str(L, opts, "FogOfWar", "explored");
  if (f == "none") fog = false;
  lua_settop(L, top);
  const TerrainMap* m = sim.map();
  int mw = m ? m->width() : 1024, mh = m ? m->height() : 1024;
  g_armies.resize(sim.armies().size());
  for (auto& ai : g_armies) {
    ai.fog = fog;
    for (int i = 0; i < 8; ++i) {
      if (i <= 1 && !fog) continue;
      auto g = std::make_unique<Grid>();
      g->cs = i == 0 ? 2 : 4;
      g->W = mw / g->cs;
      g->H = mh / g->cs;
      g->c.assign(static_cast<size_t>(g->W) * g->H, 0);
      ai.g[i] = std::move(g);
    }
  }
}

void CreateUnitIntel(Sim& sim, lua_State* L, Unit* u) {
  (void)sim;
  int top = lua_gettop(L);
  Sim::From(L)->blueprints().PushTable(L, *u->blueprint);
  int intel = lu::Sub(L, lua_gettop(L), "Intel");
  const char* radii[9] = {nullptr, "VisionRadius", "WaterVisionRadius", "RadarRadius", "SonarRadius", "OmniRadius",
                          "RadarStealthFieldRadius", "SonarStealthFieldRadius", "CloakFieldRadius"};
  EntityIntel* ei = nullptr;
  Vec3 p = SourcePos(u);
  for (int s = 1; s <= 8; ++s) {
    float r = lu::Num(L, intel, radii[s], 0);
    uint32_t R = r > 0 ? static_cast<uint32_t>(r) : 0;
    if (R == 0) continue;
    if (!ei) ei = &IntelOf(u);
    IntelHandle& h = ei->h[s];
    h.exists = true;
    h.radius = R;
    h.army = ArmyIdx(u->army);
    h.pos = p;
  }
  bool rs = lu::Bool(L, intel, "RadarStealth"), ss = lu::Bool(L, intel, "SonarStealth"), cl = lu::Bool(L, intel, "Cloak");
  float jb = lu::Num(L, intel, "JammerBlips", 0);
  int jr = lu::Sub(L, intel, "JamRadius");
  float jmax = lu::Num(L, jr, "Max", 0);
  lua_pop(L, 1);
  int sr = lu::Sub(L, intel, "SpoofRadius");
  float smax = lu::Num(L, sr, "Max", 0);
  if (rs || ss || cl || (jb > 0 && jmax > 0) || smax > 0) {
    EntityIntel& e = IntelOf(u);
    e.has[12] = rs;
    e.has[13] = ss;
    e.has[11] = cl;
    e.has[9] = jb > 0 && jmax > 0;
    e.has[10] = smax > 0;
  }
  lua_settop(L, top);
}

void ReleaseEntityIntel(Sim& sim, Entity* e) {
  (void)sim;
  if (e->intel) {
    for (int s = 1; s <= 8; ++s) Viz(e->intel->h[s], s, -1);
    e->intel->h[0] = IntelHandle{};
    for (int s = 1; s <= 8; ++s) e->intel->h[s].enabled = false;
    g_sources.erase(std::remove(g_sources.begin(), g_sources.end(), e), g_sources.end());
  }
  if (e->kind == Entity::Kind::Unit) {
    Unit* u = static_cast<Unit*>(e);
    bool any = false;
    for (int a = 0; a < 16; ++a) any = any || (u->recon[a] & kReconInUse);
    if (any) g_goneUnits.push_back(u);
    else if (u->blip) DestroyIfUnused(sim, u->blip);
  }
}

void ReconBeat(Sim& sim) {
  lua_State* L = sim.L();
  if (g_armies.size() != sim.armies().size()) InitIntelGrids(sim, L);
  // every blip follows its unit (ReconBlip::Refresh 0x5bf810)
  for (ReconBlip* b : g_blips) {
    Unit* u = b->source;
    if (!u || u->destroyQueued) continue;
    b->position = u->position;
    b->orientation = u->orientation;
    b->health = u->health;
    b->maxHealth = u->maxHealth;
  }
  int n = static_cast<int>(sim.armies().size());
  if (n <= 0) return;
  ReconTick(sim, L, static_cast<int>(sim.tick() % static_cast<uint32_t>(n)));
}

void AdvanceIntelCoords(Sim& sim) {
  uint32_t tick = sim.tick();
  for (size_t i = 0; i < g_sources.size(); ++i) {
    Entity* e = g_sources[i];
    if (!e->intel || e->destroyQueued) continue;
    EntityIntel& ei = *e->intel;
    Vec3 p = SourcePos(e);
    bool exact = false, moved;
    if (e->kind == Entity::Kind::Unit) {
      Unit* u = static_cast<Unit*>(e);
      moved = u->lastMoveTick == tick;
      // coming to rest: exact (CUnitMotion::SetMotionHorzEvent Stopped -> Entity::UpdateIntel)
      if (!moved && u->lastMoveTick + 1 == tick) exact = true;
    } else {
      IntelHandle* any = nullptr;
      for (int s = 1; s <= 8; ++s)
        if (ei.h[s].exists) any = &ei.h[s];
      moved = any && (any->pos.x != p.x || any->pos.y != p.y || any->pos.z != p.z);
      exact = moved;  // script entities move by Warp / attach: exact
    }
    if (!moved && !exact) continue;
    for (int s = 1; s <= 8; ++s) UpdatePos(ei.h[s], s, p, tick, exact);
  }
}

// ---- Lua ----------------------------------------------------------------------------------------

namespace {

int TypeArg(lua_State* L, int idx) {
  const char* s = luaL_checkstring(L, idx);
  int t = IntelType(s);
  if (t < 0) luaL_error(L, "Unknown intel type %s", s);
  return t;
}

// InitIntel(army, type, [radius]) 0x68ea20
int l_InitIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Sim& sim = *S(L);
  Army* army = sim.GetArmy(L, 2);
  int t = TypeArg(L, 3);
  EntityIntel& ei = IntelOf(e);
  if (t >= 1 && t <= 8) {
    uint32_t R = 0;
    if (lua_isnumber(L, 4)) {
      double r = lua_tonumber(L, 4);
      R = r > 0 ? static_cast<uint32_t>(r) : 0;
      if (R == 0) return luaL_error(L, "InitIntel: radius rounds to 0");
    }
    IntelHandle& h = ei.h[t];
    Viz(h, t, -1);
    h = IntelHandle{};
    h.exists = true;
    h.radius = R;
    h.army = ArmyIdx(army);
    h.pos = SourcePos(e);
  } else if (t == 9 || t == 11 || t == 12 || t == 13) {
    ei.has[t] = true;
  } else {
    Logf(LogLevel::Warning, "Unknown intel type %i", t);
  }
  return 0;
}
int l_EnableIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  if (!e->intel) return 0;
  EntityIntel& ei = *e->intel;
  if (t <= 8) {
    IntelHandle& h = ei.h[t];
    if (h.exists && !h.enabled) {
      h.enabled = true;
      Viz(h, t, +1);
    }
  } else if (ei.has[t]) {
    ei.on[t] = true;
  }
  return 0;
}
int l_DisableIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  if (!e->intel) return 0;
  EntityIntel& ei = *e->intel;
  if (t <= 8) {
    IntelHandle& h = ei.h[t];
    if (h.exists && h.enabled) {
      Viz(h, t, -1);
      h.enabled = false;
    }
  } else {
    ei.on[t] = false;
  }
  return 0;
}
int l_IsIntelEnabled(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  bool on = false;
  if (e->intel) on = t <= 8 ? e->intel->h[t].enabled : (e->intel->has[t] && e->intel->on[t]);
  lua_pushboolean(L, on);
  return 1;
}
int l_SetIntelRadius(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  double r = luaL_checknumber(L, 3);
  uint32_t R = r > 0 ? static_cast<uint32_t>(r) : 0;
  if (!e->intel || t > 8) return 0;
  IntelHandle& h = e->intel->h[t];
  if (!h.exists || h.radius == R) return 0;
  Viz(h, t, -1);
  h.radius = R;
  Viz(h, t, +1);
  return 0;
}
int l_GetIntelRadius(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  lua_pushnumber(L, e->intel && t <= 8 ? static_cast<float>(e->intel->h[t].radius) : 0.0f);
  return 1;
}

// Unit:GetBlip(armyIndex): the blip that army holds of the unit, or nil.
int l_GetBlip(lua_State* L) {
  Unit* u = CheckObject<Unit>(L, 1);
  Army* army = S(L)->GetArmy(L, 2);
  int a = ArmyIdx(army);
  if (!ArmyHasBlip(u, a) || !u->blip || !u->blip->HasLuaObject()) {
    lua_pushnil(L);
    return 1;
  }
  PushObject(L, u->blip);
  return 1;
}

ReconBlip* RB(lua_State* L) { return CheckObject<ReconBlip>(L, 1); }
uint8_t BlipFlags(lua_State* L) {
  ReconBlip* b = RB(L);
  Army* army = S(L)->GetArmy(L, 2);
  int a = ArmyIdx(army);
  Unit* u = b->source;
  if (!u || a < 0 || a >= 16) return 0;
  return u->recon[a] & 0x7f;
}
int l_blip_IsOnRadar(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 1); return 1; }
int l_blip_IsOnSonar(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 2); return 1; }
int l_blip_IsOnOmni(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 4); return 1; }
int l_blip_IsSeenNow(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 8); return 1; }
int l_blip_IsSeenEver(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 0x10); return 1; }
int l_blip_IsKnownFake(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 0x20); return 1; }
int l_blip_IsMaybeDead(lua_State* L) { lua_pushboolean(L, BlipFlags(L) & 0x40); return 1; }
int l_blip_GetSource(lua_State* L) {
  ReconBlip* b = RB(L);
  Unit* u = b->source;
  if (u && !u->destroyQueued && u->HasLuaObject()) PushObject(L, u);
  else lua_pushnil(L);
  return 1;
}
int l_blip_GetBlueprint(lua_State* L) {
  ReconBlip* b = RB(L);
  if (b->blueprint) S(L)->blueprints().PushTable(L, *b->blueprint);
  else lua_pushnil(L);
  return 1;
}
int l_IsBlip(lua_State* L) {
  ScriptObject* o = GetObject(L, 1);
  lua_pushboolean(L, o && dynamic_cast<ReconBlip*>(o) != nullptr);
  return 1;
}

}  // namespace

void RegisterIntelBindings(lua_State* L) {
  SetMethod(L, "Entity", "InitIntel", l_InitIntel);
  SetMethod(L, "Entity", "EnableIntel", l_EnableIntel);
  SetMethod(L, "Entity", "DisableIntel", l_DisableIntel);
  SetMethod(L, "Entity", "IsIntelEnabled", l_IsIntelEnabled);
  SetMethod(L, "Entity", "SetIntelRadius", l_SetIntelRadius);
  SetMethod(L, "Entity", "GetIntelRadius", l_GetIntelRadius);
  SetMethod(L, "Unit", "GetBlip", l_GetBlip);
  SetMethod(L, "ReconBlip", "IsOnRadar", l_blip_IsOnRadar);
  SetMethod(L, "ReconBlip", "IsOnSonar", l_blip_IsOnSonar);
  SetMethod(L, "ReconBlip", "IsOnOmni", l_blip_IsOnOmni);
  SetMethod(L, "ReconBlip", "IsSeenNow", l_blip_IsSeenNow);
  SetMethod(L, "ReconBlip", "IsSeenEver", l_blip_IsSeenEver);
  SetMethod(L, "ReconBlip", "IsKnownFake", l_blip_IsKnownFake);
  SetMethod(L, "ReconBlip", "IsMaybeDead", l_blip_IsMaybeDead);
  SetMethod(L, "ReconBlip", "GetSource", l_blip_GetSource);
  SetMethod(L, "ReconBlip", "GetBlueprint", l_blip_GetBlueprint);
  SetGlobal(L, "IsBlip", l_IsBlip);
}

}  // namespace moho
