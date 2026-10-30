// Pathfinding: where a footprint can stand (the original's STIMap::OCCUPY_MobileCheck) and paths
// between cells.
//
// Passability follows the original (FA exe 0x564ab0, read 2026-10-08): a footprint whose origin
// cell is (x, z) covers the heightfield vertices x..x+sizeX, z..z+sizeZ. It may stand there if
// no covered cell has a blocking terrain type and, for the land/seabed layers, the deepest water
// under it is at most MaxWaterDepth and the largest height step between neighbouring vertices is
// at most MaxSlope; water layers need at least MinWaterDepth everywhere under it.
//
// Searching: moves use the original's hierarchical search and land navigator (sim/hpath.cpp,
// sim/landnav.cpp). FindPath below (a plain A* with line-of-sight smoothing) only answers
// CanPathTo (TODO: the original's direct probe, pathfinding.md section 6).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/blueprints.h"
#include "sim/entity.h"

namespace moho {

class TerrainMap;
class WorkerPool;
struct GridScratch;  // MobileCheckCapsRect's buffers (navigation.cpp)

// Layer bits as in /lua/footprints.lua.
enum : uint8_t { kLayerLand = 1, kLayerSeabed = 2, kLayerSub = 4, kLayerWater = 8, kLayerAir = 16 };
// The two structure occupancy grids of the original's COGrid (air_extra.md 3.3): "land" (ogrid+0x38) holds the
// structures whose footprint caps & 7, "water" (ogrid+0x48) those whose caps & 8 (COGrid::ExecuteOccupy 0x721a90).
enum : uint8_t { kOccLand = 1, kOccWater = 2 };
inline uint8_t OccLayersOf(uint8_t footprintCaps) {
  return static_cast<uint8_t>(((footprintCaps & 7) ? kOccLand : 0) | ((footprintCaps & 8) ? kOccWater : 0));
}
// STIMap::OCCUPY_FootprintFits 0x7209e0 on terrain caps: Land and Seabed go where the land grid holds a structure
// (unless the footprint has IgnoreStructures), Water where the water grid does; Sub stays.
inline uint8_t FitsCaps(uint8_t caps, bool ignoreStructures, bool landHeld, bool waterHeld) {
  if (!ignoreStructures && (caps & 3) && landHeld) caps &= static_cast<uint8_t>(~3);
  if ((caps & 8) && waterHeld) caps &= static_cast<uint8_t>(~8);
  return caps;
}

class PathGrid {
 public:
  // Every cell's caps taken at once by MobileCheckCapsRect (navigation.cpp), in bands of rows on `pool` when one is
  // given and this is not inside a parallel stage. `scratch`: the buffers its later terrain changes use (Navigation's).
  PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blockingTypes, GridScratch* scratch = nullptr,
           WorkerPool* pool = nullptr);
  int width() const { return w_; }
  int height() const { return h_; }
  // Can the footprint's origin cell be (x, z)? (terrain, and no structure in the way: OCCUPY_FootprintFits)
  // `drop`: layer bits taken from the terrain caps before the structures are counted (the path navigator's Reach
  // drops Sub for a unit on the Water layer, 0x5af566 / 0x720bfa).
  bool Passable(int x, int z, uint8_t drop = 0) const {
    if (x < 0 || z < 0 || x >= w_ || z >= h_) return false;
    if (pendingAny_) ApplyTerrainChanges();
    size_t i = static_cast<size_t>(z) * w_ + x;
    return FitsCaps(static_cast<uint8_t>(cells_[i] & ~drop), ignoreStructures, blocked_[i] != 0,
                    blockedWater_[i] != 0) != 0;
  }
  // Layers the terrain allows at (x, z): OCCUPY_MobileCheck's result (structures not counted).
  uint8_t Caps(int x, int z) const {
    if (x < 0 || z < 0 || x >= w_ || z >= h_) return 0;
    if (pendingAny_) ApplyTerrainChanges();
    return cells_[static_cast<size_t>(z) * w_ + x];
  }
  // How many held cells (of either grid) the footprint at origin (x, z) covers (FindPath's escape test).
  uint16_t Blocked(int x, int z) const {
    if (x < 0 || z < 0 || x >= w_ || z >= h_) return 0;
    const size_t i = static_cast<size_t>(z) * w_ + x;
    return static_cast<uint16_t>(blocked_[i] + blockedWater_[i]);
  }
  // A structure of the land / water grid overlaps the footprint at origin (x, z).
  bool BlockedLand(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? blocked_[static_cast<size_t>(z) * w_ + x] != 0 : false;
  }
  bool BlockedWater(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? blockedWater_[static_cast<size_t>(z) * w_ + x] != 0 : false;
  }
  // A structure covering cells [x0, x1) x [z0, z1) in the given occupancy grids (kOccLand / kOccWater) holds every
  // origin whose footprint overlaps it.
  void Block(int x0, int z0, int x1, int z1, int delta, uint8_t occ);
  // The heightfield vertices [vx0, vx1] x [vz0, vz1] changed (FlattenMapRect): the terrain caps of every origin
  // whose footprint covers one of them are to be taken again from the map. They are taken when the grid is next read
  // (Passable, Caps), from the map as it is then: a cell's caps are a function of the map alone (OCCUPY_MobileCheck
  // over its footprint's vertices and their terrain types), so the cells read are the ones each call taking them at
  // once gave. The rects of a run of calls with nothing reading in between are merged where they meet: FAF's
  // FlattenGradientMapRect makes a factory's skirt with one call per vertex (81 for a land factory), and each of
  // those took (sizeX + 1) x (sizeZ + 1) origins of every grid again. (Single-threaded reads only: a parallel stage
  // that reads the grids calls ApplyTerrainChanges first.)
  void TerrainChanged(int vx0, int vz0, int vx1, int vz1);
  void ApplyTerrainChanges() const;
  const NamedFootprint& footprint() const { return fp_; }  // the footprint it was made from
  int sizeX = 1, sizeZ = 1;
  bool ignoreStructures = false;  // the footprint's IgnoreStructures flag (Flags & 1)

