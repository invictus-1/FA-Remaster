// The sync point's copies for an interface (see sim_snapshot.h).
//
// The placement snapshot holds what CanBuildStructureAt and SnapStructurePosition read (sim/placement_test.h runs the
// same code on it): each unit blueprint's footprint and placement data (once), the heightfield in bands of 32 rows
// (a band is copied again only when FlattenMapRect wrote in it: TerrainMap::heightChanges), the terrain types (once:
// nothing changes them after the load), the water, the blocking terrain types, the occupancy grids in bands of 32
// rows (copied again when Navigation::SetCells changed a bit in one: OccBandStamp), the deposits (again only when the
// list changed) and, every Update, the units the two skirt tests look at: every one the sim's unit grid holds (not
// destroy-queued) that is not mobile, with what the tests read of it.
//
// The reclaim snapshot holds the reclaimable props in the cells of the sim's prop grid (16 x 16 map units), each cell
// a shared immutable list in the props' order: an Update makes anew only the cells where a prop came, went, moved or
// was rescaled.
#include "sim/sim_snapshot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "core/fpenv.h"
#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/collision.h"
#include "sim/entity_grid.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/placement_test.h"
#include "sim/sim.h"
#include "sim/terrain.h"
#include "sim/units.h"

namespace moho {
namespace {

constexpr int kBandRows = 32;  // heightfield rows (vertices) per band; the occupancy's: Navigation::kOccBandRows
static_assert(Navigation::kOccBandRows == kBandRows, "the occupancy bands are copied with the heights' size");

using Band16 = std::shared_ptr<const std::vector<uint16_t>>;
using Band8 = std::shared_ptr<const std::vector<uint8_t>>;

struct PlaceBp {
  NamedFootprint fp;
  UnitBpData d;
};
struct BpTable {
  std::unordered_map<std::string, PlaceBp> byId;  // by the blueprint's id as SimBlueprints::Find finds it
};

// What the skirt tests read of one not-mobile unit (sim/build.cpp AnyStructureSkirtOverlaps, LiveSkirtScan).
struct PlacedUnit {
  float px = 0, pz = 0;  // its position (ForUnitsInRect's test)
  placement::Rect skirt{0, 0, 0, 0};
  uint16_t gx0 = 0, gz0 = 0, gw = 0, gh = 0;  // its entity-grid cells (Entity::gridX0 ..)
  bool gridSeen = false, alive = false, hasShape = false;
  Vec3 mn, mx;  // its collision box's bounds (ShapeBounds of GetWorldShape), when hasShape
};

struct ReclaimEntry {
  uint32_t id = 0, seq = 0, name = 0;  // entity id, the props' order, the blueprint (ReclaimSnapshot::names)
  float x = 0, y = 0, z = 0, sx = 0, sz = 0;
  uint32_t armies = 0xffffffffu;  // the armies that had seen it (PropSeenBy; every army unless a tracked wreck)
};
using Cell = std::vector<ReclaimEntry>;
// The cells in blocks of 8 x 8: an Update copies the block list (some hundred pointers) and the blocks it changes, not
// a pointer per cell (4,290 on a 1024 map).
constexpr int kBlock = 8;
struct CellBlock {
  std::array<std::shared_ptr<const Cell>, kBlock * kBlock> cells;  // null: empty
};
struct CellTable {
  int gw = 0, gh = 0, bw = 0;  // cells across and down; blocks across
  std::vector<std::shared_ptr<const CellBlock>> blocks;  // null: every cell empty
  const Cell* At(int cx, int cz) const {
    const CellBlock* b = blocks[static_cast<size_t>(cz / kBlock) * bw + cx / kBlock].get();
    return b ? b->cells[static_cast<size_t>(cz % kBlock) * kBlock + cx % kBlock].get() : nullptr;
  }
};

int UnitGridW(const Sim& sim) { return sim.map() ? (sim.map()->width() + 15) / 16 + 1 : 64; }  // (RebuildUnitGrid)
int UnitGridH(const Sim& sim) { return sim.map() ? (sim.map()->height() + 15) / 16 + 1 : 64; }

}  // namespace

// The heightfield as the placement test reads it (TerrainMap's reads, on the bands).
class SnapMap {
 public:
  int w = 0, h = 0;
  float scale = 1.0f / 128;
  std::vector<Band16> bands;  // rows 0 .. h in bands of kBandRows, each row w + 1 samples
  std::shared_ptr<const std::vector<uint8_t>> types;  // w * h
  bool hasWater = false;
  float waterElevation = 0;

