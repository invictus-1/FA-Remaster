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
  PathGrid(const TerrainMap& map, const NamedFootprint& fp, const bool* blockingTypes);
  int width() const { return w_; }
  int height() const { return h_; }
  // Can the footprint's origin cell be (x, z)? (terrain, and no structure in the way: OCCUPY_FootprintFits)
  bool Passable(int x, int z) const {
    if (x < 0 || z < 0 || x >= w_ || z >= h_) return false;
    size_t i = static_cast<size_t>(z) * w_ + x;
    return FitsCaps(cells_[i], ignoreStructures, blocked_[i] != 0, blockedWater_[i] != 0) != 0;
  }
  // Layers the terrain allows at (x, z): OCCUPY_MobileCheck's result (structures not counted).
  uint8_t Caps(int x, int z) const {
    return (x >= 0 && z >= 0 && x < w_ && z < h_) ? cells_[static_cast<size_t>(z) * w_ + x] : 0;
  }
  // How many structures (of either grid) overlap the footprint at origin (x, z) (FindPath's escape test).
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
  // whose footprint covers one of them are taken again from the map as it is now.
  void TerrainChanged(const TerrainMap& map, const bool* blockingTypes, int vx0, int vz0, int vx1, int vz1);
  int sizeX = 1, sizeZ = 1;
  bool ignoreStructures = false;  // the footprint's IgnoreStructures flag (Flags & 1)

 private:
  int w_ = 0, h_ = 0;
  NamedFootprint fp_;
  std::vector<uint8_t> cells_;
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
  explicit Navigation(const TerrainMap* map) : map_(map) { blocking_.fill(false); }
  void SetBlockingTerrainType(int code, bool blocking) {
    if (code >= 0 && code < 256) blocking_[code] = blocking;
  }
  const PathGrid* Grid(const NamedFootprint& fp);
  bool IsBlockingType(int code) const { return code >= 0 && code < 256 && blocking_[code]; }
  // OCCUPY_MobileCheck 0x564ab0 at one origin cell (what Grid(fp)->Caps(x, z) holds), without making the grid.
  uint8_t MobileCaps(const NamedFootprint& fp, int x, int z) const;
  // Waypoints from `from` to `to` (world positions; the footprint's centre). The last waypoint is
  // `to`, or the nearest reachable cell to it. Returns false when no path exists at all.
  bool FindPath(const NamedFootprint& fp, const Vec3& from, const Vec3& to, std::vector<Vec3>* out);
  // Structures: their footprints block movement (and building) until they are gone. fpCaps: the footprint's
  // OccupancyCaps, which pick the occupancy grids (OccLayersOf); none: nothing is held.
  void AddStructure(uint32_t entity, int x0, int z0, int x1, int z1, uint8_t fpCaps);
  void RemoveStructure(uint32_t entity);
  const std::vector<OccupiedRect>& Structures() const { return structures_; }
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
  std::vector<OccupiedRect> structures_;
  std::vector<uint16_t> occ_;       // land grid: structure cells (count), map cells
  std::vector<uint16_t> occWater_;  // water grid
};

}  // namespace moho
