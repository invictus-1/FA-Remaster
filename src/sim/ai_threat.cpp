// The armies' threat maps (CInfluenceMap) and the CAiBrain methods that read or write them: see ai_threat.h.
//
// Read in the FA exe (2026-10-15), with the addresses:
//   CInfluenceMap ctor 0x716140, VectorToCoords 0x715c60, InsertEntry 0x715d10, RemoveEntry 0x715eb0,
//   UpdateBlipPosition 0x715f30, GetThreatRect 0x715ff0, Update 0x716480, AssignThreatAtPosition 0x716b00,
//   GetThreatAtPosition 0x716e30, GetThreatBetweenPositions 0x716e60 (+ InitGridTraversalLine 0x40d860),
//   GetHighestThreatPosition 0x716fc0, GetThreatsAroundPosition 0x7171d0 (+ its std::sort 0x71e200);
//   InfluenceGrid ctor 0x715030, DecayInfluence 0x715130, GetThreat 0x715750 (jump table 0x715b80);
//   the Lua functions cfunc_CAiBrain*L 0x590000 (Assign), 0x5902e0 (GetThreatAtPosition), 0x590630 (Between),
//   0x590950 (Highest), 0x590c80 (Around); the threat type names EThreatTypeTypeInfo::AddEnums 0x715590 and
//   their parsing REnumType::SetLexical 0x8d9670 (+ func_ParseNum 0x8d8fa0).
// Inferred (not read) is marked as such.
#include "sim/ai_threat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "script/script_state.h"
#include "sim/combat.h"
#include "sim/enum_arg.h"
#include "sim/luautil.h"
#include "sim/motion.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {

namespace combat {
int Relation(const Army* a, const Army* b);
int LayerBit(const std::string& l);
bool TerrainSegmentHit(const TerrainMap* map, Vec3 p0, Vec3 p1, float* dist, Vec3* hit);
}  // namespace combat

namespace {

Sim* S(lua_State* L) { return Sim::From(L); }

// SThreat (14 floats, 0x38 bytes). Its slot k holds EThreatType k + 1 for k >= 1; slot 0 is Overall.
enum : int {
  kOverall = 0, kStructures, kStructuresNotMex, kNaval, kLand, kAir, kExperimental, kCommander, kArtillery,
  kAntiAir, kAntiSurface, kAntiSub, kEconomy, kUnknown, kSlots
};
struct SThreat {
  float f[kSlots] = {};
};

// InfluenceMapEntry (0x2c bytes; the value of the cell's map, keyed by the blip's entity id): one blip the army
// has seen in this cell.
struct Entry {
  uint32_t id = 0;     // +0x00 the blip's entity id
  int army = -1;       // +0x04 the army in the id's army byte (IArmy*; -1: none)
  Vec3 pos;            // +0x08 where it was put in (not read by the update or the queries)
  const BlueprintInfo* bp = nullptr;  // +0x14 the blip's blueprint (its source's)
  int layer = 0;       // +0x18 the blip's layer bit, refreshed by each update while the blip exists
  bool identified = false;  // +0x1c once the army had it on omni or in sight: its threat counts by kind
  float threat = 1;    // +0x20 its weight: 1 when seen, fading out after `counter` updates without a sighting
  float decay = 0;     // +0x24 per update: 0.02 (0xe4f8dc) for mobile units, 0 for structures
  int counter = 10;    // +0x28 updates left at full weight
};

// InfluenceGrid (0x8c bytes): one cell.
struct Cell {
  std::map<uint32_t, Entry> entries;  // +0x04
  std::vector<SThreat> perArmy;       // +0x10: the threat each army's blips in the cell pose (made by the update)
  SThreat assigned;                   // +0x1c: what the scripts assigned (AssignThreatAtPosition)
  SThreat assignedDecay;              // +0x54: per update
};

// CInfluenceMap (0x30 bytes).
struct InfluenceMap {
  int army = -1;                  // +0x00 (0-based index)
  int W = 0, H = 0, cs = 32;      // +0x08, +0x0c, +0x10
  std::map<uint32_t, int> cellOf;  // +0x14: blip id -> cell index
  std::vector<Cell> cells;        // +0x20: cz * W + cx
};

// What the update reads of a blueprint: RUnitBlueprint Defense (+0x420) AirThreatLevel +0x42c, SurfaceThreatLevel
// +0x430, SubThreatLevel +0x434, EconomyThreatLevel +0x438 (RUnitBlueprintDefenseTypeInfo::AddFields 0x521ab0),
// IsMobile (vtable slot 4, 0x51e460: Physics.MotionType != None) and Air.CanFly (+0x368).
// (The original reads its blueprint objects, made once from the blueprint tables; we read the sim's table the
// first time a blueprint's blip goes into a map. They differ only if a script changes those four numbers in the
// table during the game.)
struct ThreatBp {
  float air = 0, surface = 0, sub = 0, economy = 0;
  bool mobile = false, canFly = false;
};
}  // namespace

// The sim's threat maps and their blueprint cache (Sim::threatState). Multicore step 1: these were process-wide
// (g_maps, g_bps).
struct ThreatState {
  std::vector<InfluenceMap> maps;
  std::map<const BlueprintInfo*, ThreatBp> bps;
};