  int width() const { return w; }
  int height() const { return h; }
  float heightScale() const { return scale; }
  const uint16_t* heightRow(int vz) const {
    return bands[static_cast<size_t>(vz / kBandRows)]->data() + static_cast<size_t>(vz % kBandRows) * (w + 1);
  }
  float HeightAt(int x, int z) const {  // TerrainMap::HeightAt
    x = std::clamp(x, 0, w);
    z = std::clamp(z, 0, h);
    return heightRow(z)[x] * scale;
  }
  float TerrainHeight(float x, float z) const {  // TerrainMap::TerrainHeight
    if (bands.empty()) return 0;
    return TerrainHeightBilinear([this](int ix, int iz) { return HeightAt(ix, iz); }, x, z);
  }
  float SurfaceHeight(float x, float z) const {  // TerrainMap::SurfaceHeight
    float v = TerrainHeight(x, z);
    return hasWater ? std::max(v, waterElevation) : v;
  }
  uint8_t TerrainType(int x, int z) const {  // TerrainMap::TerrainType
    if (!types || types->empty()) return 0;
    x = std::clamp(x, 0, w - 1);
    z = std::clamp(z, 0, h - 1);
    return (*types)[static_cast<size_t>(z) * w + x];
  }
};

class PlacementSnapshot {
 public:
  uint32_t tick = 0;
  bool hasMap = false;
  SnapMap map;
  std::shared_ptr<const BpTable> bps;
  std::shared_ptr<const std::vector<ResourceDeposit>> deposits;
  bool hasNav = false;  // (none yet: no blocking types, no occupancy; the sim makes it at its first path or test)
  std::array<bool, 256> blocking{};
  bool occAny = false;  // Navigation's grids made (its first structure)
  int occW = 0, occH = 0;
  std::vector<Band8> occLand, occWater;  // rows of occW cells in bands of kBandRows
  bool egridReady = false;               // Sim::entityGrid().ready()
  int gridW = 0, gridH = 0;              // the sim's unit grid (ForUnitsInRect)
  std::shared_ptr<const std::vector<PlacedUnit>> units;  // by cell
  std::shared_ptr<const std::vector<uint32_t>> cellStart;  // gridW * gridH + 1
};

class ReclaimSnapshot {
 public:
  uint32_t tick = 0;
  std::shared_ptr<const CellTable> table;
  std::shared_ptr<const std::vector<std::string>> names;
};

namespace {

// The placement test's reads of a snapshot (the world of sim/placement_test.h).
struct SnapPlaceWorld {
  const PlacementSnapshot& s;
  const SnapMap* map() const { return s.hasMap ? &s.map : nullptr; }
  // (the sim's offers only the deposits near the rect; every one that can meet it is among them, so walking them all
  // gives the same answer: sim/build.cpp DepositCells)
  template <class F>
  bool AnyDepositNear(double, double, double, double, F&& test) const {
    if (s.deposits)
      for (const ResourceDeposit& d : *s.deposits)
        if (test(d)) return true;
    return false;
  }
  uint8_t MobileCaps(const NamedFootprint& fp, int x, int z) const {  // Navigation::MobileCaps
    return s.hasMap ? MobileCheckCapsOf(s.map, fp, s.blocking.data(), x, z) : 0;
  }
  bool AnyIn(int x0, int z0, int x1, int z1, uint8_t occ) const {  // Navigation::AnyStructureIn
    if (!s.occAny || !s.hasMap) return false;
    for (int z = std::max(0, z0); z < std::min(s.occH, z1); ++z) {
      const size_t b = static_cast<size_t>(z / kBandRows), row = static_cast<size_t>(z % kBandRows) * s.occW;
      const uint8_t* land = s.occLand[b]->data() + row;
      const uint8_t* water = s.occWater[b]->data() + row;
      for (int x = std::max(0, x0); x < std::min(s.occW, x1); ++x)
        if (((occ & kOccLand) && land[x]) || ((occ & kOccWater) && water[x])) return true;
    }
    return false;
  }
  uint8_t FootprintFits(uint8_t caps, const NamedFootprint& fp, int x0, int z0) const {  // Navigation::FootprintFits
    return FitsCaps(caps, (fp.flags & 1) != 0, AnyIn(x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ, kOccLand),
                    AnyIn(x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ, kOccWater));
  }
  bool AnyStructureIn(int x0, int z0, int x1, int z1) const { return AnyIn(x0, z0, x1, z1, kOccLand | kOccWater); }
  // Sim::ForUnitsInRect over the copied units (they are the not-mobile ones; the tests skip every other)
  template <class F>
  void ForUnitsInRect(float x0, float z0, float x1, float z1, F&& f) const {
    if (!s.units) return;
    int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
    int cx1 = std::min(s.gridW - 1, static_cast<int>(x1) >> 4), cz1 = std::min(s.gridH - 1, static_cast<int>(z1) >> 4);
    if (x1 < 0 || z1 < 0) return;
    const std::vector<PlacedUnit>& u = *s.units;
    const std::vector<uint32_t>& start = *s.cellStart;
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx) {
        const size_t c = static_cast<size_t>(cz) * s.gridW + cx;
        for (uint32_t i = start[c]; i < start[c + 1]; ++i) {
          const PlacedUnit& e = u[i];
          if (e.px >= x0 && e.px <= x1 && e.pz >= z0 && e.pz <= z1) f(e);
        }
      }
  }
  bool AnyStructureSkirtOverlaps(const placement::Rect& R) const {  // sim/build.cpp AnyStructureSkirtOverlaps
    const placement::GridCells gc = placement::GridCellsOf(R);
    bool hit = false;
    ForUnitsInRect(R.x0 - 32, R.z0 - 32, R.x1 + 32, R.z1 + 32, [&](const PlacedUnit& e) {
      if (hit) return;
      if (s.egridReady && e.gridSeen && !placement::GridCellsShared(gc, e.gx0, e.gz0, e.gw, e.gh)) return;
      if (placement::SkirtsMeet(e.skirt, R)) hit = true;
    });
    return hit;
  }
  bool LiveStructureSkirtOverlaps(const placement::Rect& R) const {  // sim/build.cpp LiveSkirtScan, relation 3
    const Vec3 bmn{R.x0, -1000.0f, R.z0}, bmx{R.x1, 1000.0f, R.z1};
    bool hit = false;
    ForUnitsInRect(R.x0 - 32, R.z0 - 32, R.x1 + 32, R.z1 + 32, [&](const PlacedUnit& e) {
      if (hit || !e.hasShape) return;
      const Vec3& mn = e.mn;
      const Vec3& mx = e.mx;
      if (bmn.x > mx.x || mn.x > bmx.x || bmn.y > mx.y || mn.y > bmx.y || bmn.z > mx.z || mn.z > bmx.z) return;
      if (!GridCellsMeet(bmn, bmx, mn, mx)) return;
      if (!e.alive) return;
      if (placement::SkirtsMeet(e.skirt, R)) hit = true;
    });
    return hit;
  }
};

double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ---- Update's own state ---------------------------------------------------------------------------------------------

struct SyncSnapshots::Builder {
  bool started = false;
  // placement
  std::shared_ptr<const BpTable> bps;
  SnapMap map;
  bool hasMap = false;
  size_t heightChangesSeen = 0;
  uint64_t heightWrites = 0;
  std::vector<Band8> occLand, occWater;
  std::vector<uint64_t> occStamps;  // per band: the stamp it was copied at (0: not copied)
  std::shared_ptr<const std::vector<ResourceDeposit>> deposits;
  // reclaim
  std::shared_ptr<const CellTable> table;
  std::shared_ptr<const std::vector<std::string>> names;
  std::unordered_map<const BlueprintInfo*, uint32_t> nameOf;
  uint32_t nextSeq = 0;
  bool unitsWalked = false;      // the first Update looked at every unit
  std::vector<uint32_t> structs;  // the handles of the not-mobile units (Units)
  bool walked = false;   // the first Update walked every prop
  size_t roll = 0;       // the window's place in the props
  long long liveIn = 0;  // props with an entry
  size_t missed = 0;     // (MOHO64_SNAPSHOT_CHECK) changes nothing noted
  struct CellOps {
    std::vector<uint32_t> remove;
    std::vector<ReclaimEntry> add;
  };
  std::vector<CellOps> ops;   // per cell (kept: their buffers are reused)
  std::vector<uint8_t> opsMark;  // the cell is in `touched`
  std::vector<int> touched;      // the cells with ops this Update
  CellOps& OpsAt(int c) {
    if (!opsMark[static_cast<size_t>(c)]) opsMark[static_cast<size_t>(c)] = 1, touched.push_back(c);
    return ops[static_cast<size_t>(c)];
  }

