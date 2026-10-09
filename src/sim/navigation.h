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

// Layer bits as in /lua/footprints.lua.
enum : uint8_t { kLayerLand = 1, kLayerSeabed = 2, kLayerSub = 4, kLayerWater = 8, kLayerAir = 16 };

class PathGrid {
 public:
  PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blockingTypes);
  int width() const { return w_; }
  int height() const { return h_; }
  // Can the footprint's origin cell be (x, z)? (terrain, and no structure in the way)
  bool Passable(int x, int z) const {
    if (x < 0 || z < 0 || x >= w_ || z >= h_) return false;
    size_t i = static_cast<size_t>(z) * w_ + x;
    return cells_[i] != 0 && blocked_[i] == 0;
  }
  // Layers the terrain allows at (x, z): OCCUPY_MobileCheck's result (structures not counted).
  uint8_t Caps(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? cells_[static_cast<size_t>(z) * w_ + x] : 0;
  }
  uint16_t Blocked(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? blocked_[static_cast<size_t>(z) * w_ + x] : 0;
  }
  // A structure covering cells [x0, x1) x [z0, z1) blocks every origin whose footprint overlaps it.
  void Block(int x0, int z0, int x1, int z1, int delta);
  int sizeX = 1, sizeZ = 1;

 private:
  int w_ = 0, h_ = 0;
  std::vector<uint8_t> cells_;
  std::vector<uint16_t> blocked_;
};

// A structure's cells (footprint rect) in the occupancy grid.
struct OccupiedRect {
  uint32_t entity = 0;
  int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};

class Navigation {
 public:
  explicit Navigation(const TerrainMap* map) : map_(map) { blocking_.fill(false); }
  void SetBlockingTerrainType(int code, bool blocking) {
    if (code >= 0 && code < 256) blocking_[code] = blocking;
  }
  const PathGrid* Grid(const NamedFootprint& fp);
  bool IsBlockingType(int code) const { return code >= 0 && code < 256 && blocking_[code]; }
  // Waypoints from `from` to `to` (world positions; the footprint's centre). The last waypoint is
  // `to`, or the nearest reachable cell to it. Returns false when no path exists at all.
  bool FindPath(const NamedFootprint& fp, const Vec3& from, const Vec3& to, std::vector<Vec3>* out);
  // Structures: their footprints block movement (and building) until they are gone.
  void AddStructure(uint32_t entity, int x0, int z0, int x1, int z1);
  void RemoveStructure(uint32_t entity);
  const std::vector<OccupiedRect>& Structures() const { return structures_; }
  // Is any structure cell inside [x0, x1) x [z0, z1)?
  bool AnyStructureIn(int x0, int z0, int x1, int z1) const;
  // Work of the last search (cells walked or expanded) and totals for the profiler.
  uint64_t lastWork = 0, searches = 0, expanded = 0;

 private:
  const TerrainMap* map_;
  std::array<bool, 256> blocking_;
  std::map<std::string, std::unique_ptr<PathGrid>> grids_;
  std::vector<OccupiedRect> structures_;
  std::vector<uint16_t> occ_;  // structure cells (count), map cells
};

}  // namespace moho