namespace {

ThreatState& TS(Sim& sim) {
  if (!sim.threatState) sim.threatState = std::make_shared<ThreatState>();
  return *sim.threatState;
}
const ThreatState& TS(const Sim& sim) {
  static const ThreatState none;  // (no maps made: immutable)
  return sim.threatState ? *sim.threatState : none;
}

const ThreatBp& BpOf(Sim& sim, const BlueprintInfo* bp) {
  static const ThreatBp none;
  if (!bp) return none;
  std::map<const BlueprintInfo*, ThreatBp>& bps = TS(sim).bps;
  auto it = bps.find(bp);
  if (it != bps.end()) return it->second;
  ThreatBp t;
  lua_State* L = sim.L();
  if (L && bp->kind == BpKind::Unit) {
    int top = lua_gettop(L);
    sim.blueprints().PushTable(L, *bp);
    int def = lu::Sub(L, lua_gettop(L), "Defense");
    t.air = lu::Num(L, def, "AirThreatLevel", 0);
    t.surface = lu::Num(L, def, "SurfaceThreatLevel", 0);
    t.sub = lu::Num(L, def, "SubThreatLevel", 0);
    t.economy = lu::Num(L, def, "EconomyThreatLevel", 0);
    lua_settop(L, top);
    const MotionBlueprint& m = GetMotionBlueprint(L, *bp, sim.blueprints());
    t.mobile = m.mobile();
    t.canFly = m.canFly;
    lua_settop(L, top);
  }
  return bps.emplace(bp, t).first->second;
}

InfluenceMap* MapOf(Sim& sim, int a) {
  std::vector<InfluenceMap>& maps = TS(sim).maps;
  return a >= 0 && static_cast<size_t>(a) < maps.size() ? &maps[static_cast<size_t>(a)] : nullptr;
}
const InfluenceMap* MapOf(const Sim& sim, int a) {
  const std::vector<InfluenceMap>& maps = TS(sim).maps;
  return a >= 0 && static_cast<size_t>(a) < maps.size() ? &maps[static_cast<size_t>(a)] : nullptr;
}

// The playable rect (STIMap +0x08..+0x14, ints; sim.h playableRect): the whole map until SetPlayableRect.
void PlayableRect(const Sim& sim, int r[4]) {
  for (int i = 0; i < 4; ++i) r[i] = sim.playableRect[i];
}

// _ftol2 (the Lua functions' (int) of a number): truncation; out of range gives 0x80000000.
int Ftol(double d) {
  if (!(d > -2147483649.0 && d < 2147483648.0)) return static_cast<int>(0x80000000u);
  return static_cast<int>(d);
}

// CInfluenceMap::VectorToCoords 0x715c60: (int)x / cs and (int)z / cs (truncations), each clamped to the grid.
int VectorToCoords(const InfluenceMap& m, float x, float z) {
  if (m.W <= 0 || m.H <= 0) return 0;
  int cx = static_cast<int>(x) / m.cs;  // cvttss2si (no range guard in the original either)
  cx = std::min(cx, m.W - 1);
  if (cx < 0) cx = 0;
  int cz = static_cast<int>(z) / m.cs;
  cz = std::min(cz, m.H - 1);
  if (cz < 0) cz = 0;
  return cz * m.W + cx;
}

// InfluenceGrid::GetThreat 0x715750 (EAX = cell, ECX = army, EDX = type): the cell's threat of one type from one
// army (army < 0: all armies, in index order), plus what the scripts assigned (not for OverallNotAssigned).
// A type above 14 (also a negative one: unsigned compare) gives 0.
float GetThreat(const Cell& c, int army, int type) {
  if (static_cast<unsigned>(type) > 14u) return 0.0f;
  if (type == 1) {  // OverallNotAssigned: the blips' Overall only
    if (army >= 0) return c.perArmy[static_cast<size_t>(army)].f[kOverall];
    float s = 0.0f;
    for (const SThreat& t : c.perArmy) s = t.f[kOverall] + s;
    return s;
  }
  const int k = type == 0 ? kOverall : type - 1;
  float s = c.assigned.f[k];
  if (army >= 0) return c.perArmy[static_cast<size_t>(army)].f[k] + s;
  for (const SThreat& t : c.perArmy) s = t.f[k] + s;
  return s;
}

// CInfluenceMap::GetThreatRect 0x715ff0: rows cz - rings .. cz + rings, in each the columns cx - rings .. cx + rings;
// a cell off the grid, or (playableOnly) off the playable rect divided by the cell size, is skipped. Summed as
// sum = cell + sum. (We only walk the rows and columns that are on the grid: the others add nothing.)
float GetThreatRect(const Sim& sim, const InfluenceMap& m, int cx, int cz, int rings, bool playableOnly, int type,
                    int army) {
  float sum = 0.0f;
  const long long r = rings;
  if (r < 0) return sum;  // (cz - rings > cz + rings)
  int pr[4];
  PlayableRect(sim, pr);
  const int px0 = pr[0] / m.cs, pz0 = pr[1] / m.cs, px1 = pr[2] / m.cs, pz1 = pr[3] / m.cs;
  const long long z0 = std::max<long long>(cz - r, 0), z1 = std::min<long long>(cz + r, m.H - 1);
  const long long x0 = std::max<long long>(cx - r, 0), x1 = std::min<long long>(cx + r, m.W - 1);
  for (long long z = z0; z <= z1; ++z) {
    if (playableOnly && (z < pz0 || z > pz1)) continue;
    for (long long x = x0; x <= x1; ++x) {
      if (playableOnly && (x < px0 || x > px1)) continue;
      sum = GetThreat(m.cells[static_cast<size_t>(z * m.W + x)], army, type) + sum;
    }
  }
  return sum;
}

// InfluenceGrid::DecayInfluence 0x715130: every assigned threat above 0 moves toward 0 by its decay without
// passing it (max(v - d, min(v + d, 0))); then Overall := the sum of the others, in this order.
void DecayInfluence(Cell& c) {
  SThreat& t = c.assigned;
  for (int k = 1; k < kSlots; ++k) {
    const float v = t.f[k];
    if (!(v > 0.0f)) continue;
    const float d = c.assignedDecay.f[k];
    const float a = v + d, b = v - d;
    float m = a > 0.0f ? 0.0f : a;
    if (b > m) m = b;
    t.f[k] = m;
  }
  float s = t.f[kAntiSurface];
  s = s + t.f[kExperimental];
  s = s + t.f[kStructuresNotMex];
  s = s + t.f[kAntiSub];
  s = s + t.f[kCommander];
  s = s + t.f[kNaval];
  s = s + t.f[kEconomy];
  s = s + t.f[kArtillery];
  s = s + t.f[kLand];
  s = s + t.f[kUnknown];
  s = s + t.f[kAntiAir];
  s = s + t.f[kAir];
  s = s + t.f[kStructures];
  t.f[kOverall] = s;
}

// CInfluenceMap::RemoveEntry 0x715eb0
void RemoveEntry(InfluenceMap& m, uint32_t id) {
  auto it = m.cellOf.find(id);
  if (it == m.cellOf.end()) return;
  Cell& c = m.cells[static_cast<size_t>(it->second)];
  auto e = c.entries.find(id);
  if (e != c.entries.end()) c.entries.erase(e);
  m.cellOf.erase(it);
}

// CInfluenceMap::InsertEntry 0x715d10 (id, &pos, bp): the entry is made or overwritten in the cell of pos.
void InsertEntry(Sim& sim, InfluenceMap& m, uint32_t id, Vec3 pos, const BlueprintInfo* bp) {
  if (m.cells.empty()) return;
  const int cell = VectorToCoords(m, pos.x, pos.z);
  // the army: the id's army byte (id >> 20 & 0xff; 0xff or past the armies: none)
  const uint32_t ai = (id >> 20) & 0xffu;
  const int army = ai != 0xffu && ai < sim.armies().size() ? static_cast<int>(ai) : -1;
  const float decay = BpOf(sim, bp).mobile ? 0.02f : 0.0f;  // 0xe4f8dc
  Entry& e = m.cells[static_cast<size_t>(cell)].entries[id];
  e.id = id;
  e.army = army;
  e.pos = pos;
  e.bp = bp;
  e.layer = 0;
  e.identified = false;
  e.threat = 1.0f;  // 0xdfec20
  e.decay = decay;
  e.counter = 10;
  m.cellOf[id] = cell;
}

// CInfluenceMap::UpdateBlipPosition 0x715f30: a blip the map holds is seen again: full weight for another 10
// updates, or moved to its new cell (RemoveEntry + InsertEntry, which also forgets that it was identified).
void UpdateBlipPosition(Sim& sim, InfluenceMap& m, uint32_t id, Vec3 pos, const BlueprintInfo* bp) {
  auto it = m.cellOf.find(id);
  if (it == m.cellOf.end()) return;
  const int cell = it->second;
  if (cell == VectorToCoords(m, pos.x, pos.z)) {
    Cell& c = m.cells[static_cast<size_t>(cell)];
    auto e = c.entries.find(id);
    if (e == c.entries.end()) return;
    e->second.threat = 1.0f;
    e->second.counter = 10;
    e->second.pos = pos;
    return;
  }
  RemoveEntry(m, id);
  InsertEntry(sim, m, id, pos, bp);
}

// A blip's recon flags for army index a (the original's blip+0x4c4 per-army entries, +4 flags): a real blip's are
// its unit's (Unit::recon), a jammer's fake keeps its own.
uint8_t BlipFlags(const ReconBlip* b, int a) {
  if (a < 0 || a >= 16) return 0;
  if (b->fake) return b->rflags[a];
  return b->source ? b->source->recon[a] : 0;
}

// CInfluenceMap::Update 0x716480
void Update(Sim& sim, InfluenceMap& m) {
  const auto& armies = sim.armies();
  if (m.army < 0 || static_cast<size_t>(m.army) >= armies.size()) return;
  const Army* us = armies[static_cast<size_t>(m.army)].get();
  // categories by name (RRuleGameRulesImpl::GetEntityCategory 0x52b1e0, an exact lookup: an unknown name gives
  // the empty set). "ARTILLERY, STRATEGIC" is not a category name, so in the original the Artillery threat is
  // always 0; BpInCategory finds no such category either.
  static const char* const kCommand = "COMMAND";
  static const char* const kExperimentalCat = "EXPERIMENTAL";
  static const char* const kArtilleryCat = "ARTILLERY, STRATEGIC";
  static const char* const kMex = "MASSEXTRACTION";
  for (Cell& c : m.cells) {
    DecayInfluence(c);
    for (SThreat& t : c.perArmy) t = SThreat{};
    for (auto it = c.entries.begin(); it != c.entries.end();) {
      Entry& e = it->second;
      e.counter = std::max(e.counter - 1, 0);
      if (e.counter == 0) {  // fade toward 0 by decay, not past it
        const float v = e.threat, d = e.decay;
        const float a = v + d, b = v - d;
        float mm = a > 0.0f ? 0.0f : a;
        if (b > mm) mm = b;
        e.threat = mm;
      }
      if (!(e.threat > 0.0f)) {  // faded out (or a known fake): out of the map
        auto ci = m.cellOf.find(e.id);
        if (ci != m.cellOf.end()) m.cellOf.erase(ci);
        it = c.entries.erase(it);
        continue;
      }
      // IArmy::GetAllianceWith 0x579430 (our army's view): allies' blips are kept but do not count
      const Army* src = e.army >= 0 ? armies[static_cast<size_t>(e.army)].get() : nullptr;
      if (!src || combat::Relation(us, src) == 2) {
        ++it;
        continue;
      }
      // the blip entity (EntityDB sim+0x984 by id): its layer, and what we know of it (blip+0x4c4[us].flags)
      if (Entity* ent = sim.FindEntityById(e.id); ent && ent->kind == Entity::Kind::Blip) {
        const ReconBlip* b = static_cast<const ReconBlip*>(ent);
        if (b->source) e.layer = combat::LayerBit(b->source->layer);  // (the blip's layer = its source's)
        const uint8_t f = BlipFlags(b, m.army);
        if (f & 0x20) e.threat = 0.0f;               // KnownFake: counts nothing, goes at the next update
        else if (f & (0x04 | 0x10)) e.identified = true;  // Omni or LOSEver
      }
      const ThreatBp& bp = BpOf(sim, e.bp);
      const float t = e.threat;
      const float eco = bp.economy * t, sub = bp.sub * t, surf = bp.surface * t, air = bp.air * t;
      const float total = ((eco + sub) + surf) + air;
      SThreat& s = c.perArmy[static_cast<size_t>(e.army)];
      s.f[kOverall] += total;
      if (bp.mobile) {
        if (bp.canFly) s.f[kAir] += total;
        else if (e.layer == 1) s.f[kLand] += total;                                    // Land
        else if (e.layer == 8 || e.layer == 2 || e.layer == 4) s.f[kNaval] += total;  // Water, Seabed, Sub
      } else if (BpInCategory(sim, e.bp, kMex)) {
        s.f[kStructures] += total;
      } else {
        s.f[kStructuresNotMex] += total;
        s.f[kStructures] += total;
      }
      if (e.identified) {
        if (BpInCategory(sim, e.bp, kExperimentalCat)) s.f[kExperimental] += total;
        if (BpInCategory(sim, e.bp, kCommand)) s.f[kCommander] += total;
        if (BpInCategory(sim, e.bp, kArtilleryCat)) s.f[kArtillery] += total;
        s.f[kAntiAir] = s.f[kAntiAir] + air;
        s.f[kAntiSurface] = s.f[kAntiSurface] + surf;
        s.f[kAntiSub] = s.f[kAntiSub] + sub;
        s.f[kEconomy] = s.f[kEconomy] + eco;
      } else {
        s.f[kUnknown] += total;
      }
      ++it;
    }
  }
}

// CInfluenceMap::AssignThreatAtPosition 0x716b00 (decay < 0: 0.01, 0xdfeb80). Overall goes into Unknown's slot;
// OverallNotAssigned and types above 14 assign nothing. The cell's assigned Overall is only re-summed by the next
// update (DecayInfluence), so Overall queries do not see it until then.
void AssignThreat(InfluenceMap& m, Vec3 pos, int type, float threat, float decay) {
  if (m.cells.empty()) return;
  Cell& c = m.cells[static_cast<size_t>(VectorToCoords(m, pos.x, pos.z))];
  if (0.0f > decay) decay = 0.01f;
  if (static_cast<unsigned>(type) > 14u || type == 1) return;
  const int k = type == 0 ? kUnknown : type - 1;
  c.assigned.f[k] = c.assigned.f[k] + threat;
  c.assignedDecay.f[k] = c.assigned.f[k] * decay;
}

// CInfluenceMap::GetThreatBetweenPositions 0x716e60: the cells on the line between the two points' cells (their
// centres; InitGridTraversalLine 0x40d860 with step 1, mirrored so the walk goes in +x and +z), one ring each.
float ThreatBetween(const Sim& sim, const InfluenceMap& m, Vec3 p1, Vec3 p2, bool playableOnly, int type,
                    int army) {
  float sum = 0.0f;
  if (m.cells.empty()) return sum;
  auto cellX = [&](float x) { return std::max(std::min(static_cast<int>(x) / m.cs, m.W - 1), 0); };
  auto cellZ = [&](float z) { return std::max(std::min(static_cast<int>(z) / m.cs, m.H - 1), 0); };
  const float x0 = static_cast<float>(cellX(p1.x)) + 0.5f, z0 = static_cast<float>(cellZ(p1.z)) + 0.5f;
  const float x1 = static_cast<float>(cellX(p2.x)) + 0.5f, z1 = static_cast<float>(cellZ(p2.z)) + 0.5f;
  float X0, X1, Z0, Z1;
  int xmask, zmask;
  if (x1 >= x0) {
    X0 = x0, X1 = x1, xmask = 0;
  } else {
    X0 = -0.0f - x0, X1 = -0.0f - x1, xmask = -1;
  }
  if (z1 >= z0) {
    Z0 = z0, Z1 = z1, zmask = 0;
  } else {
    Z0 = -0.0f - z0, Z1 = -0.0f - z1, zmask = -1;
  }
  const float dx = X1 - X0, dz = Z1 - Z0;
  int cx = static_cast<int>(std::floor(X0)), cz = static_cast<int>(std::floor(Z0));
  for (;;) {
    sum = GetThreatRect(sim, m, cx ^ xmask, cz ^ zmask, 0, playableOnly, type, army) + sum;
    const int nx = cx + 1, nz = cz + 1;
    const float a = (static_cast<float>(nx) - X1) * dz;
    const float b = (static_cast<float>(nz) - Z1) * dx;
    if (b > a) cx = nx;
    else cz = nz;
    if (static_cast<float>(cx) > X1) break;
    if (!(static_cast<float>(cz) <= Z1)) break;
  }
  return sum;
}

// the cell's centre: cs * c + cs / 2 (integer), as a float
float Centre(const InfluenceMap& m, int c) { return static_cast<float>(m.cs * c + m.cs / 2); }

// CInfluenceMap::GetHighestThreatPosition 0x716fc0: the cell with the most threat (rings 0: the cell alone, without
// the playable test; else GetThreatRect around it); equal threats go to the cell nearer the army's start
// (CArmyImpl::GetArmyStartPos 0x6fdce0), then to the first. Both start at -inf (0x10b9200 = -0.0 - +inf).
void Highest(const Sim& sim, const InfluenceMap& m, int rings, bool playableOnly, int type, int army, Vec3* pos,
             float* threat) {
  const Army& a = *sim.armies()[static_cast<size_t>(m.army)];
  const float sx = a.startX, sz = a.startZ;
  const float inf = HUGE_VALF;
  float best = -0.0f - inf, bestD = -0.0f - inf;
  int bestI = 0;
  for (size_t i = 0; i < m.cells.size(); ++i) {
    const int ii = static_cast<int>(i);
    const float t = rings == 0 ? GetThreat(m.cells[i], army, type)
                               : GetThreatRect(sim, m, ii % m.W, ii / m.W, rings, playableOnly, type, army);
    if (t > best) {
      const float dx = sx - Centre(m, ii % m.W), dz = sz - Centre(m, ii / m.W);
      bestD = dx * dx + dz * dz;
      best = t;
      bestI = ii;
    } else if (t == best) {
      const float dx = sx - Centre(m, ii % m.W), dz = sz - Centre(m, ii / m.W);
      const float d = dz * dz + dx * dx;
      if (bestD > d) {
        bestD = d;
        best = t;
        bestI = ii;
      }
    }
  }
  *threat = best;
  *pos = m.W > 0 ? Vec3{Centre(m, bestI % m.W), 0.0f, Centre(m, bestI / m.W)} : Vec3{};
}

// ---- std::sort as the original's library has it (MSVC 8/9: 0x71e200 _Sort, 0x71efa0 _Unguarded_partition,
// 0x71f870 _Median, 0x71fee0 _Med3, 0x71f2c0 _Insertion_sort, 0x71f990 / 0x71fa00 make_heap / sort_heap), so that
// equal threats come out in the original's order.
struct ThreatPt {
  float x = 0, y = 0, z = 0, threat = 0;
};
bool Before(const ThreatPt& a, const ThreatPt& b) { return a.threat > b.threat; }  // the predicate (0x71f2e0)
using It = ThreatPt*;

void Med3(It a, It b, It c) {
  if (Before(*b, *a)) std::swap(*b, *a);
  if (Before(*c, *b)) std::swap(*c, *b);
  if (Before(*b, *a)) std::swap(*b, *a);
}
void Median(It first, It mid, It last) {  // last inclusive
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
std::pair<It, It> UnguardedPartition(It first, It last) {
  It mid = first + (last - first) / 2;
  Median(first, mid, last - 1);
  It pfirst = mid, plast = pfirst + 1;
  while (first < pfirst && !Before(*(pfirst - 1), *pfirst) && !Before(*pfirst, *(pfirst - 1))) --pfirst;
  while (plast < last && !Before(*plast, *pfirst) && !Before(*pfirst, *plast)) ++plast;
  It gfirst = plast, glast = pfirst;
  for (;;) {
    for (; gfirst < last; ++gfirst) {
      if (Before(*pfirst, *gfirst)) continue;
      if (Before(*gfirst, *pfirst)) break;
      std::swap(*plast++, *gfirst);
    }
    for (; first < glast; --glast) {
      if (Before(*(glast - 1), *pfirst)) continue;
      if (Before(*pfirst, *(glast - 1))) break;
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
void InsertionSort(It first, It last) {
  if (first == last) return;
  for (It next = first; ++next != last;) {
    ThreatPt val = *next;
    if (Before(val, *first)) {
      std::copy_backward(first, next, next + 1);
      *first = val;
    } else {
      It hole = next;
      for (It prev = next; Before(val, *--prev); hole = prev) *hole = *prev;
      *hole = val;
    }
  }
}
void PushHeap(It first, ptrdiff_t hole, ptrdiff_t top, ThreatPt val) {
  for (ptrdiff_t idx = (hole - 1) / 2; top < hole && Before(first[idx], val); idx = (hole - 1) / 2) {
    first[hole] = first[idx];
    hole = idx;
  }
  first[hole] = val;
}
void AdjustHeap(It first, ptrdiff_t hole, ptrdiff_t bottom, ThreatPt val) {
  const ptrdiff_t top = hole;
  ptrdiff_t idx = 2 * hole + 2;
  for (; idx < bottom; idx = 2 * idx + 2) {
    if (Before(first[idx], first[idx - 1])) --idx;
    first[hole] = first[idx];
    hole = idx;
  }
  if (idx == bottom) {
    first[hole] = first[bottom - 1];
    hole = bottom - 1;
  }
  PushHeap(first, hole, top, val);
}
void HeapSort(It first, It last) {
  const ptrdiff_t bottom = last - first;
  if (bottom > 1)
    for (ptrdiff_t hole = bottom / 2; 0 < hole;) {
      --hole;
      AdjustHeap(first, hole, bottom, first[hole]);
    }
  for (; 1 < last - first; --last) {
    ThreatPt val = *(last - 1);
    *(last - 1) = *first;
    AdjustHeap(first, 0, (last - 1) - first, val);
  }
}
void MsvcSort(It first, It last, ptrdiff_t ideal) {
  ptrdiff_t count;
  for (; 32 < (count = last - first) && 0 < ideal;) {
    std::pair<It, It> mid = UnguardedPartition(first, last);
    ideal /= 2, ideal += ideal / 2;
    if (mid.first - first < last - mid.second) {
      MsvcSort(first, mid.first, ideal);
      first = mid.second;
    } else {
      MsvcSort(mid.second, last, ideal);
      last = mid.first;
    }
  }
  if (32 < count) HeapSort(first, last);
  else if (1 < count) InsertionSort(first, last);
}

// CInfluenceMap::GetThreatsAroundPosition 0x7171d0: the cells around pos's cell (rows, then columns, as
// GetThreatRect) whose threat is above 0, at their centres, most threat first.
std::vector<ThreatPt> Around(const Sim& sim, const InfluenceMap& m, Vec3 pos, int rings, bool playableOnly, int type,
                             int army) {
  std::vector<ThreatPt> out;
  if (m.cells.empty() || rings < 0) return out;
  const int cell = VectorToCoords(m, pos.x, pos.z);
  const int cx = cell % m.W, cz = cell / m.W;
  int pr[4];
  PlayableRect(sim, pr);
  const int px0 = pr[0] / m.cs, pz0 = pr[1] / m.cs, px1 = pr[2] / m.cs, pz1 = pr[3] / m.cs;
  const long long r = rings;
  const long long z0 = std::max<long long>(cz - r, 0), z1 = std::min<long long>(cz + r, m.H - 1);
  const long long x0 = std::max<long long>(cx - r, 0), x1 = std::min<long long>(cx + r, m.W - 1);
  for (long long z = z0; z <= z1; ++z) {
    if (playableOnly && (z < pz0 || z > pz1)) continue;
    for (long long x = x0; x <= x1; ++x) {
      if (playableOnly && (x < px0 || x > px1)) continue;
      const int i = static_cast<int>(z * m.W + x);
      const float t = GetThreat(m.cells[static_cast<size_t>(i)], army, type);
      if (!(t > 0.0f)) continue;
      out.push_back({Centre(m, i % m.W), 0.0f, Centre(m, i / m.W), t});
    }
  }
  if (!out.empty()) MsvcSort(out.data(), out.data() + out.size(), static_cast<ptrdiff_t>(out.size()));
  return out;
}

// ---- Lua ------------------------------------------------------------------------------------------------

int DebugThreat() {
  static const int v = getenv("MOHO64_DEBUG_THREAT") ? atoi(getenv("MOHO64_DEBUG_THREAT")) : 0;
  return v;
}

void ArgCount(lua_State* L, int lo, int hi, const char* usage) {
  const int n = lua_gettop(L);
  if (n < lo || n > hi) luaL_error(L, "%s\n  expected between %d and %d args, but got %d", usage, lo, hi, n);
}
// LuaStackObject checks: "integer" and "number" want a number (no string), a boolean may be nil (GetBoolean
// 0x415560).
int IntArg(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "integer");
  return Ftol(lua_tonumber(L, idx));
}
float NumArg(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "number");
  return static_cast<float>(lua_tonumber(L, idx));
}
bool BoolArg(lua_State* L, int idx) {
  const int t = lua_type(L, idx);
  if (t != LUA_TBOOLEAN && t != LUA_TNIL) luaL_typerror(L, idx, "boolean");
  return lua_toboolean(L, idx) != 0;
}
// SCR_FromLuaCopy<Vector3f> 0x4d0080: t[1], t[2], t[3]
Vec3 VecArg(lua_State* L, int idx) {
  luaL_checktype(L, idx, LUA_TTABLE);
  Vec3 v;
  lua_rawgeti(L, idx, 1);
  v.x = static_cast<float>(lua_tonumber(L, -1));
  lua_rawgeti(L, idx, 2);
  v.y = static_cast<float>(lua_tonumber(L, -1));
  lua_rawgeti(L, idx, 3);
  v.z = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 3);
  return v;
}
// [armyIndex] (1-based): 0-based, or the function's error
int ArmyArg(lua_State* L, int idx, const char* err) {
  const int a = IntArg(L, idx) - 1;
  if (a < 0 || static_cast<size_t>(a) >= S(L)->armies().size()) luaL_error(L, "%s", err);
  return a;
}

AiBrain* BrainArg(lua_State* L) { return CheckObject<AiBrain>(L, 1); }
InfluenceMap* BrainMap(lua_State* L, AiBrain* b) {
  InfluenceMap* m = b->army ? MapOf(*S(L), b->army->index - 1) : nullptr;
  if (!m) luaL_error(L, "moho64: the brain's army has no threat map");
  return m;
}

// CAiBrain:AssignThreatAtPosition(position, threat, [decay], [threattype]) 0x590000: decay is clamped to [0, 1]
// (not given: -1, i.e. 0.01 per update); the type defaults to Overall.
int l_AssignThreatAtPosition(lua_State* L) {
  ArgCount(L, 3, 5, "CAiBrain:AssignThreatAtPosition(position, threat, [decay], [threattype])");
  AiBrain* brain = BrainArg(L);
  const Vec3 pos = VecArg(L, 2);
  const float threat = NumArg(L, 3);
  float decay = -1.0f;  // 0xe4f6e8
  int type = 0;
  const int n = lua_gettop(L);
  if (n > 3) {
    const float d = NumArg(L, 4);
    decay = 1.0f > d ? d : 1.0f;  // (fcompi: NaN gives 1)
    if (0.0f > decay) decay = 0.0f;
  }
  if (n > 4) type = CheckThreatType(L, 5);
  InfluenceMap* m = BrainMap(L, brain);
  AssignThreat(*m, pos, type, threat, decay);
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u army %d Assign (%.1f, %.1f) type %d threat %g decay %g", S(L)->tick(),
         m->army + 1, pos.x, pos.z, type, threat, decay);
  return 0;
}

// CAiBrain:GetThreatAtPosition(position, ring, restriction, [threatType], [armyIndex]) 0x5902e0
int l_GetThreatAtPosition(lua_State* L) {
  ArgCount(L, 4, 6, "CAiBrain:GetThreatAtPosition(position, ring, restriction, [threatType], [armyIndex] )");
  AiBrain* brain = BrainArg(L);
  const Vec3 pos = VecArg(L, 2);
  const int rings = IntArg(L, 3);
  const bool restrict = BoolArg(L, 4);
  int type = 0, army = -1;
  const int n = lua_gettop(L);
  if (n > 4) type = CheckThreatType(L, 5);
  if (n > 5) army = ArmyArg(L, 6, "Invalid army index passed in to GetThreatAtPosition");
  InfluenceMap* m = BrainMap(L, brain);
  float t = 0.0f;
  if (!m->cells.empty()) {
    const int cell = VectorToCoords(*m, pos.x, pos.z);
    t = GetThreatRect(*S(L), *m, cell % m->W, cell / m->W, rings, restrict, type, army);
  }
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u army %d AtPosition (%.1f, %.1f) rings %d type %d of %d = %g", S(L)->tick(),
         m->army + 1, pos.x, pos.z, rings, type, army + 1, t);
  lua_pushnumber(L, t);
  return 1;
}