  void Start(Sim& sim, Costs& costs);
  void Heights(const Sim& sim, Costs& costs);
  void Occupancy(const Sim& sim, Costs& costs, PlacementSnapshot& out);
  void Deposits(const Sim& sim);
  void Units(Sim& sim, PlacementSnapshot& out, Costs& costs);
  std::shared_ptr<const ReclaimSnapshot> Reclaim(Sim& sim, uint32_t tick, Costs& costs);
};

void SyncSnapshots::Builder::Start(Sim& sim, Costs&) {
  started = true;
  // every unit blueprint's footprint and placement data, as BuildSiteOf reads them (ViewReadScope: the blueprint
  // tables read without filling the sim's own cache, through reads that make nothing in the Lua state)
  auto t = std::make_shared<BpTable>();
  const SimBlueprints& all = sim.blueprints();
  if (sim.L()) {
    ViewReadScope view;
    for (int i = 0; i < all.EntityCount(); ++i) {
      const BlueprintInfo* bp = all.ByEntityIndex(i);
      if (!bp || bp->kind != BpKind::Unit || all.Find(bp->id) != bp) continue;
      t->byId.emplace(bp->id, PlaceBp{placement::FootprintOf(*bp), GetUnitBpData(sim.L(), *bp)});
    }
  }
  bps = std::move(t);
  const TerrainMap* m = sim.map();
  hasMap = m != nullptr;
  if (m) {
    map.w = m->width();
    map.h = m->height();
    map.scale = m->heightScale();
    auto types = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(map.w) * map.h);
    for (int z = 0; z < map.h; ++z)
      for (int x = 0; x < map.w; ++x) (*types)[static_cast<size_t>(z) * map.w + x] = m->TerrainType(x, z);
    map.types = std::move(types);
    map.bands.assign(static_cast<size_t>(map.h + 1 + kBandRows - 1) / kBandRows, nullptr);
    heightChangesSeen = m->heightChanges().size();
    heightWrites = m->changes();
  }
}

