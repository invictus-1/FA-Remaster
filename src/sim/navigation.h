// Pathfinding: where a footprint can stand (the original's STIMap::OCCUPY_MobileCheck) and paths
// between cells.
//
// Passability follows the original (FA exe 0x564ab0, read 2026-10-08): a footprint whose origin
// cell is (x, z) covers the heightfield vertices x..x+sizeX, z..z+sizeZ. It may stand there if
// no covered cell has a blocking terrain type and, for the land/seabed layers, the deepest water
// under it is at most MaxWaterDepth and the largest height step between neighbouring vertices is
// at most MaxSlope; water layers need at least MinWaterDepth everywhere under it.
//
// Searching: the original uses a hierarchical A* (gpgcore hastar). Ours is a plain A* over cells
// with the original's octile heuristic, followed by line-of-sight smoothing into waypoints.
// TODO(M3b): hierarchical search for long paths; structures in the occupancy grid (M4).
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
  // Can the footprint's origin cell be (x, z)?
  bool Passable(int x, int z) const {
    return x >= 0 && z >= 0 && x < w_ && z < h_ && cells_[static_cast<size_t>(z) * w_ + x] != 0;
  }
  // Layers usable at (x, z): OCCUPY_MobileCheck's result.
  uint8_t Caps(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? cells_[static_cast<size_t>(z) * w_ + x] : 0;
  }

 private:
  int w_ = 0, h_ = 0;
  std::vector<uint8_t> cells_;
};

class Navigation {
 public:
  explicit Navigation(const TerrainMap* map) : map_(map) { blocking_.fill(false); }
  void SetBlockingTerrainType(int code, bool blocking) {
    if (code >= 0 && code < 256) blocking_[code] = blocking;
  }
  const PathGrid* Grid(const NamedFootprint& fp);
  // Waypoints from `from` to `to` (world positions; the footprint's centre). The last waypoint is
  // `to`, or the nearest reachable cell to it. Returns false when no path exists at all.
  bool FindPath(const NamedFootprint& fp, const Vec3& from, const Vec3& to, std::vector<Vec3>* out);
  // Work of the last search (cells walked or expanded) and totals for the profiler.
  uint64_t lastWork = 0, searches = 0, expanded = 0;

 private:
  const TerrainMap* map_;
  std::array<bool, 256> blocking_;
  std::map<std::string, std::unique_ptr<PathGrid>> grids_;
  bool LineOfSight(const PathGrid& g, int x0, int z0, int x1, int z1) const;
};

}  // namespace moho