// CAiBrain:GetThreatBetweenPositions(position, position, restriction, [threatType], [armyIndex]) 0x590630
int l_GetThreatBetweenPositions(lua_State* L) {
  ArgCount(L, 4, 6, "CAiBrain:GetThreatBetweenPositions( position, position, restriction, [threatType], [armyIndex] )");
  AiBrain* brain = BrainArg(L);
  const Vec3 p1 = VecArg(L, 2);
  const Vec3 p2 = VecArg(L, 3);
  const bool restrict = BoolArg(L, 4);
  int type = 0, army = -1;
  const int n = lua_gettop(L);
  if (n > 4) type = CheckThreatType(L, 5);
  if (n > 5) army = ArmyArg(L, 6, "Invalid army index passed in to GetThreatBetweenPositions");
  InfluenceMap* m = BrainMap(L, brain);
  const float t = ThreatBetween(*S(L), *m, p1, p2, restrict, type, army);
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u army %d Between (%.1f, %.1f) (%.1f, %.1f) type %d of %d = %g", S(L)->tick(),
         m->army + 1, p1.x, p1.z, p2.x, p2.z, type, army + 1, t);
  lua_pushnumber(L, t);
  return 1;
}

// CAiBrain:GetHighestThreatPosition(ring, restriction, [threatType], [armyIndex]) 0x590950: position, threat
int l_GetHighestThreatPosition(lua_State* L) {
  ArgCount(L, 3, 5, "CAiBrain:GetHighestThreatPosition( ring, restriction, [threatType], [armyIndex] )");
  AiBrain* brain = BrainArg(L);
  const int rings = IntArg(L, 2);
  const bool restrict = BoolArg(L, 3);
  int type = 0, army = -1;
  const int n = lua_gettop(L);
  if (n > 3) type = CheckThreatType(L, 4);
  if (n > 4) army = ArmyArg(L, 5, "Invalid army index passed in to GetHighestThreatPosition");
  InfluenceMap* m = BrainMap(L, brain);
  Vec3 pos;
  float t = 0.0f;
  Highest(*S(L), *m, rings, restrict, type, army, &pos, &t);
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u army %d Highest rings %d type %d of %d = (%.1f, %.1f) %g", S(L)->tick(),
         m->army + 1, rings, type, army + 1, pos.x, pos.z, t);
  PushVector(L, pos.x, pos.y, pos.z);
  lua_pushnumber(L, t);
  return 2;
}