void SyncSnapshots::Builder::Heights(const Sim& sim, Costs& costs) {
  const TerrainMap* m = sim.map();
  if (!m) return;
  map.hasWater = m->hasWater;
  map.waterElevation = m->waterElevation;
  std::vector<bool> dirty(map.bands.size(), false);
  for (size_t b = 0; b < map.bands.size(); ++b)
    if (!map.bands[b]) dirty[b] = true;
  const auto& ch = m->heightChanges();
  for (size_t i = heightChangesSeen; i < ch.size(); ++i)
    for (int b = ch[i].z0 / kBandRows; b <= ch[i].z1 / kBandRows && b < static_cast<int>(dirty.size()); ++b)
      if (b >= 0) dirty[static_cast<size_t>(b)] = true;
  if (m->changes() != heightWrites && ch.size() == heightChangesSeen) dirty.assign(dirty.size(), true);  // (unnoted)
  heightChangesSeen = ch.size();
  heightWrites = m->changes();
  const size_t stride = static_cast<size_t>(map.w) + 1;
  for (size_t b = 0; b < map.bands.size(); ++b) {
    if (!dirty[b]) continue;
    const int r0 = static_cast<int>(b) * kBandRows, r1 = std::min(map.h + 1, r0 + kBandRows);
    auto band = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(r1 - r0) * stride);
    for (int r = r0; r < r1; ++r)
      std::memcpy(band->data() + static_cast<size_t>(r - r0) * stride, m->heightRow(r), stride * sizeof(uint16_t));
    map.bands[b] = std::move(band);
    ++costs.heightBandsCopied;
  }
}

void SyncSnapshots::Builder::Occupancy(const Sim& sim, Costs& costs, PlacementSnapshot& out) {
  const Navigation* nav = sim.NavigationIfMade();
  out.hasNav = nav != nullptr;
  if (!nav || !hasMap) return;
  out.blocking = nav->BlockingTypes();
  if (!nav->OccRow(0, false)) return;  // (no structure yet: no grid)
  const int W = map.w, H = map.h;
  const size_t n = static_cast<size_t>(H + kBandRows - 1) / kBandRows;
  if (occLand.size() != n) occLand.assign(n, nullptr), occWater.assign(n, nullptr), occStamps.assign(n, 0);
  for (size_t b = 0; b < n; ++b) {
    const uint64_t stamp = nav->OccBandStamp(static_cast<int>(b));
    if (occLand[b] && occStamps[b] == stamp) continue;
    const int r0 = static_cast<int>(b) * kBandRows, r1 = std::min(H, r0 + kBandRows);
    auto land = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(r1 - r0) * W);
    auto water = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(r1 - r0) * W);
    for (int r = r0; r < r1; ++r) {
      std::memcpy(land->data() + static_cast<size_t>(r - r0) * W, nav->OccRow(r, false), static_cast<size_t>(W));
      std::memcpy(water->data() + static_cast<size_t>(r - r0) * W, nav->OccRow(r, true), static_cast<size_t>(W));
    }
    occLand[b] = std::move(land);
    occWater[b] = std::move(water);
    occStamps[b] = stamp;
    ++costs.occBandsCopied;
  }
  out.occAny = true;
  out.occW = W;
  out.occH = H;
  out.occLand = occLand;
  out.occWater = occWater;
}

void SyncSnapshots::Builder::Deposits(const Sim& sim) {
  auto same = [](const ResourceDeposit& a, const ResourceDeposit& b) {
    return a.x0 == b.x0 && a.z0 == b.z0 && a.x1 == b.x1 && a.z1 == b.z1 && a.type == b.type;
  };
  const std::vector<ResourceDeposit>& d = sim.deposits;
  bool changed = !deposits || deposits->size() != d.size();
  for (size_t i = 0; !changed && i < d.size(); ++i) changed = !same((*deposits)[i], d[i]);
  if (changed) deposits = std::make_shared<const std::vector<ResourceDeposit>>(d);
}