 private:
  int w_ = 0, h_ = 0;
  NamedFootprint fp_;
  const TerrainMap* map_ = nullptr;
  const bool* blocking_ = nullptr;  // Navigation's blocking terrain types
  GridScratch* scratch_ = nullptr;  // Navigation's (ApplyTerrainChanges runs on one thread)
  struct Rect {
    int x0, z0, x1, z1;  // origins [x0, x1] x [z0, z1]
  };
  mutable std::vector<Rect> pending_;  // origins whose caps are to be taken again (TerrainChanged)
  mutable bool pendingAny_ = false;
  mutable std::vector<uint8_t> cells_;
  std::vector<uint16_t> blocked_;       // land grid
  std::vector<uint16_t> blockedWater_;  // water grid
};

// A structure's cells (footprint rect) in the occupancy grid.
struct OccupiedRect {
  uint32_t entity = 0;
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
  uint8_t occ = kOccLand | kOccWater;  // the occupancy grids it holds
};

class Navigation {
 public:
  explicit Navigation(const TerrainMap* map);
  ~Navigation();
  // The sim's worker pool: a new grid's build splits into bands of rows on it (PathGrid's ctor).
  void SetWorkers(WorkerPool* pool) { pool_ = pool; }
  bool HasMap() const { return map_ != nullptr; }
  void SetBlockingTerrainType(int code, bool blocking) {
    for (auto& [key, g] : grids_) g->ApplyTerrainChanges();  // (what is pending was measured with the old types)
    if (code >= 0 && code < 256) blocking_[code] = blocking;
  }
  const PathGrid* Grid(const NamedFootprint& fp);
  // Every grid's pending terrain changes taken now (PathGrid::TerrainChanged), on one thread: before a stage whose
  // tasks read the grids in parallel, as Passable / Caps would otherwise write them from several threads.
  void ApplyTerrainChanges() {
    for (auto& [key, g] : grids_) g->ApplyTerrainChanges();
  }
  bool IsBlockingType(int code) const { return code >= 0 && code < 256 && blocking_[code]; }
  // OCCUPY_MobileCheck 0x564ab0 at one origin cell (what Grid(fp)->Caps(x, z) holds), without making the grid.
  uint8_t MobileCaps(const NamedFootprint& fp, int x, int z) const;
  // The terrain caps of one cell for the asking footprint itself, as the original reads them at its single-cell
  // tests (no cache: the entity's own footprint, Entity::GetFootprint 0x678880, on the live map), without making a
  // grid. A grid's key holds name, size, caps and IgnoreStructures but not MaxSlope / MinWaterDepth /
  // MaxWaterDepth, which differ between structures of one key (e.g. /2x2/8/0: the torpedo launchers' MinWaterDepth
  // 1.5, xrb2308's 2): so the grid of the key is read only when it was made from the same three values (it holds
  // MobileCheckCaps of the map as it is now: its pending terrain changes are taken at the read), else
  // MobileCheckCaps here. callerCapsAll: the caller is OCCUPY_FootprintFits 0x7209e0 with caps -1 (StartingLayer
  // 0x62adca, a transport's drop 0x720aa0): a footprint of size 1 (max(SizeX, SizeZ) == 1) then goes to OCCUPY_Filter
  // 0x720920, whose terrain caps are STIMap::OccupancyCapsOfFootprintAt 0x564df0 (one cell's type), not
  // OCCUPY_MobileCheck 0x564ab0. (A footprint without a named one, a structure's own or an air landing's S x S,
  // cost a whole grid at its first such read: 141-1240 ms, mp2 2026-10-21.)
  uint8_t CellCaps(const NamedFootprint& fp, int x, int z, bool callerCapsAll = false);
  // Waypoints from `from` to `to` (world positions; the footprint's centre). The last waypoint is
  // `to`, or the nearest reachable cell to it. Returns false when no path exists at all.
  bool FindPath(const NamedFootprint& fp, const Vec3& from, const Vec3& to, std::vector<Vec3>* out);
  // Structures: their footprints block movement (and building) until they are gone. fpCaps: the footprint's
  // OccupancyCaps, which pick the occupancy grids (OccLayersOf); none: nothing is held.
  // The original's COGrid keeps one BIT per cell and layer (BitArray2D at ogrid+0x38 land / +0x48 water): occupying
  // sets the rect's bits (ExecuteOccupy 0x721a90 -> FillRect 0x8d8370 with 1), releasing clears them (ReleaseOccupy
  // 0x721b30 -> FillRect with 0), whoever else still stands on those cells. So releasing one of two overlapping
  // occupiers frees the shared cells. rect: the cells to clear (the releaser's own computation, e.g. a prop's rect
  // at its position then); nullptr: the rect it occupied.
  // append: the entity holds several rects (a quantum gateway's Physics.OccupyRects): this one is added to its others.
  void AddStructure(uint32_t entity, int x0, int z0, int x1, int z1, uint8_t fpCaps, bool append = false);
  // Every record of the entity goes; rect given: those cells are cleared, else each record's own.
  void RemoveStructure(uint32_t entity, const int* rect = nullptr);
  // The entity's records go, its cells stay as they are (a unit that no longer counts as occupying ground).
  void ForgetStructure(uint32_t entity);
  const std::vector<OccupiedRect>& Structures() const { return structures_; }
  // Whether AddStructure over [x0, x1) x [z0, z1) with these caps would set a cell's layer bit (SetCells' test).
  bool WouldChangeCells(int x0, int z0, int x1, int z1, uint8_t fpCaps) const;
  // The heightfield vertices [vx0, vx1] x [vz0, vz1] took new heights (FlattenMapRect). The original reads the
  // terrain's caps from the map at every test (CanTraverseCell 0x5aa77d / 0x5aa7e2 call OCCUPY_MobileCheck 0x564ab0
  // with the live STIMap); the grids here keep them, so they take the changed ones again.
  void TerrainChanged(int vx0, int vz0, int vx1, int vz1);
  // Is any structure cell of the given occupancy grids inside [x0, x1) x [z0, z1)?
  bool AnyStructureIn(int x0, int z0, int x1, int z1, uint8_t occ = kOccLand | kOccWater) const;
  // OCCUPY_FootprintFits 0x7209e0 for a footprint at origin (x0, z0) on terrain caps `caps`.
  uint8_t FootprintFits(uint8_t caps, const NamedFootprint& fp, int x0, int z0) const {
    return FitsCaps(caps, (fp.flags & 1) != 0, AnyStructureIn(x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ, kOccLand),
                    AnyStructureIn(x0, z0, x0 + fp.sizeX, z0 + fp.sizeZ, kOccWater));
  }
  // Work of the last search (cells walked or expanded) and totals for the profiler.
  uint64_t lastWork = 0, searches = 0, expanded = 0;

 private:
  const TerrainMap* map_;
  std::array<bool, 256> blocking_;
  std::map<std::string, std::unique_ptr<PathGrid>> grids_;
  WorkerPool* pool_ = nullptr;
  std::unique_ptr<GridScratch> scratch_;  // the grids' terrain changes (one thread)
  std::map<std::string, std::pair<uint64_t, uint64_t>> cellCounts_;  // MOHO64_DEBUG_GRIDCHECK: CellCaps (grid, none)
  std::vector<OccupiedRect> structures_;
  std::vector<uint8_t> occ_;       // land grid: the occupancy bit of each map cell
  std::vector<uint8_t> occWater_;  // water grid
  // Set (on) or clear the bits of [x0, x1) x [z0, z1) in the grids `occ`; each origin's count of held cells in its
  // footprint (PathGrid::blocked_) follows every bit that changes.
  void SetCells(int x0, int z0, int x1, int z1, uint8_t occ, bool on);
};

}  // namespace moho