// CAiBrain:GetThreatsAroundPosition(position, ring, restriction, [threatType], [armyIndex]) 0x590c80:
// { {x, z, threat}, ... }, most threat first
int l_GetThreatsAroundPosition(lua_State* L) {
  ArgCount(L, 4, 6, "CAiBrain:GetThreatsAroundPosition( position, ring, restriction, [threatType], [armyIndex] )");
  AiBrain* brain = BrainArg(L);
  const Vec3 pos = VecArg(L, 2);
  const int rings = IntArg(L, 3);
  const bool restrict = BoolArg(L, 4);
  int type = 0, army = -1;
  const int n = lua_gettop(L);
  if (n > 4) type = CheckThreatType(L, 5);
  if (n > 5) army = ArmyArg(L, 6, "Invalid army index passed in to GetThreatsAroundPosition");
  InfluenceMap* m = BrainMap(L, brain);
  const std::vector<ThreatPt> pts = Around(*S(L), *m, pos, rings, restrict, type, army);
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u army %d Around (%.1f, %.1f) rings %d type %d of %d = %zu cells, top %g",
         S(L)->tick(), m->army + 1, pos.x, pos.z, rings, type, army + 1, pts.size(),
         pts.empty() ? 0.0 : static_cast<double>(pts[0].threat));
  lua_newtablesized(L, static_cast<int>(pts.size()), 0);
  int i = 0;
  for (const ThreatPt& p : pts) {
    lua_newtablesized(L, 3, 0);
    lua_pushnumber(L, p.x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, p.z);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, p.threat);
    lua_rawseti(L, -2, 3);
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}