void SyncSnapshots::Builder::Units(Sim& sim, PlacementSnapshot& out, Costs& costs) {
  const int gw = UnitGridW(sim), gh = UnitGridH(sim);
  out.gridW = gw;
  out.gridH = gh;
  const EntityGrid* eg = sim.EntityGridIfMade();
  out.egridReady = eg && eg->ready();
  // the not-mobile units RebuildUnitGrid would put in the grid (not destroy-queued), by cell. Which units are not
  // mobile is kept: every unit at the first Update, then the units made since (Sim::snapUnitsMade); a unit's motion type
  // is its blueprint's. Reading only them, not the thousands of mobile ones, is most of the saving; keeping an unchanged
  // structure's entry from the beat before saved nothing (measured), so each is read again.
  auto structure = [](const Unit* u) { return !(u->motion.bp && u->motion.bp->mobile()); };
  if (!unitsWalked) {
    structs.clear();
    for (const Unit* u : sim.units())
      if (structure(u)) structs.push_back(u->handle);
    unitsWalked = true;
  } else {
    for (uint32_t h : sim.snapUnitsMade) {
      Entity* e = sim.FindEntity(h);
      if (e && e->kind == Entity::Kind::Unit && structure(static_cast<Unit*>(e))) structs.push_back(h);
    }
  }
  sim.snapUnitsMade.clear();
  std::vector<std::pair<uint32_t, PlacedUnit>> found;
  found.reserve(structs.size());
  lua_State* L = sim.L();
  ViewReadScope view;
  size_t keep = 0;
  for (size_t ui = 0; ui < structs.size(); ++ui) {
    Entity* ent = sim.FindEntity(structs[ui]);
    if (!ent || ent->kind != Entity::Kind::Unit) continue;  // (gone: dropped from the list)
    structs[keep++] = structs[ui];
    if (ui + 8 < structs.size())  // (speed only: the units lie apart in memory; their lines asked for ahead)
      if (const Entity* q = sim.FindEntity(structs[ui + 8])) __builtin_prefetch(&q->position);
    Unit* u = static_cast<Unit*>(ent);
    if (u->destroyQueued || !u->blueprint) continue;
    PlacedUnit e;
    e.px = u->position.x;
    e.pz = u->position.z;
    const int cx = std::clamp(static_cast<int>(u->position.x) >> 4, 0, gw - 1);
    const int cz = std::clamp(static_cast<int>(u->position.z) >> 4, 0, gh - 1);
    e.skirt = placement::SkirtRectOf(placement::FootprintOf(*u->blueprint), u->bpData ? *u->bpData : GetUnitBpData(L, *u->blueprint),
                                     u->position.x, u->position.z);
    e.gx0 = u->gridX0, e.gz0 = u->gridZ0, e.gw = u->gridW, e.gh = u->gridH;
    e.gridSeen = u->gridSeen;
    e.alive = !u->dead && !u->destroyQueued;
    WorldShape s;
    e.hasShape = GetWorldShape(u, &s);
    if (e.hasShape) ShapeBounds(s, &e.mn, &e.mx);
    found.emplace_back(static_cast<uint32_t>(cz) * gw + cx, e);
  }
  structs.resize(keep);
  static const bool check = getenv("MOHO64_SNAPSHOT_CHECK") != nullptr;
  if (check) {  // MOHO64_SNAPSHOT_CHECK=1: the list against every unit
    size_t n = 0;
    for (const Unit* u : sim.units())
      if (!u->destroyQueued && u->blueprint && structure(u)) ++n;
    if (n != found.size() && missed++ < 20)
      Logf(LogLevel::Info, "snapshots: tick %u: %zu not-mobile units listed, %zu in the simulation", sim.tick(), found.size(), n);
  }
  auto start = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(gw) * gh + 1, 0);
  for (const auto& f : found) ++(*start)[f.first + 1];
  for (size_t i = 1; i < start->size(); ++i) (*start)[i] += (*start)[i - 1];
  auto units = std::make_shared<std::vector<PlacedUnit>>(found.size());
  std::vector<uint32_t> fill(start->begin(), start->end() - 1);
  for (const auto& f : found) (*units)[fill[f.first]++] = f.second;
  costs.structures = found.size();
  out.units = std::move(units);
  out.cellStart = std::move(start);
}

