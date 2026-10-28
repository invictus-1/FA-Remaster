// Intel grids and recon blips: see intel.h.
#include "sim/intel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/ai_threat.h"
#include "sim/combat.h"
#include "sim/enum_arg.h"
#include "sim/luautil.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {

namespace combat {
int Relation(const Army* a, const Army* b);
}

namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

struct Grid {
  int cs = 4, W = 0, H = 0;
  std::vector<int8_t> c;
  bool Visible(Vec3 p) const { return At(Cell(p.x), Cell(p.z)); }
  int Cell(float v) const { return static_cast<int>(std::floor(v * (1.0f / static_cast<float>(cs)))); }  // of x or z
  bool At(int x, int z) const {
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
}  // namespace

// The sim's intel (Sim::intelState). Multicore step 1: these were process-wide (g_armies, g_sources, g_blips,
// g_goneUnits), so two sims in one process shared them.
struct IntelState {
  std::vector<ArmyIntel> armies;
  std::vector<Entity*> sources;   // entities with intel handles
  std::vector<ReconBlip*> blips;  // live blip entities
  std::vector<Unit*> goneUnits;   // destroyed units whose blips armies may still hold
};

namespace {

IntelState& IS(Sim& sim) {
  if (!sim.intelState) sim.intelState = std::make_shared<IntelState>();
  return *sim.intelState;
}
const IntelState& IS(const Sim& sim) {
  static const IntelState none;  // (a sim whose intel was never set up: no armies; immutable)
  return sim.intelState ? *sim.intelState : none;
}

Grid* GridFor(IntelState& st, int army, int slot) {  // pos handle slots 1..5
  if (army < 0 || army >= static_cast<int>(st.armies.size())) return nullptr;
  return st.armies[static_cast<size_t>(army)].g[slot - 1].get();
}

void Viz(IntelState& st, IntelHandle& h, int slot, int delta) {
  if (!h.enabled || h.radius == 0) return;
  if (slot <= 5) {
    Grid* g = GridFor(st, h.army, slot);
    if (!g) return;
    g->Raster(h.pos, static_cast<int>(h.radius / static_cast<uint32_t>(g->cs)), delta);
  } else {  // counter fields paint into every other army's counter grid
    for (size_t a = 0; a < st.armies.size(); ++a) {
      if (static_cast<int>(a) == h.army) continue;
      Grid* g = st.armies[a].g[slot - 1].get();
      if (g) g->Raster(h.pos, static_cast<int>(h.radius / static_cast<uint32_t>(g->cs)), delta);
    }
  }
}

// The EIntel kind t's word in Entity::intelAttr, -1 = none: 1..5 (Vision..Omni) -> t - 1, 12 RadarStealth -> 5,
// 13 SonarStealth -> 6, 11 Cloak -> 7; the fields (6..8), Jammer and Spoof have none (the jump tables of
// EntityAttributes::SetIntelRadius 0x5bd470 / GetRange 0x5bd530, EnableIntel 0x68e202, DisableIntel 0x68e4a5).
int IntelAttrSlot(int t) {
  if (t >= 1 && t <= 5) return t - 1;
  if (t == 12) return 5;
  if (t == 13) return 6;
  if (t == 11) return 7;
  return -1;
}
// The exe's units always hold a CIntel (Unit::Unit 0x6a5860), so the attribute words of a unit change even when it
// has no circle of that kind; another entity needs InitIntel first ("... called before InitIntel" otherwise).
bool HasCIntel(const Entity* e) { return e->intel || dynamic_cast<const Unit*>(e) != nullptr; }
// SetIntelRadius / InitIntel's word: the radius in the low bits, the enabled bit kept (0x5bd477..: and 0x80000000,
// or)
void SetAttrRadius(Entity* e, int t, uint32_t R) {
  const int s = IntelAttrSlot(t);
  if (s >= 0) e->intelAttr[s] = (e->intelAttr[s] & 0x80000000u) | R;
}
void SetAttrEnabled(Entity* e, int t, bool on) {
  const int s = IntelAttrSlot(t);
  if (s < 0) return;
  if (on) e->intelAttr[s] |= 0x80000000u;   // 0x68e209..0x68e25d
  else e->intelAttr[s] &= 0x7fffffffu;      // 0x68e4ac..0x68e500
}

EntityIntel& IntelOf(IntelState& st, Entity* e) {
  if (!e->intel) {
    e->intel = std::make_shared<EntityIntel>();
    st.sources.push_back(e);
  }
  return *e->intel;
}

Vec3 SourcePos(const Entity* e) { return {e->position.x, e->position.y + 1, e->position.z}; }

// CIntelPosHandle::UpdatePos 0x76f1e0 (exact: CIntel::Update 0x76e4c0)
void UpdatePos(IntelState& st, IntelHandle& h, int slot, Vec3 p, uint32_t tick, bool exact) {
  if (!h.exists) return;
  if (!h.enabled) {
    h.pos = p;
    if (exact) h.lastTick = tick;
    return;
  }
  // (the new place against the handle's, memcmp 0x4f0a50: CIntel::Update 0x76e4e1..0x76e4f1, CIntelPosHandle::Update
  // 0x76efc1..0x76efd2)
  bool moved = !vm::SameBits(p, h.pos);
  if (!exact) {
    float dx = p.x - h.pos.x, dy = p.y - h.pos.y, dz = p.z - h.pos.z;
    float t = static_cast<float>(h.radius) * 0.333f;
    if (!(dx * dx + dy * dy + dz * dz >= t * t || tick - h.lastTick > 5)) return;
  }
  h.lastTick = tick;
  if (moved) {
    Viz(st, h, slot, -1);
    h.pos = p;
    Viz(st, h, slot, +1);
  }
}

int ArmyIdx(const Army* a) { return a ? a->index - 1 : -1; }

// The unit whose layer an entity has for recon: itself, or a blip's source (ReconBlip::Refresh copies the layer)
const Unit* LayerUnit(const Entity* e) {
  if (!e) return nullptr;
  if (e->kind == Entity::Kind::Unit) return static_cast<const Unit*>(e);
  if (e->kind == Entity::Kind::Blip) return static_cast<const ReconBlip*>(e)->source;
  return nullptr;
}

bool Underwater(const Entity* e) {
  const Unit* u = LayerUnit(e);
  if (!u) return false;
  return u->layer == "Seabed" || u->layer == "Sub";
}

uint8_t NewReconFor(const ArmyIntel& db, const Entity* e, Vec3 p, bool underwater) {
  uint8_t f = 0;
  if (!db.fog || !db.g[0]) f = 8;
  else if (underwater ? (db.g[1] && db.g[1]->Visible(p)) : db.g[0]->Visible(p)) f = 8;
  bool sonar = underwater;
  if (!sonar)
    if (const Unit* src = LayerUnit(e)) sonar = src->layer == "Sub" || src->layer == "Water";
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
  const auto& armies = sim.armies();
  const Army* us = armies[static_cast<size_t>(a)].get();
  // 0x5c18fd: nothing outside the playable rect (margin 0; the whole map if our army may use it, vt+0xb8)
  if (sim.map() && !sim.IsWithin(p.x, p.z, 0.0f, Sim::UseWholeMap(us))) return 0;
  if (e->army && combat::Relation(e->army, us) == 2) return 0xF;
  bool under = Underwater(e);
  const std::vector<ArmyIntel>& dbs = IS(sim).armies;
  const ArmyIntel& db = dbs[static_cast<size_t>(a)];
  uint8_t f = NewReconFor(db, e, p, under);
  for (size_t j = 0; j < armies.size() && j < dbs.size(); ++j)
    if (static_cast<int>(j) != a && combat::Relation(armies[j].get(), us) == 2)
      f |= NewReconFor(dbs[j], e, p, under);
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

// ReconBlip::ReconBlip 0x5be6e0. The blip Lua object: an instance of /lua/sim/Blip.lua's Blip. A jammer's fake
// blip sits at a random offset from its source: distance JamRadius.Min + IntRange(Max - Min), scaled by a uniform
// number, in the direction normalize(Gauss, 0, Gauss); the draws are NextUInt32, Gauss, Gauss, NextUInt32.
ReconBlip* MakeBlip(Sim& sim, lua_State* L, Unit* u, bool fake = false) {
  auto o = std::make_unique<ReconBlip>();
  ReconBlip* b = o.get();
  b->kind = Entity::Kind::Blip;
  b->source = u;
  b->mobile = u->motion.bp && u->motion.bp->mobile();
  b->blueprint = u->blueprint;
  b->army = u->army;
  b->id = sim.ReserveId(u->army, 0x3);
  b->fake = fake;
  static const bool dbgBlip = getenv("MOHO64_DEBUG_BLIP") != nullptr;  // (read once)
  if (dbgBlip)
    Logf(LogLevel::Info, "blip %u id %#x of unit %#x%s", sim.tick(), b->id, u->id, fake ? " (fake)" : "");
  if (fake && u->intel) {
    const EntityIntel& ei = *u->intel;
    const uint32_t span = ei.jamMax > ei.jamMin ? ei.jamMax - ei.jamMin : 0;
    const uint32_t d = ei.jamMin + sim.IntRange(span);
    // the vector is (second draw, 0, first draw) (0x5bea35..0x5bea54), made unit length by the normalised copy
    // 0x44f7e0 (0x5bea95: times 1 / length when the length is above 0, else zero)
    const float g1 = static_cast<float>(sim.Gauss()), g2 = static_cast<float>(sim.Gauss());
    float gx = g2, gz = g1;
    const float len = std::sqrt(gx * gx + gz * gz);
    if (len > 0.0f) {
      const float inv = 1.0f / len;
      gx = gx * inv;
      gz = inv * gz;
    } else {
      gx = gz = 0;
    }
    const float r = sim.U01() * static_cast<float>(d);
    b->offset = Vec3{gx * r, 0, gz * r};
  }
  b->position = Vec3{u->position.x + b->offset.x, u->position.y + b->offset.y, u->position.z + b->offset.z};
  b->prevPos = b->position;
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
  IS(sim).blips.push_back(b);
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
  ThreatUpdateBlipPosition(sim, a, u->blip);  // UpdateBlip 0x5c1f53
  CheckIntelEvents(sim, L, a, u->blip, old, flags);
}

void DeleteEntry(Sim& sim, lua_State* L, int a, Unit* u) {
  uint8_t old = u->recon[a] & 0x7f;
  CheckIntelEvents(sim, L, a, u->blip, old, 0);
  u->recon[a] = 0;
}

// ReconBlip::DestroyIfUnused 0x5bf6f0: kept while its source still exists and is not dead (0x5bf6f0..0x5bf727: the
// source's weak pointer, IUnit vt+0x28, blip+0x294), whether or not a Destroy is pending (Entity+0x1b9); else kept while
// some army's entry is in use, else destroyed. Only the recon database calls it (ReconTick, UpdateBlips, DeleteBlip(s),
// Flush), never the unit's own destruction.
void DestroyIfUnused(Sim& sim, ReconBlip* b) {
  Unit* u = b->source;
  if (u && !u->dead) return;
  if (u)
    for (int a = 0; a < 16; ++a)
      if (u->recon[a] & kReconInUse) return;
  if (u) u->blip = nullptr;
  b->source = nullptr;
  if (!b->destroyQueued) sim.QueueDestroy(b);
  std::vector<ReconBlip*>& blips = IS(sim).blips;
  blips.erase(std::remove(blips.begin(), blips.end(), b), blips.end());
}

// ---- a jammer's fake blips (intel.md 3.4, 3.5) ----

bool HoldsFake(const Unit* u, int a) {
  for (const ReconBlip* b : u->fakeBlips)
    if (b->rflags[a] & kReconInUse) return true;
  return false;
}

// DestroyIfUnused 0x5bf6f0 for a fake: it goes once no army holds it
void DestroyFakeIfUnused(Sim& sim, ReconBlip* b) {
  for (int k = 0; k < 16; ++k)
    if (b->rflags[k] & kReconInUse) return;
  if (Unit* u = b->source) u->fakeBlips.erase(std::remove(u->fakeBlips.begin(), u->fakeBlips.end(), b), u->fakeBlips.end());
  b->source = nullptr;
  if (!b->destroyQueued) sim.QueueDestroy(b);
  std::vector<ReconBlip*>& blips = IS(sim).blips;
  blips.erase(std::remove(blips.begin(), blips.end(), b), blips.end());
}

// army a lets go of a fake (DeleteBlip: events for what it showed, the entry cleared)
void DropFake(Sim& sim, lua_State* L, int a, ReconBlip* b) {
  uint8_t old = b->rflags[a] & 0x7f;
  CheckIntelEvents(sim, L, a, b, old, 0);
  b->rflags[a] = 0;
  DestroyFakeIfUnused(sim, b);
}

// FindOrCreateBlip 0x5c0930 for a fake: one of the unit's fakes this army does not hold yet, else a new one
ReconBlip* FindOrCreateFake(Sim& sim, lua_State* L, int a, Unit* u) {
  ReconBlip* b = nullptr;
  for (ReconBlip* f : u->fakeBlips)
    if (!(f->rflags[a] & kReconInUse)) {
      b = f;
      break;
    }
  if (!b) {
    b = MakeBlip(sim, L, u, true);
    u->fakeBlips.push_back(b);
    static const bool dbgJam = getenv("MOHO64_DEBUG_JAM") != nullptr;  // (read once)
    if (dbgJam)
      Logf(LogLevel::Info, "jamnew %u army %d unit %u %s fake %u at %.2f %.2f", sim.tick(), a, u->id,
           u->blueprint ? u->blueprint->id.c_str() : "?", b->id, b->position.x, b->position.z);
  }
  b->rflags[a] = kReconInUse;
  return b;
}

// UpdateBlip 0x5c1cf0 for a fake: it is detected at its own spot; line of sight, omni, an allied jammer or a
// spot off the playable area (shrunk by the source's footprint) give it away (KnownFake)
void UpdateFake(Sim& sim, lua_State* L, int a, ReconBlip* b) {
  uint8_t old = b->rflags[a] & 0x7f;
  // At the fake's CURRENT transform (UpdateBlip 0x5c1d9c ReconCanDetect and 0x5c1eb4 IsWithin read blip+0xac).
  // ReconBlip::Refresh 0x5bf810 (step 9; the ctor calls it too, 0x5bec49) sets the blip's pending transform to the
  // source's pending one + the offset (0x5bf842..0x5bf8d6) and its current one to the source's current one (vt+0x18)
  // + the offset (0x5bf8db..0x5bf962); the current one is the transform committed at the end of the last beat (step
  // 13). So a moving jammer's fake is looked at one beat behind, as a unit is (ReconTick below). Seen: MP1 7982, army
  // 5's frigate 0x50008e: its fake 0x305000fc leaves army 2's sight at 7992 in the original (probe mp1_8163: 7984,
  // 7986, 7990, 7992 for armies 4, 6, 0, 2), at 7982 in ours; the last loss sets FAF's ResetJammer, so ours's jammer
  // toggled a second early and made army 1's new fakes at 8161, not 8171.
  Vec3 at = b->position;
  if (const Unit* s = b->source) {
    const Vec3& c = sim.coordsPending && s->lastPosTick == sim.tick() ? s->lastPosition : s->position;
    at = Vec3{b->offset.x + c.x, b->offset.y + c.y, b->offset.z + c.z};
  }
  uint8_t flags = CanDetect(sim, a, b, at);
  flags |= old & 0x30;
  if (flags & 8) flags |= 0x30;
  const Army* us = sim.armies()[static_cast<size_t>(a)].get();
  const bool ally = b->army && combat::Relation(b->army, us) == 2;
  if (!(flags & 4) && !ally) {
    // 0x5c1ec3: STIMap::IsWithin(the blip's position, margin = the footprint's larger side, our army's flag)
    float margin = 0;
    if (b->blueprint && b->blueprint->hasFootprint)
      margin = static_cast<float>(std::max(b->blueprint->footprint.sizeX, b->blueprint->footprint.sizeZ));
    const Vec3& p = at;  // (blip+0xac, 0x5c1eb4)
    if (sim.map() && !sim.IsWithin(p.x, p.z, margin, Sim::UseWholeMap(us))) flags |= 0x20;
  } else {
    flags |= 0x20;
  }
  b->rflags[a] = static_cast<uint8_t>(kReconInUse | flags);
  ThreatUpdateBlipPosition(sim, a, b);  // UpdateBlip 0x5c1f53
  CheckIntelEvents(sim, L, a, b, old, flags);
}

// CAiReconDBImpl::ReconTick 0x5c0c40 for army a
void ReconTick(Sim& sim, lua_State* L, int a) {
  // blips whose unit is gone: mobile ones go, structures stay (MaybeDead) until seen again
  std::vector<Unit*>& goneUnits = IS(sim).goneUnits;
  for (size_t i = 0; i < goneUnits.size(); ++i) {
    Unit* u = goneUnits[i];
    {
      const std::vector<ReconBlip*> fakes = u->fakeBlips;  // (a gone jammer's fakes go with it)
      for (ReconBlip* f : fakes)
        if (f->rflags[a] & kReconInUse) DropFake(sim, L, a, f);
    }
    if (!(u->recon[a] & kReconInUse)) continue;
    ReconBlip* b = u->blip;
    bool mobile = b ? b->mobile : true;
    bool allied = u->army && combat::Relation(u->army, sim.armies()[static_cast<size_t>(a)].get()) == 2;
    Vec3 p = b ? b->position : u->position;
    if (mobile || allied || (CanDetect(sim, a, b ? static_cast<Entity*>(b) : u, p) & 8)) {
      if (!mobile && b) ThreatRemoveEntry(sim, a, b->id);  // a structure confirmed gone (0x5c0da6 / 0x5c0f91)
      DeleteEntry(sim, L, a, u);
      if (b) DestroyIfUnused(sim, b);
    } else {
      u->recon[a] |= 0x40;
    }
  }
  // ... and the units whose Destroy is pending but not yet carried out (the map loop's test 0x5c0e75: the source's
  // Entity+0x1b9, set by Entity::Destroy 0x679b0f): the same gone path, now. Their DestroyIfUnused keeps the blip (the
  // source still exists and is not dead), and nothing asks again once the unit is gone: the blip and its id stay.
  // Seen (probe mp3_dyunused): MP3, army 1's Cybran build drone 0x100002 (blip 0x30100002) is destroyed by FAF's
  // ReturnBotsThread in beat 1383, which is army 3's recon beat: the original's army 3 lets the entry go then, the
  // blip is kept and its id never comes back, so every later army-1 blip id is one higher than ours; army 0's Attack
  // at 2791 on 0x30100021 is then on the interceptor 0x100020, not the Mantis 0x100021.
  {
    const Army* us = sim.armies()[static_cast<size_t>(a)].get();
    for (const auto& army : sim.armies()) {
      if (army.get() == us) continue;
      const std::vector<Unit*> list = army->units;
      for (Unit* u : list) {
        if (!u->destroyQueued || !(u->recon[a] & kReconInUse)) continue;
        ReconBlip* b = u->blip;
        bool mobile = b ? b->mobile : true;
        bool allied = u->army && combat::Relation(u->army, us) == 2;
        Vec3 p = b ? b->position : u->position;
        if (mobile || allied || (CanDetect(sim, a, b ? static_cast<Entity*>(b) : u, p) & 8)) {
          if (!mobile && b) ThreatRemoveEntry(sim, a, b->id);
          DeleteEntry(sim, L, a, u);
          if (b) DestroyIfUnused(sim, b);
        } else {
          u->recon[a] |= 0x40;
        }
      }
    }
  }
  goneUnits.erase(std::remove_if(goneUnits.begin(), goneUnits.end(),
                                   [](Unit* u) {
                                     for (int k = 0; k < 16; ++k)
                                       if ((u->recon[k] & kReconInUse) || HoldsFake(u, k)) return false;
                                     return true;
                                   }),
                  goneUnits.end());
  // every unit of every other army
  struct Pending {
    Unit* u;
    uint8_t flags;
    bool fake;
  };
  std::vector<Pending> pending;
  // UpdateBlips 0x5c1f80: the unit's real blip, then its fakes: as many as the jammer makes are kept, the rest
  // dropped, and the missing ones are queued as new blips
  auto updateBlips = [&](Unit* u, uint8_t f) {
    if (u->recon[a] & kReconInUse) UpdateBlip(sim, L, a, u, f);
    const uint32_t jam = FlagOn(u, 9) ? u->intel->jammerBlips : 0;
    uint32_t kept = 0;
    const std::vector<ReconBlip*> fakes = u->fakeBlips;
    for (ReconBlip* b : fakes) {
      if (!(b->rflags[a] & kReconInUse)) continue;
      if (kept < jam) {
        UpdateFake(sim, L, a, b);
        ++kept;
      } else {
        DropFake(sim, L, a, b);
      }
    }
    for (uint32_t i = kept; i < jam; ++i) pending.push_back({u, f, true});
  };
  const Army* us = sim.armies()[static_cast<size_t>(a)].get();
  const std::vector<bool>& visibleToRecon = BpCategoryMask(sim, "VISIBLETORECON");  // (BpInCategory, once)
  for (const auto& army : sim.armies()) {
    if (army.get() == us) continue;
    std::vector<Unit*> list = army->units;
    for (Unit* u : list) {
      if (u->destroyQueued) continue;
      const BlueprintInfo* ubp = u->blueprint;
      if (!ubp || ubp->entityIndex < 0 || static_cast<size_t>(ubp->entityIndex) >= visibleToRecon.size() ||
          !visibleToRecon[static_cast<size_t>(ubp->entityIndex)])
        continue;
      // ReconCanDetect at IUnit::GetPosition (0x5c10cb -> 0x6a49b0, entity+0xac): the transform committed at the end of
      // the last beat. Recon runs before Entity::AdvanceCoords (step 13), so a unit that has had its turn of this
      // beat's motion is looked at where it stood when the beat began. Seen: MP2 2305, army 0's Cybran build drone
      // 0x2a crosses into army 1's radar between 2304 and 2305; the original's army 1 sees it only at 2309, with the
      // engineer beside it, which then holds the lower blip id (0x30000005) that army 1's orders at 2380 name.
      const Vec3& at = sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
      uint8_t f = CanDetect(sim, a, u, at);
      bool inUse = (u->recon[a] & kReconInUse) != 0;
      if (f && !inUse) {
        pending.push_back({u, f, false});
      } else if (f) {
        static const bool dbgJam = getenv("MOHO64_DEBUG_JAM") != nullptr;  // (read once)
        if (dbgJam && FlagOn(u, 9))
          Logf(LogLevel::Info, "jamdbg %u army %d unit %u flags %d", sim.tick(), a, u->id, f);
        updateBlips(u, f);
      } else if (inUse) {
        bool mobile = u->motion.bp && u->motion.bp->mobile();
        if ((u->recon[a] & 0x10) && !mobile) updateBlips(u, 0);
        else {
          DeleteEntry(sim, L, a, u);
          if (u->blip) DestroyIfUnused(sim, u->blip);
          const std::vector<ReconBlip*> fakes = u->fakeBlips;
          for (ReconBlip* b : fakes)
            if (b->rflags[a] & kReconInUse) DropFake(sim, L, a, b);
        }
      }
    }
  }
  // GenerateNewBlips 0x5c0a70
  for (const Pending& p : pending) {
    Unit* u = p.u;
    if (u->destroyQueued) continue;
    if (p.fake) {
      ReconBlip* fake = FindOrCreateFake(sim, L, a, u);
      UpdateFake(sim, L, a, fake);
      ThreatInsertEntry(sim, a, fake);  // 0x5c0bd0
      if (u->HasLuaObject()) {
        lua_pushnumber(L, a + 1);
        sim.CallMethod(L, u, "OnDetectedBy", 1);
      }
      continue;
    }
    if (!u->blip) u->blip = MakeBlip(sim, L, u);
    u->recon[a] = kReconInUse;
    UpdateBlip(sim, L, a, u, p.flags);
    ThreatInsertEntry(sim, a, u->blip);  // GenerateNewBlips 0x5c0bd0: the army's threat map gets the blip
    if (u->HasLuaObject()) {
      lua_pushnumber(L, a + 1);
      sim.CallMethod(L, u, "OnDetectedBy", 1);
    }
  }
}

}  // namespace

bool ArmyHasBlip(const Unit* u, int a) { return a >= 0 && a < 16 && (u->recon[a] & kReconInUse) != 0; }

namespace {
// What one army's grids say of a point (ReconAtPoint's rules below; vis(grid): the grid is set there).
template <class Vis>
uint8_t GridsSay(const ArmyIntel& db, int water, const Vis& vis) {
  uint8_t f = 0;
  if (!db.fog || !db.g[0]) f = 8;
  else if (water == 2 ? (db.g[1] && vis(*db.g[1])) : vis(*db.g[0])) f = 8;
  if (water != 0 && db.g[3] && vis(*db.g[3])) f |= 2;
  if (water != 2 && db.g[2] && vis(*db.g[2])) f |= 1;
  if (db.g[4] && vis(*db.g[4])) f |= 4;
  return f;
}
// ... and what the army's own counter-intel fields take from f (its own and its allies' bits together)
template <class Vis>
uint8_t CounterIntel(const ArmyIntel& db, uint8_t f, const Vis& vis) {
  if (f == 0 || (f & 4)) return f;
  if (db.g[5] && vis(*db.g[5])) f &= static_cast<uint8_t>(~1);
  if (db.g[6] && vis(*db.g[6])) f &= static_cast<uint8_t>(~2);
  if (db.g[7] && vis(*db.g[7])) f &= static_cast<uint8_t>(~8);
  return f;
}
}  // namespace

// For a host: what army a's grids (its own and its allies', with its counter-intel fields) say of a point now.
// The grid rules of CanDetect without an entity: `water` 0 = something on land or in the air, 1 = on the water's
// surface (sonar counts too), 2 = under water (water vision instead of vision, sonar, no radar).
uint32_t ReconAtPoint(const Sim& sim, int a, Vec3 p, int water) {
  const auto& armies = sim.armies();
  const std::vector<ArmyIntel>& dbs = IS(sim).armies;
  if (a < 0 || static_cast<size_t>(a) >= armies.size() || static_cast<size_t>(a) >= dbs.size()) return 0;
  const TerrainMap* m = sim.map();
  if (m && (p.x < 0 || p.z < 0 || p.x > m->width() || p.z > m->height())) return 0;
  auto vis = [&](const Grid& g) { return g.Visible(p); };
  const Army* us = armies[static_cast<size_t>(a)].get();
  const ArmyIntel& db = dbs[static_cast<size_t>(a)];
  uint8_t f = GridsSay(db, water, vis);
  for (size_t j = 0; j < armies.size() && j < dbs.size(); ++j)
    if (static_cast<int>(j) != a && combat::Relation(armies[j].get(), us) == 2) f |= GridsSay(dbs[j], water, vis);
  return CounterIntel(db, f, vis);
}

// ReconAtPoint at the centre of every cell of a w x h grid over the map, as w x h calls would give it (sim_view.h
// ReconGrid; render REQUESTS 38: the picture's sight grid, 65,536 calls every fifth beat). The same rules on the
// same floats, taken grid by grid instead of point by point: the army's allies are worked out once, each intel
// grid's cell of every column and row once (Grid::Cell of the same x or z that Visible takes), and each grid then
// adds its bit to the cells where it is set. The bits are ORed over the armies as ReconAtPoint ORs them, and the
// counter-intel fields clear theirs only where the result is neither 0 nor on omni, as there.
void ReconGridPoints(const Sim& sim, int a, int w, int h, int water, std::vector<uint8_t>* out) {
  out->clear();
  const TerrainMap* m = sim.map();
  if (!m || w < 1 || h < 1) return;
  const size_t uw = static_cast<size_t>(w), n = uw * static_cast<size_t>(h);
  out->assign(n, 0);
  const auto& armies = sim.armies();
  const std::vector<ArmyIntel>& dbs = IS(sim).armies;
  if (a < 0 || static_cast<size_t>(a) >= armies.size() || static_cast<size_t>(a) >= dbs.size()) return;
  const Army* us = armies[static_cast<size_t>(a)].get();
  const ArmyIntel& db = dbs[static_cast<size_t>(a)];
  std::vector<const ArmyIntel*> seeing{&db};  // the army, then its allies in army order
  for (size_t j = 0; j < armies.size() && j < dbs.size(); ++j)
    if (static_cast<int>(j) != a && combat::Relation(armies[j].get(), us) == 2) seeing.push_back(&dbs[j]);
  const float mw = static_cast<float>(m->width()), mh = static_cast<float>(m->height());
  std::vector<float> px(uw), pz(static_cast<size_t>(h));
  for (int x = 0; x < w; ++x) px[static_cast<size_t>(x)] = (static_cast<float>(x) + 0.5f) * mw / static_cast<float>(w);
  for (int z = 0; z < h; ++z) pz[static_cast<size_t>(z)] = (static_cast<float>(z) + 0.5f) * mh / static_cast<float>(h);
  // each column's and row's cell, per cell size (a game's grids share one or two sizes)
  struct Cells {
    std::vector<int> x, z;
  };
  std::map<int, Cells> bySize;
  auto cellsOf = [&](const Grid& g) -> const Cells& {
    auto it = bySize.find(g.cs);
    if (it != bySize.end()) return it->second;
    Cells& c = bySize[g.cs];
    for (float v : px) c.x.push_back(g.Cell(v));
    for (float v : pz) c.z.push_back(g.Cell(v));
    return c;
  };
  uint8_t* o = out->data();
  auto set = [&](const Grid& g, uint8_t bit) {  // bit into every cell where g is set
    const Cells& c = cellsOf(g);
    for (size_t z = 0; z < static_cast<size_t>(h); ++z)
      for (size_t x = 0; x < uw; ++x)
        if (g.At(c.x[x], c.z[z])) o[z * uw + x] |= bit;
  };
  for (const ArmyIntel* d : seeing) {  // GridsSay, a grid at a time
    if (!d->fog || !d->g[0]) {
      for (size_t i = 0; i < n; ++i) o[i] |= 8;
    } else if (water == 2) {
      if (d->g[1]) set(*d->g[1], 8);
    } else {
      set(*d->g[0], 8);
    }
    if (water != 0 && d->g[3]) set(*d->g[3], 2);
    if (water != 2 && d->g[2]) set(*d->g[2], 1);
    if (d->g[4]) set(*d->g[4], 4);
  }
  for (int k = 5; k <= 7; ++k) {  // CounterIntel: RCI takes radar, SCI sonar, VCI vision
    if (!db.g[k]) continue;
    const Grid& g = *db.g[k];
    const Cells& c = cellsOf(g);
    const uint8_t keep = static_cast<uint8_t>(~(k == 5 ? 1 : k == 6 ? 2 : 8));
    for (size_t z = 0; z < static_cast<size_t>(h); ++z)
      for (size_t x = 0; x < uw; ++x) {
        uint8_t& f = o[z * uw + x];
        if (f != 0 && !(f & 4) && g.At(c.x[x], c.z[z])) f &= keep;
      }
  }
  for (size_t z = 0; z < static_cast<size_t>(h); ++z)  // (0 outside the map, as ReconAtPoint)
    for (size_t x = 0; x < uw; ++x)
      if (pz[z] < 0 || px[x] < 0 || px[x] > m->width() || pz[z] > m->height()) o[z * uw + x] = 0;
}

uint32_t ReconOfEntity(const Sim& sim, int a, const Entity* e) {
  const auto& armies = sim.armies();
  if (!e || a < 0 || static_cast<size_t>(a) >= armies.size() || a >= 16) return 0;
  const Army* us = armies[static_cast<size_t>(a)].get();
  if (e->army && (e->army == us || combat::Relation(e->army, us) == 2)) return 0xF;
  if (e->kind == Entity::Kind::Unit) return static_cast<const Unit*>(e)->recon[a];
  return ReconAtPoint(sim, a, e->position, 0);
}

void InitIntelGrids(Sim& sim, lua_State* L) {
  IntelState& st = IS(sim);
  st.armies.clear();
  st.sources.clear();
  st.blips.clear();
  st.goneUnits.clear();
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
  st.armies.resize(sim.armies().size());
  for (auto& ai : st.armies) {
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
    // EntityAttributes::Initialize 0x6a46a0 (Unit::Unit 0x6a5786): the words from the blueprint's Vision ..
    // Omni radii (Intel+0x00..+0x10) and the three field radii (Intel+0x18..+0x20), enabled when not 0
    u->intelAttr[s - 1] = R != 0 ? (R | 0x80000000u) : 0u;
    if (R == 0) continue;
    if (!ei) ei = &IntelOf(IS(sim), u);
    IntelHandle& h = ei->h[s];
    h.exists = true;
    h.radius = R;
    h.army = ArmyIdx(u->army);
    h.pos = p;
  }
  bool rs = lu::Bool(L, intel, "RadarStealth"), ss = lu::Bool(L, intel, "SonarStealth"), cl = lu::Bool(L, intel, "Cloak");
  float jb = lu::Num(L, intel, "JammerBlips", 0);
  int jr = lu::Sub(L, intel, "JamRadius");
  float jmax = lu::Num(L, jr, "Max", 0), jmin = lu::Num(L, jr, "Min", 0);
  lua_pop(L, 1);
  int sr = lu::Sub(L, intel, "SpoofRadius");
  float smax = lu::Num(L, sr, "Max", 0);
  if (rs || ss || cl || (jb > 0 && jmax > 0) || smax > 0) {
    EntityIntel& e = IntelOf(IS(sim), u);
    e.has[12] = rs;
    e.has[13] = ss;
    e.has[11] = cl;
    e.has[9] = jb > 0 && jmax > 0;
    e.has[10] = smax > 0;
    e.jammerBlips = jb > 0 ? static_cast<uint32_t>(jb) : 0;
    e.jamMin = jmin > 0 ? static_cast<uint32_t>(jmin) : 0;
    e.jamMax = jmax > 0 ? static_cast<uint32_t>(jmax) : 0;
  }
  lua_settop(L, top);
}

void ReleaseEntityIntel(Sim& sim, Entity* e) {
  IntelState& st = IS(sim);
  if (e->intel) {
    for (int s = 1; s <= 8; ++s) Viz(st, e->intel->h[s], s, -1);
    e->intel->h[0] = IntelHandle{};
    for (int s = 1; s <= 8; ++s) e->intel->h[s].enabled = false;
    st.sources.erase(std::remove(st.sources.begin(), st.sources.end(), e), st.sources.end());
  }
  if (e->kind == Entity::Kind::Unit) {
    Unit* u = static_cast<Unit*>(e);
    bool any = !u->fakeBlips.empty();
    for (int a = 0; a < 16; ++a) any = any || (u->recon[a] & kReconInUse);
    if (any) {
      st.goneUnits.push_back(u);
    } else if (u->blip) {
      // (no army holds it: nothing destroys it any more; its weak pointer to the unit goes empty)
      u->blip->source = nullptr;
      u->blip = nullptr;
    }
  }
}

void ReconBeat(Sim& sim) {
  lua_State* L = sim.L();
  if (IS(sim).armies.size() != sim.armies().size()) InitIntelGrids(sim, L);
  // every blip follows its unit (ReconBlip::Refresh 0x5bf810)
  for (ReconBlip* b : IS(sim).blips) {
    Unit* u = b->source;
    if (!u || u->destroyQueued) continue;
    // (current := the unit's committed transform, pending := its pending one; AdvanceCoords then makes the first the
    // previous: so prevPos is where the unit stood when this beat began)
    const Vec3& from = u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
    b->prevPos = Vec3{from.x + b->offset.x, from.y + b->offset.y, from.z + b->offset.z};
    b->position = Vec3{u->position.x + b->offset.x, u->position.y + b->offset.y, u->position.z + b->offset.z};
    b->orientation = u->orientation;
    b->health = u->health;
    b->maxHealth = u->maxHealth;
  }
  int n = static_cast<int>(sim.armies().size());
  if (n <= 0) return;
  ReconTick(sim, L, static_cast<int>(sim.tick() % static_cast<uint32_t>(n)));
}

// Entity::UpdateIntel 0x679120 -> CIntel::Update 0x76e4c0 (read): at the entity's current transform (+0xac, the one
// committed at the end of the last beat; 1 up), every handle: an enabled one whose position differs re-rasters
// there, a disabled one only stores it, and all get lastTick = the tick, which sets the phase of their 6-tick
// clock (UpdatePos 0x76f1e0). Its callers (SetMotionHorzEvent 0x6b8f65, NotifyAttached 0x6b9501,
// ProcessCommonMotionState 0x6c2a8b) run it when the horizontal event has just turned to Stopped, after the Lua
// callback, if the event is still Stopped then. Seen (oracle probe mp2_2972): MP2, army 0's air scout 0x4 turns
// Stopped at 2723 and 2751 while it still drifts (its radar's lastTick 2723, 2751, then 2757 .. 2967); ours had
// no such update for an aircraft and kept the phase of its first flight (2681 .. 2969), so at the recon tick 2972
// its radar (16 cells) ended one column short of army 1's Striker 0x10003b, whose blip 0x3010000c the original
// makes there; every later id of army 1's blips was one lower in ours (MP2 3679: an Attack order on 0x3010000f
// named another unit).
void IntelExactUpdate(Sim& sim, Unit* u) {
  if (!u || !u->intel) return;
  const Vec3& c = sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
  const Vec3 p{c.x, c.y + 1, c.z};
  for (int s = 1; s <= 8; ++s) UpdatePos(IS(sim), u->intel->h[s], s, p, sim.tick(), true);
}

void AdvanceIntelCoords(Sim& sim) {
  uint32_t tick = sim.tick();
  IntelState& st = IS(sim);
  for (size_t i = 0; i < st.sources.size(); ++i) {
    Entity* e = st.sources[i];
    if (!e->intel || e->destroyQueued) continue;
    EntityIntel& ei = *e->intel;
    Vec3 p = SourcePos(e);
    bool exact = false, moved;
    if (e->kind == Entity::Kind::Unit) {
      // (coming to rest: the exact update ran when the horizontal event turned Stopped, IntelExactUpdate)
      // Sim::AdvanceBeat (0x74a239..0x74a25f) runs Entity::AdvanceCoords 0x678f10 for every entity on the dirty-entity
      // list sim+0xa5c, and AdvanceCoords runs CIntel::ForceUpdate 0x76e490 for every entity with intel (+0x1d8,
      // 0x67907e..0x6790cb) whether its transform changed or not. The list keeps its links from beat to beat (a unit is
      // linked when it is made; probe mp2_8034: no unlink in the oracle's beats; see GridAdvanceCoords), so every unit
      // has its ForceUpdate -> UpdatePos in every beat: the 6-tick clock (lastTick) runs while the unit stands, is being
      // built, is attached, immobile or stunned alike (the circle itself only moves when the position differs). Seen:
      // MP2 7929-7949, army 1's Pillar 0x100123 stunned (position held; the exact update when it came to rest at 7936):
      // the original's clock ran on through the stun (7936 + 6k: ... 8362, 8368), so the raster at 8368 reached army 0's
      // mass extractor 0x48 and army 1 made its blip at the recon beat 8369 (probe mp2_8374: FindOrCreateBlip at 8369);
      // the Pillar took it at 8370 and fired at 8374. Ours stopped the clock during the stun and restarted it at 7949
      // (8363, 8369): the blip came at 8373 and the Pillar fired later (the draws split at 8374). (Earlier cases that
      // the same rule covers: MP2 2186, a land scout standing from 2073 whose radar clock ran on; MP3 2365, an
      // interceptor built in an air factory whose vision clock ran from 2365.)
      moved = true;
    } else {
      IntelHandle* any = nullptr;
      for (int s = 1; s <= 8; ++s)
        if (ei.h[s].exists) any = &ei.h[s];
      moved = any && !vm::SameBits(any->pos, p);
      exact = moved;  // script entities move by Warp / attach: exact
    }
    if (!moved && !exact) continue;
    for (int s = 1; s <= 8; ++s) UpdatePos(st, ei.h[s], s, p, tick, exact);
  }
}

// ---- Lua ----------------------------------------------------------------------------------------

namespace {

// CAiReconDBImpl::ReconFlushBlipsInRect 0x5c05a0 for army a: the blips whose position lies inside the rect, edges
// included (none of x0 > x, x > x1, z0 > z, z > z1 with the ints as floats), are deleted as DeleteBlip 0x5c21f0
// deletes them (CheckIntelEvents to nothing, the entry cleared, ReconBlip::DestroyIfUnused): first the orphans
// (db+0x24: a structure's blip remembered after its unit died, MaybeDead), then the map's entries whose source still
// exists (blip+0x270; one whose unit is gone and not yet looked at by this army's recon tick is skipped). The army
// sees what is still there again at its next recon tick (new blips, OnDetectedBy).
void FlushBlipsInRect(Sim& sim, lua_State* L, int a, const int r[4]) {
  auto inRect = [&](const Vec3& p) {
    return !(static_cast<float>(r[0]) > p.x) && !(p.x > static_cast<float>(r[2])) &&
           !(static_cast<float>(r[1]) > p.z) && !(p.z > static_cast<float>(r[3]));
  };
  const std::vector<Unit*> gone = IS(sim).goneUnits;
  for (Unit* u : gone) {
    if ((u->recon[a] & (kReconInUse | 0x40)) != (kReconInUse | 0x40)) continue;
    ReconBlip* b = u->blip;
    if (!inRect(b ? b->position : u->position)) continue;
    DeleteEntry(sim, L, a, u);
    if (b) DestroyIfUnused(sim, b);
  }
  const Army* us = sim.armies()[static_cast<size_t>(a)].get();
  for (const auto& army : sim.armies()) {
    if (army.get() == us) continue;
    const std::vector<Unit*> list = army->units;
    for (Unit* u : list) {
      if ((u->recon[a] & kReconInUse) && u->blip && inRect(u->blip->position)) {
        ReconBlip* b = u->blip;
        DeleteEntry(sim, L, a, u);
        DestroyIfUnused(sim, b);
      }
      const std::vector<ReconBlip*> fakes = u->fakeBlips;
      for (ReconBlip* f : fakes)
        if ((f->rflags[a] & kReconInUse) && inRect(f->position)) DropFake(sim, L, a, f);
    }
  }
}

// FlushIntelInRect(minX, minZ, maxX, maxZ) 0x75ca50: four integers (truncated), then every army's recon database
// in army order (sim+0x910, army vt+0x20 GetReconDB, its vt+0x54) flushes the rect
int l_FlushIntelInRect(lua_State* L) {
  if (lua_gettop(L) != 4)
    luaL_error(L, "%s\n  expected %d args, but got %d", "FlushIntelInRect( minX, minZ, maxX, maxZ )", 4, lua_gettop(L));
  int v[5] = {0, 0, 0, 0, 0};
  for (int i = 4; i >= 1; --i) {  // (read 4, 3, 2, 1)
    if (lua_type(L, i) != LUA_TNUMBER) luaL_typerror(L, i, "integer");
    const double d = lua_tonumber(L, i);
    v[i] = d > -2147483649.0 && d < 2147483648.0 ? static_cast<int>(d) : static_cast<int>(0x80000000u);
  }
  const int r[4] = {v[1], v[2], v[3], v[4]};
  Sim& sim = *S(L);
  if (IS(sim).armies.size() != sim.armies().size()) return 0;
  for (size_t a = 0; a < sim.armies().size() && a < 16; ++a) FlushBlipsInRect(sim, L, static_cast<int>(a), r);
  return 0;
}

// A kind the bindings act on: the exe indexes its circles and flags with whatever value the parse gave (0x68e1c6..),
// out of 0..13 that is memory past its tables; ours does nothing for those
bool KindOk(int t) { return t >= 0 && t <= 13; }
// EIntel by name (sim/enum_arg.h: no case, "INTEL_", numbers, '|'). FAF's XRB3301 asks for 'vision'.
int TypeArg(lua_State* L, int idx) { return EnumValue(L, kEnumIntel, luaL_checkstring(L, idx)); }

// InitIntel(army, type, [radius]) 0x68ea20
int l_InitIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  Sim& sim = *S(L);
  Army* army = sim.GetArmy(L, 2);
  int t = TypeArg(L, 3);
  if (!KindOk(t)) return 0;
  EntityIntel& ei = IntelOf(IS(sim), e);
  uint32_t R = 0;
  if (lua_isnumber(L, 4)) {
    double r = lua_tonumber(L, 4);
    R = r > 0 ? static_cast<uint32_t>(r) : 0;
  }
  if (t >= 1 && t <= 8 && lua_isnumber(L, 4) && R == 0) return luaL_error(L, "InitIntel: radius rounds to 0");
  SetAttrRadius(e, t, R);  // 0x68ece3..0x68ecf9, for every kind (the view's words only)
  if (t >= 1 && t <= 8) {
    IntelHandle& h = ei.h[t];
    Viz(IS(*S(L)), h, t, -1);
    h = IntelHandle{};  // (the handle ctor 0x76d810: at 0, 0, 0, lastTick 0, disabled)
    h.exists = true;
    h.radius = R;
    h.army = ArmyIdx(army);
  } else if (t == 9 || t == 11 || t == 12 || t == 13) {
    ei.has[t] = true;
  } else {
    Logf(LogLevel::Warning, "Unknown intel type %i", t);
  }
  // 0x68ecaf..0x68ece1: then every handle of the entity (the new one too) takes UpdatePos 0x76f1e0 (the 6-tick one,
  // not the exact one) at the entity's COMMITTED place, +0xac as it is (no 1 up, unlike AdvanceCoords' ForceUpdate):
  // the new handle, disabled, only stores it; an enabled one re-rasters if it moved far enough or its clock ran out.
  // The committed place is where the beat before left the entity; a beam's moves with its weapon's muzzle only at
  // the beat's end. Seen: MP1 10501, army 4's Seraphim destroyer 0x40004e's BackTurret beam hits an army-1 unit,
  // FAF's CollisionBeam.ShowBeamSource gives army 1 a radius-2 vision circle there (InitIntel + EnableIntel) each
  // check; ours put it at the muzzle of this beat (179.83, 328.97: vision cell (89, 164), 0x40004e's own), not the
  // committed one of 10500 (180.34, 329.05: cell (90, 164)), so army 1's recon beat 10501 made a blip of 0x40004e,
  // army 1's destroyer 0x10002e listed it at 10508 (GetBlipsInRange) and its FrontTurret took it at 10514. The
  // original's rebuild at 10508 (probe mp1_10514b: same tick, same reach 78.75) listed only 0x2000e0, 0x200127.
  {
    Vec3 c = e->shapeHeld ? e->shapePos : e->position;
    if (e->kind == Entity::Kind::Unit) {
      const Unit* u = static_cast<const Unit*>(e);
      c = sim.coordsPending && u->lastPosTick == sim.tick() ? u->lastPosition : u->position;
    }
    for (int s = 1; s <= 8; ++s) UpdatePos(IS(sim), ei.h[s], s, c, sim.tick(), false);
  }
  return 0;
}
int l_EnableIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  if (!KindOk(t)) return 0;
  if (HasCIntel(e)) SetAttrEnabled(e, t, true);
  if (!e->intel) return 0;
  EntityIntel& ei = *e->intel;
  if (t <= 8) {
    IntelHandle& h = ei.h[t];
    if (h.exists && !h.enabled) {
      h.enabled = true;
      Viz(IS(*S(L)), h, t, +1);
    }
  } else if (ei.has[t]) {
    ei.on[t] = true;
  }
  return 0;
}
int l_DisableIntel(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  if (!KindOk(t)) return 0;
  if (HasCIntel(e)) SetAttrEnabled(e, t, false);
  if (!e->intel) return 0;
  EntityIntel& ei = *e->intel;
  if (t <= 8) {
    IntelHandle& h = ei.h[t];
    if (h.exists && h.enabled) {
      Viz(IS(*S(L)), h, t, -1);
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
  if (e->intel && KindOk(t)) on = t <= 8 ? e->intel->h[t].enabled : (e->intel->has[t] && e->intel->on[t]);
  lua_pushboolean(L, on);
  return 1;
}
int l_SetIntelRadius(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  if (!KindOk(t)) return 0;
  double r = luaL_checknumber(L, 3);
  uint32_t R = r > 0 ? static_cast<uint32_t>(r) : 0;
  if (HasCIntel(e)) SetAttrRadius(e, t, R);  // 0x68e76a..0x68e77a, with or without a circle of that kind
  if (!e->intel || t > 8) return 0;
  IntelHandle& h = e->intel->h[t];
  if (!h.exists || h.radius == R) return 0;
  Viz(IS(*S(L)), h, t, -1);
  h.radius = R;
  Viz(IS(*S(L)), h, t, +1);
  return 0;
}
int l_GetIntelRadius(lua_State* L) {
  Entity* e = CheckObject<Entity>(L, 1);
  int t = TypeArg(L, 2);
  // the attribute word, not the circle (0x68e94e: EntityAttributes::GetRange 0x5bd530, radius & 0x7fffffff, as an
  // int): 'RadarStealth' gives the radar stealth field's radius, 'RadarStealthField' 0, and a kind the unit has no
  // circle of still its word (the exe raises "GetIntelRadius called before InitIntel" for an entity without intel)
  const int s = HasCIntel(e) ? IntelAttrSlot(t) : -1;
  lua_pushnumber(L, s >= 0 ? static_cast<float>(static_cast<int>(e->intelAttr[s] & 0x7fffffffu)) : 0.0f);
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
  if (a < 0 || a >= 16) return 0;
  if (b->fake) return b->rflags[a] & 0x7f;
  if (!u) return 0;
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
  SetGlobal(L, "FlushIntelInRect", l_FlushIntelInRect);
}

}  // namespace moho