// CAiBrain:CheckBlockingTerrain( startPos, endPos, arcType ) 0x5910a0: whether the terrain is in the way of a shot.
// Not a threat-map function: it reads only the terrain (STIMap::Intersection 0x476f30 on segments made by
// MakeSegment3fFromEndpoints 0x4fe130, exactly what combat::TerrainSegmentHit does: from the segment's start over its
// length; a miss leaves the hit point invalid, IsValid_Vector3f 0x5657f0). The start is raised by 1, the end by 0.5.
// 'none' (_stricmp): the straight segment start -> end. Anything else: 4 pieces of a quarter of the way each, the
// height bent by 0.707, 0.293, -0.293, -0.707 (0xf6a1d4) times a quarter's length times 0.5 for 'low' and 2 for any
// other arc. Blocked when a piece hits and its length is not below the hit distance.
int l_CheckBlockingTerrain(lua_State* L) {
  const int n = lua_gettop(L);
  if (n != 4)
    luaL_error(L, "%s\n  expected %d args, but got %d", "CAiBrain:CheckBlockingTerrain( startPos, endPos, arcType )", 4,
               n);
  BrainArg(L);
  Vec3 a = VecArg(L, 2), b = VecArg(L, 3);
  const char* arc = lua_tostring(L, 4);
  if (!arc) luaL_typerror(L, 4, "string");
  a.y = a.y + 1.0f;  // 0xdfec20
  b.y = b.y + 0.5f;  // 0xe4f724
  const TerrainMap* map = S(L)->map();
  auto valid = [](const Vec3& v) { return !std::isnan(v.x) && !std::isnan(v.y) && !std::isnan(v.z); };
  bool blocked = false;
  float t = 0;
  Vec3 hit;
  if (strcasecmp(arc, "none") == 0) {
    if (combat::TerrainSegmentHit(map, a, b, &t, &hit) && valid(hit)) {
      const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      const float d = std::sqrt((dz * dz + dy * dy) + dx * dx);  // (fsqrt at precision 24)
      if (d >= t) blocked = true;                                 // (fcompi; jb: NaN is not blocked)
    }
  } else {
    const Vec3 step{(b.x - a.x) * 0.25f, (b.y - a.y) * 0.25f, (b.z - a.z) * 0.25f};  // 0xe4f6ec
    const float sl = std::sqrt((step.z * step.z + step.y * step.y) + step.x * step.x);
    static const float kBend[4] = {0.707f, 0.293f, -0.293f, -0.707f};
    const float k = strcasecmp(arc, "low") == 0 ? 0.5f : 2.0f;  // "low" 0xe1aed0; 0xe4f724 / 0xdfeb0c
    Vec3 cur = a;
    for (int i = 0; i < 4 && !blocked; ++i) {
      Vec3 next{step.x + cur.x, cur.y + step.y, cur.z + step.z};
      next.y = kBend[i] * sl * k + next.y;
      if (combat::TerrainSegmentHit(map, cur, next, &t, &hit) && valid(hit)) {
        const float dx = cur.x - next.x, dz = cur.z - next.z, dy = cur.y - next.y;
        const float d = std::sqrt((dx * dx + dz * dz) + dy * dy);
        if (d >= t) blocked = true;
      }
      cur = next;
    }
  }
  if (DebugThreat())
    Logf(LogLevel::Info, "threat %u CheckBlockingTerrain (%.1f, %.1f, %.1f) (%.1f, %.1f, %.1f) %s = %d", S(L)->tick(),
         a.x, a.y, a.z, b.x, b.y, b.z, arc, blocked ? 1 : 0);
  lua_pushboolean(L, blocked);
  return 1;
}

}  // namespace