std::shared_ptr<const ReclaimSnapshot> SyncSnapshots::Builder::Reclaim(Sim& sim, uint32_t tick, Costs& costs) {
  // The copy's cells are 8 x 8 map units, a quarter of the prop grid's (RebuildPropGrid: 16 x 16), so that the cells
  // made anew when props change (falling trees, sinking wrecks in a forest fight: some hundred a beat) are small. Cell
  // (cx, cz) here lies in the prop grid's (cx >> 1, cz >> 1), clamped the same way at both ends.
  const int gw = UnitGridW(sim) * 2, gh = UnitGridH(sim) * 2;
  if (!table || table->gw != gw || table->gh != gh) {
    auto t = std::make_shared<CellTable>();
    t->gw = gw;
    t->gh = gh;
    t->bw = (gw + kBlock - 1) / kBlock;
    t->blocks.assign(static_cast<size_t>(t->bw) * ((gh + kBlock - 1) / kBlock), nullptr);
    table = std::move(t);
    if (!names) names = std::make_shared<const std::vector<std::string>>();
    ops.assign(static_cast<size_t>(gw) * gh, CellOps{});
    opsMark.assign(static_cast<size_t>(gw) * gh, 0);
    touched.clear();
  }
  bool any = false;
  for (const auto& [id, cell] : sim.snapPropsGone) {
    if (cell >= 0 && static_cast<size_t>(cell) < ops.size()) OpsAt(cell).remove.push_back(id);
    --liveIn;
    any = true;
  }
  sim.snapPropsGone.clear();
  std::shared_ptr<std::vector<std::string>> newNames;
  const auto scan0 = std::chrono::steady_clock::now();
  // A prop's entry follows what it is now: in (not destroy-queued, a RECLAIMABLE blueprint, a script object), its place,
  // its box (the blueprint's size times its scale) and its cell. Returns whether its entry is as it is (nothing to do).
  auto consider = [&](Prop* p) {
    if (!p->snapSeen) {
      p->snapSeen = true;
      p->snapSeq = nextSeq++;
    }
    const bool in = !p->destroyQueued && p->blueprint && p->HasLuaObject() && p->blueprint->reclaimable;
    if (!in) {
      if (!p->snapIn) return true;
      OpsAt(p->snapCell).remove.push_back(p->id);
      p->snapIn = false;
      --liveIn;
      any = true;
      return false;
    }
    const float at[5] = {p->position.x, p->position.y, p->position.z, p->blueprint->sizeX * p->scale[0],
                         p->blueprint->sizeZ * p->scale[2]};
    const int cx = std::clamp(static_cast<int>(at[0]) >> 3, 0, gw - 1);
    const int cz = std::clamp(static_cast<int>(at[2]) >> 3, 0, gh - 1);
    const int cell = cz * gw + cx;
    // (Sim::collectPropsSeen: the set of the armies that have seen it; PropsSeenBeat notes a prop whose set grew)
    const uint32_t armies = sim.collectPropsSeen ? PropSeenBy(sim, p->id) : 0xffffffffu;
    if (p->snapIn && p->snapCell == cell && std::memcmp(at, p->snapAt, sizeof at) == 0 && p->snapArmies == armies)
      return true;
    if (p->snapIn) OpsAt(p->snapCell).remove.push_back(p->id);
    else ++liveIn;
    ReclaimEntry e;
    e.id = p->id;
    e.seq = p->snapSeq;
    auto it = nameOf.find(p->blueprint);
    if (it == nameOf.end()) {
      if (!newNames) newNames = std::make_shared<std::vector<std::string>>(*names);
      it = nameOf.emplace(p->blueprint, static_cast<uint32_t>(newNames->size())).first;
      newNames->push_back(p->blueprint->id);
    }
    e.name = it->second;
    e.x = at[0], e.y = at[1], e.z = at[2], e.sx = at[3], e.sz = at[4];
    e.armies = armies;
    OpsAt(cell).add.push_back(e);
    p->snapIn = true;
    p->snapCell = cell;
    std::memcpy(p->snapAt, at, sizeof at);
    p->snapArmies = armies;
    any = true;
    return false;
  };
  const std::vector<Prop*>& props = sim.props;
  static const bool check = getenv("MOHO64_SNAPSHOT_CHECK") != nullptr;
  if (!walked) {
    // the first: every prop, in the props' order (their order numbers: the ties of the reclaim cursor)
    for (Prop* p : props) consider(p);
    walked = true;
  } else {
    // then the props the simulation noted (Sim::NotePropTouched: made, moved, rescaled, warped, attached, destroy-queued;
    // in the order noted, which is the props' order for the new ones), and a window of the others in turn: every prop is
    // looked at again within 256 beats, whatever changed it (MOHO64_SNAPSHOT_CHECK found nothing the notes missed)
    for (uint32_t h : sim.snapPropsTouched) {
      Entity* e = sim.FindEntity(h);
      if (e && e->kind == Entity::Kind::Prop) consider(static_cast<Prop*>(e));
    }
    const size_t n = props.size();
    const size_t k = std::min(n, std::max<size_t>(64, (n + 255) / 256));
    for (size_t i = 0; i < k; ++i) {
      if (roll >= n) roll = 0;
      if (!consider(props[roll]) && check) {
        if (missed++ < 20)
          Logf(LogLevel::Info, "snapshots: tick %u: prop %#x (%s) changed unnoted (found by the window)", sim.tick(),
               props[roll]->id, props[roll]->blueprint ? props[roll]->blueprint->id.c_str() : "?");
      }
      ++roll;
    }
    if (check)  // MOHO64_SNAPSHOT_CHECK=1: every prop again, each change found here was noted by nothing
      for (Prop* p : props)
        if (!consider(p) && missed++ < 20)
          Logf(LogLevel::Info, "snapshots: tick %u: prop %#x (%s) changed unnoted", sim.tick(), p->id,
               p->blueprint ? p->blueprint->id.c_str() : "?");
  }
  sim.snapPropsTouched.clear();
  costs.reclaimables = static_cast<size_t>(liveIn);
  costs.propsMissed = missed;
  costs.scanMsTotal += MsSince(scan0);
  if (newNames) names = std::move(newNames);
  if (any) {
    auto t = std::make_shared<CellTable>(*table);
    std::vector<std::pair<size_t, std::shared_ptr<CellBlock>>> made;  // (block index, its new copy)
    for (int c : touched) {
      CellOps& o = ops[static_cast<size_t>(c)];
      if (o.remove.empty() && o.add.empty()) continue;
      const int cx = c % gw, cz = c / gw;
      const size_t bi = static_cast<size_t>(cz / kBlock) * t->bw + cx / kBlock;
      const size_t slot = static_cast<size_t>(cz % kBlock) * kBlock + cx % kBlock;
      CellBlock* nb = nullptr;
      for (auto& m : made)
        if (m.first == bi) nb = m.second.get();
      if (!nb) {
        made.emplace_back(bi, t->blocks[bi] ? std::make_shared<CellBlock>(*t->blocks[bi]) : std::make_shared<CellBlock>());
        nb = made.back().second.get();
      }
      const Cell* old = nb->cells[slot].get();
      auto n = std::make_shared<Cell>();
      n->reserve((old ? old->size() : 0) + o.add.size());
      if (old)
        for (const ReclaimEntry& e : *old)
          if (std::find(o.remove.begin(), o.remove.end(), e.id) == o.remove.end()) n->push_back(e);
      for (const ReclaimEntry& e : o.add)
        n->insert(std::upper_bound(n->begin(), n->end(), e, [](const ReclaimEntry& x, const ReclaimEntry& y) { return x.seq < y.seq; }), e);
      nb->cells[slot] = n->empty() ? nullptr : std::shared_ptr<const Cell>(std::move(n));
      ++costs.cellsCopied;
    }
    for (auto& m : made) t->blocks[m.first] = std::move(m.second);
    table = std::move(t);
  }
  for (int c : touched) {
    ops[static_cast<size_t>(c)].remove.clear();
    ops[static_cast<size_t>(c)].add.clear();
    opsMark[static_cast<size_t>(c)] = 0;
  }
  touched.clear();
  auto r = std::make_shared<ReclaimSnapshot>();
  r->tick = tick;
  r->table = table;
  r->names = names;
  return r;
}