// EThreatType by name (sim/enum_arg.h, SCR_GetEnum 0x4d2550)
int CheckThreatType(lua_State* L, int idx) { return EnumArgAt(L, idx, kEnumThreatType); }

void InitThreatMaps(Sim& sim) {
  ThreatState& ts = TS(sim);
  ts.maps.clear();
  ts.bps.clear();
  const TerrainMap* map = sim.map();
  const int mw = map ? map->width() : 0, mh = map ? map->height() : 0;
  // cell size (CArmyImpl ctor 0x6feb40): max(32, max(w, h) / 16), w and h = the heightfield's size - 1
  const int cs = std::max(32, std::max(mw, mh) / 16);
  const size_t n = sim.armies().size();
  ts.maps.resize(n);
  for (size_t a = 0; a < n; ++a) {
    InfluenceMap& m = ts.maps[a];
    m.army = static_cast<int>(a);
    m.cs = cs;
    m.W = mw / cs;
    m.H = mh / cs;
    if (m.W <= 0 || m.H <= 0) continue;
    m.cells.resize(static_cast<size_t>(m.W) * static_cast<size_t>(m.H));
    for (Cell& c : m.cells) c.perArmy.resize(n);  // a threat per army (all armies, civilians too)
  }
}

void ThreatInsertEntry(Sim& sim, int a, const ReconBlip* blip) {
  InfluenceMap* m = MapOf(sim, a);
  if (m && blip) InsertEntry(sim, *m, blip->id, blip->position, blip->blueprint);
}

void ThreatUpdateBlipPosition(Sim& sim, int a, const ReconBlip* blip) {
  InfluenceMap* m = MapOf(sim, a);
  if (m && blip) UpdateBlipPosition(sim, *m, blip->id, blip->position, blip->blueprint);
}

void ThreatRemoveEntry(Sim& sim, int a, uint32_t blipId) {
  if (InfluenceMap* m = MapOf(sim, a)) RemoveEntry(*m, blipId);
}

void ThreatMapBeat(Sim& sim) {
  const uint32_t slot = sim.tick() % 30u;
  std::vector<InfluenceMap>& maps = TS(sim).maps;
  if (slot >= maps.size()) return;
  InfluenceMap& m = maps[slot];
  Update(sim, m);
  if (DebugThreat()) {
    size_t entries = 0;
    float overall = 0;
    for (const Cell& c : m.cells) {
      entries += c.entries.size();
      overall += GetThreat(c, -1, 0);
    }
    Logf(LogLevel::Info, "threat %u army %d update: %zu entries, Overall %g", sim.tick(), m.army + 1, entries,
         overall);
    if (DebugThreat() >= 2)  // =2: every cell with blips, per army: its non-zero classes
      for (size_t i = 0; i < m.cells.size(); ++i) {
        const Cell& c = m.cells[i];
        if (c.entries.empty()) continue;
        for (size_t k = 0; k < c.perArmy.size(); ++k) {
          const SThreat& t = c.perArmy[k];
          std::string s;
          static const char* const names[kSlots] = {"all", "str", "strnm", "naval", "land", "air", "exp", "cmd",
                                                    "arty", "aa", "surf", "sub", "eco", "unk"};
          for (int j = 0; j < kSlots; ++j)
            if (t.f[j] != 0) s += std::string(" ") + names[j] + "=" + std::to_string(t.f[j]);
          if (!s.empty())
            Logf(LogLevel::Info, "threat   cell (%d, %d) %zu blips, army %zu:%s", static_cast<int>(i) % m.W,
                 static_cast<int>(i) / m.W, c.entries.size(), k + 1, s.c_str());
        }
      }
  }
}