// ---- SyncSnapshots --------------------------------------------------------------------------------------------------

SyncSnapshots::SyncSnapshots() : b_(std::make_unique<Builder>()) {}
SyncSnapshots::~SyncSnapshots() = default;

void SyncSnapshots::Update(Sim& sim) {
  const auto t0 = std::chrono::steady_clock::now();
  Builder& b = *b_;
  costs_.heightBandsCopied = costs_.occBandsCopied = costs_.cellsCopied = 0;
  if (!b.started) b.Start(sim, costs_);
  auto p = std::make_shared<PlacementSnapshot>();
  p->tick = sim.tick();
  b.Heights(sim, costs_);
  p->hasMap = b.hasMap;
  p->map = b.map;
  p->bps = b.bps;
  b.Occupancy(sim, costs_, *p);
  b.Deposits(sim);
  p->deposits = b.deposits;
  const auto tu = std::chrono::steady_clock::now();
  b.Units(sim, *p, costs_);
  const auto tr = std::chrono::steady_clock::now();
  std::shared_ptr<const ReclaimSnapshot> r = b.Reclaim(sim, sim.tick(), costs_);
  const auto te = std::chrono::steady_clock::now();
  const double unitsMs = std::chrono::duration<double, std::milli>(tr - tu).count();
  const double reclaimMs = std::chrono::duration<double, std::milli>(te - tr).count();
  costs_.unitsMsTotal += unitsMs;
  costs_.reclaimMsTotal += reclaimMs;
  {
    std::lock_guard<std::mutex> lock(m_);
    placement_ = std::move(p);
    reclaim_ = std::move(r);
  }
  tick_.store(sim.tick(), std::memory_order_release);
  const double ms = MsSince(t0);
  // MOHO64_SNAPSHOT_SLOW=<ms>: every Update longer than that, with its parts (for measurements)
  static const double slow = getenv("MOHO64_SNAPSHOT_SLOW") ? std::atof(getenv("MOHO64_SNAPSHOT_SLOW")) : 0.0;
  if (slow > 0 && ms > slow && costs_.updates > 0)
    Logf(LogLevel::Info, "snapshots: tick %u: a long update, %.3f ms (units %.3f, reclaim %.3f); %zu height bands, %zu "
         "occupancy bands, %zu cells made anew", sim.tick(), ms, unitsMs, reclaimMs, costs_.heightBandsCopied,
         costs_.occBandsCopied, costs_.cellsCopied);
  if (costs_.updates == 0) costs_.firstMs = ms;
  else costs_.maxMs = std::max(costs_.maxMs, ms);  // (the first, with the blueprints and the whole map, apart)
  costs_.lastMs = ms;
  costs_.totalMs += ms;
  ++costs_.updates;
  static const bool bytesEveryTime = getenv("MOHO64_SNAPSHOT_BYTES") != nullptr;
  if (bytesEveryTime || costs_.updates == 1) {  // (a walk over the parts: measurements only)
    const PlacementSnapshot& s = *placement_;
    size_t pb = s.map.bands.size() * (static_cast<size_t>(kBandRows) * (s.map.w + 1) * 2) + (s.map.types ? s.map.types->size() : 0) +
                s.occLand.size() * 2 * static_cast<size_t>(kBandRows) * s.occW + (s.units ? s.units->size() * sizeof(PlacedUnit) : 0) +
                (s.cellStart ? s.cellStart->size() * 4 : 0) + (s.deposits ? s.deposits->size() * sizeof(ResourceDeposit) : 0) +
                (s.bps ? s.bps->byId.size() * sizeof(PlaceBp) : 0);
    costs_.placementBytes = pb;
    size_t rb = 0;
    if (reclaim_->table)
      for (const auto& b : reclaim_->table->blocks)
        if (b) {
          rb += sizeof(CellBlock);
          for (const auto& c : b->cells)
            if (c) rb += c->capacity() * sizeof(ReclaimEntry) + 32;
        }
    costs_.reclaimBytes = rb;
  }
}