float ThreatRect(const Sim& sim, int a, int cx, int cz, int rings, bool playableOnly, int type, int threatArmy) {
  const InfluenceMap* m = MapOf(sim, a);
  return m && !m->cells.empty() ? GetThreatRect(sim, *m, cx, cz, rings, playableOnly, type, threatArmy) : 0.0f;
}

int ThreatCellOf(const Sim& sim, int a, float x, float z) {
  const InfluenceMap* m = MapOf(sim, a);
  return m ? VectorToCoords(*m, x, z) : 0;
}

float ThreatRectAt(const Sim& sim, int a, float x, float z, int rings, bool playableOnly, int type, int threatArmy) {
  const InfluenceMap* m = MapOf(sim, a);
  if (!m || m->cells.empty() || m->W <= 0) return 0.0f;
  const int cell = VectorToCoords(*m, x, z);
  return GetThreatRect(sim, *m, cell % m->W, cell / m->W, rings, playableOnly, type, threatArmy);
}

void RegisterThreatBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "AssignThreatAtPosition", l_AssignThreatAtPosition);
  SetMethod(L, "CAiBrain", "GetThreatAtPosition", l_GetThreatAtPosition);
  SetMethod(L, "CAiBrain", "GetThreatBetweenPositions", l_GetThreatBetweenPositions);
  SetMethod(L, "CAiBrain", "GetHighestThreatPosition", l_GetHighestThreatPosition);
  SetMethod(L, "CAiBrain", "GetThreatsAroundPosition", l_GetThreatsAroundPosition);
  SetMethod(L, "CAiBrain", "CheckBlockingTerrain", l_CheckBlockingTerrain);
}

}  // namespace moho