std::shared_ptr<const PlacementSnapshot> SyncSnapshots::Placement() const {
  std::lock_guard<std::mutex> lock(m_);
  return placement_;
}
std::shared_ptr<const ReclaimSnapshot> SyncSnapshots::Reclaim() const {
  std::lock_guard<std::mutex> lock(m_);
  return reclaim_;
}

// ---- the queries ----------------------------------------------------------------------------------------------------

bool BuildSiteOf(const PlacementSnapshot& snap, const PlacementQuery& q, float site[3], bool* canBuild) {
  SimFpScope fp;  // (the simulation's floating-point state: the answers are the sim's to the bit)
  if (!snap.bps) return false;
  std::string id = q.blueprint;
  for (auto& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));  // (SimBlueprints::Find)
  auto it = snap.bps->byId.find(id);
  if (it == snap.bps->byId.end()) return false;
  const PlaceBp& b = it->second;
  SnapPlaceWorld w{snap};
  const Vec3 s = placement::SnapStructurePosition(w, b.fp, Vec3{q.position[0], q.position[1], q.position[2]});
  site[0] = s.x;
  site[1] = s.y;
  site[2] = s.z;
  if (canBuild) *canBuild = placement::CanBuildStructureAt(w, b.fp, b.d, s.x, s.z);
  return true;
}
uint32_t SnapshotTick(const PlacementSnapshot& snap) { return snap.tick; }

bool ReclaimableAt(const ReclaimSnapshot& snap, const float at[3], float within, ReclaimableView* out,
                   uint32_t armyMask) {
  SimFpScope fp;
  if (!snap.table) return false;
  const CellTable& t = *snap.table;
  // sim/sim_view.cpp ReclaimableAt over Sim::ForPropsInRect, on the copies: the prop grid's cells (16 x 16) the rect
  // touches, each as the two by two cells of the copy within it. ForPropsInRect gives them row by row, and within a
  // cell in the props' order, and of props at the same distance the first given wins: so here the order is
  // (the prop grid's cell, row by row; the prop's order number).
  const float reach = (within > 0 ? within : 0.0f) + 16.0f;  // (no prop's box is wider than that)
  const float x0 = at[0] - reach, z0 = at[2] - reach, x1 = at[0] + reach, z1 = at[2] + reach;
  if (x1 < 0 || z1 < 0) return false;
  const int gw16 = t.gw / 2, gh16 = t.gh / 2;
  int cx0 = std::max(0, static_cast<int>(x0) >> 4), cz0 = std::max(0, static_cast<int>(z0) >> 4);
  int cx1 = std::min(gw16 - 1, static_cast<int>(x1) >> 4), cz1 = std::min(gh16 - 1, static_cast<int>(z1) >> 4);
  const ReclaimEntry* best = nullptr;
  float bestEdge = 0, bestCentre = 0;
  int bestCell = 0;
  for (int cz = cz0 * 2; cz <= cz1 * 2 + 1; ++cz)
    for (int cx = cx0 * 2; cx <= cx1 * 2 + 1; ++cx) {
      const Cell* c = t.At(cx, cz);
      if (!c) continue;
      const int cell16 = (cz >> 1) * gw16 + (cx >> 1);
      for (const ReclaimEntry& p : *c) {
        if (armyMask && !(p.armies & armyMask)) continue;  // (a wreck none of the mask's armies had seen)
        const float sx = p.sx, sz = p.sz;
        const float dx = std::fabs(at[0] - p.x), dz = std::fabs(at[2] - p.z);
        const float ex = std::max(0.0f, dx - sx * 0.5f), ez = std::max(0.0f, dz - sz * 0.5f);
        const float edge = std::sqrt(ex * ex + ez * ez), centre = std::sqrt(dx * dx + dz * dz);
        if (edge > (within > 0 ? within : 0.0f)) continue;
        if (best) {
          if (edge > bestEdge || (edge == bestEdge && centre > bestCentre)) continue;
          if (edge == bestEdge && centre == bestCentre &&  // (a tie: the one ForPropsInRect gives first)
              (cell16 > bestCell || (cell16 == bestCell && p.seq > best->seq)))
            continue;
        }
        best = &p;
        bestEdge = edge;
        bestCentre = centre;
        bestCell = cell16;
      }
    }
  if (!best) return false;
  if (out) {
    out->id = best->id;
    out->blueprint = snap.names && best->name < snap.names->size() ? (*snap.names)[best->name] : std::string();
    out->position[0] = best->x;
    out->position[1] = best->y;
    out->position[2] = best->z;
    out->sizeX = best->sx;
    out->sizeZ = best->sz;
    out->mass = -1;  // (not copied: the prop's script table holds them)
    out->energy = -1;
  }
  return true;
}
uint32_t SnapshotTick(const ReclaimSnapshot& snap) { return snap.tick; }

}  // namespace moho
