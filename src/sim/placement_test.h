// The structure placement test (CAiBrain::CanBuildStructureAt 0x57cbb0 and the parts it is made of) written once,
// over "the world it reads": the running Sim (sim/build.cpp's SimPlaceWorld: what the simulation itself asks) or a
// copy of what the test reads, made between beats (sim/sim_snapshot.cpp: the interface's cursor, from any thread).
// Both instantiate the same code, so a snapshot's answer is the simulation's answer at the moment it was made.
//
// A world W gives:
//   const M* map() const          the heightfield (null: no map); M has width(), height(), HeightAt(x, z),
//                                 heightScale(), heightRow(vz) (row vz's raw samples), TerrainHeight(x, z),
//                                 SurfaceHeight(x, z), TerrainType(x, z) and the fields hasWater, waterElevation
//   bool AnyDepositNear(double x0, double z0, double x1, double z1, test)   any deposit d with test(d) (every one
//                                 that could meet the rect is offered: sim/build.cpp DepositCells)
//   uint8_t MobileCaps(fp, x, z)  OCCUPY_MobileCheck 0x564ab0 at the origin cell (Navigation::MobileCaps)
//   uint8_t FootprintFits(caps, fp, x, z)   OCCUPY_FootprintFits 0x7209e0 (Navigation::FootprintFits)
//   bool AnyStructureIn(x0, z0, x1, z1)     a held cell of either occupancy grid in [x0, x1) x [z0, z1)
//   bool AnyStructureSkirtOverlaps(R)       0x720770: a not-mobile unit's skirt meets R (SkirtsMeet below)
//   bool LiveStructureSkirtOverlaps(R)      0x57cda3..0x57cf9f with relation 3 (every army's units count)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "sim/blueprints.h"
#include "sim/entity.h"

namespace moho {
namespace placement {

struct Rect {
  float x0, z0, x1, z1;
};

inline int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }

// The footprint the engine uses for a unit blueprint (BlueprintInfo::footprint), 1 x 1 for one without.
inline const NamedFootprint& FootprintOf(const BlueprintInfo& bp) {
  static const NamedFootprint one{"", 1, 1, 1, 0, 0, 0, 0};
  return bp.hasFootprint ? bp.footprint : one;
}

// RUnitBlueprint::GetSkirtRect 0x51ec50 of a blueprint with footprint fp and placement data d at (x, z).
template <class D>
Rect SkirtRectOf(const NamedFootprint& fp, const D& d, float x, float z) {
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  Rect r;
  if (d.skirtSizeX == 0.0f) {
    r.x0 = static_cast<float>(ox);
    r.x1 = static_cast<float>(ox + fp.sizeX);
  } else {
    r.x0 = d.skirtOffsetX + static_cast<float>(ox);
    r.x1 = r.x0 + d.skirtSizeX;
  }
  if (d.skirtSizeZ == 0.0f) {
    r.z0 = static_cast<float>(oz);
    r.z1 = static_cast<float>(oz + fp.sizeZ);
  } else {
    r.z0 = d.skirtOffsetZ + static_cast<float>(oz);
    r.z1 = r.z0 + d.skirtSizeZ;
  }
  return r;
}

inline int DepositType(const std::string& restriction) {
  if (restriction == "RULEUBR_OnMassDeposit") return 1;
  if (restriction == "RULEUBR_OnHydrocarbonDeposit") return 2;
  return 0;
}

// Two skirts meet (strictly), as 0x720770 and 0x57cf9f test them (0x416c90).
inline bool SkirtsMeet(const Rect& r, const Rect& R) {
  return r.x1 > R.x0 && R.x1 > r.x0 && r.z1 > R.z0 && R.z1 > r.z0 && R.x1 > R.x0 && R.z0 < R.z1 && r.x1 > r.x0 &&
         r.z0 < r.z1;
}
// The collision grid's cells of rect R (0x4fcb40: the rect truncated, 4 x 4 cells, at least one cell each way), and
// whether an entity's cells (Entity::gridX0 ..) share one with them.
struct GridCells {
  int cx0, cz0, cw, ch;
};
inline GridCells GridCellsOf(const Rect& R) {
  auto lo = [](float v) { return std::clamp(static_cast<int>(v) >> 2, 0, 0xffff); };
  auto len = [](float v, int c0) { return std::max(1, std::min(((static_cast<int>(v) + 3) >> 2) - c0, 0xffff - c0)); };
  GridCells g;
  g.cx0 = lo(R.x0);
  g.cz0 = lo(R.z0);
  g.cw = len(R.x1, g.cx0);
  g.ch = len(R.z1, g.cz0);
  return g;
}
inline bool GridCellsShared(const GridCells& g, int gx0, int gz0, int gw, int gh) {
  return gx0 < g.cx0 + g.cw && g.cx0 < gx0 + gw && gz0 < g.cz0 + g.ch && g.cz0 < gz0 + gh;
}

// CSimResources::AreaHasDeposit 0x546860: a deposit of the type, grown by 0.5 (mass) / 1.5 (hydro) on every side,
// strictly overlaps the rect.
template <class W>
bool DepositOverlaps(const W& w, int type, float x0, float z0, float x1, float z1) {
  float g = type == 1 ? 0.5f : 1.5f;
  return w.AnyDepositNear(x0, z0, x1, z1, [&](const auto& d) {
    return d.type == type && x0 < static_cast<float>(d.x1) + g && static_cast<float>(d.x0) - g < x1 &&
           z0 < static_cast<float>(d.z1) + g && static_cast<float>(d.z0) - g < z1;
  });
}

// CSimResources::DepositIsInArea 0x546650: the rect contains a deposit of the type, or one contains the rect.
template <class W>
bool DepositInArea(const W& w, int type, int x0, int z0, int x1, int z1) {
  return w.AnyDepositNear(x0, z0, x1, z1, [&](const auto& d) {
    return d.type == type && ((x0 <= d.x0 && d.x1 <= x1 && z0 <= d.z0 && d.z1 <= z1) ||
                              (d.x0 <= x0 && x1 <= d.x1 && d.z0 <= z0 && z1 <= d.z1));
  });
}

// OCCUPY_Check for a structure blueprint at (x, z): the layers it could be built on (0 = none).
template <class W, class D>
int StructureLayers(const W& w, const NamedFootprint& fp, const D& d, float x, float z) {
  const auto* map = w.map();
  if (!map) return 0;
  Rect sk = SkirtRectOf(fp, d, x, z);
  // OCCUPY_Check 0x5652e0 (engine-ref structure_placement.md): the skirt within the vertex bounds
  if (std::floor(sk.x0) < 0 || std::floor(sk.z0) < 0 || std::ceil(sk.x1) > map->width() ||
      std::ceil(sk.z1) > map->height())
    return 0;
  // the flatness samples: the skirt truncated to vertices
  int x0 = static_cast<int>(sk.x0), z0 = static_cast<int>(sk.z0);
  int x1 = static_cast<int>(sk.x1), z1 = static_cast<int>(sk.z1);
  int caps = d.buildOnLayerCaps;  // Physics.BuildOnLayerCaps (bp+0x2f4)
  float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
  auto take = [&](int vx, int vz) {
    vx = std::clamp(vx, 0, map->width());
    vz = std::clamp(vz, 0, map->height());
    float h = map->HeightAt(vx, vz);
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  };
  // The samples' lowest and highest point taken on the stored heights (HeightAt = height * scale; with the scale > 0
  // the product keeps their order, rounding included, so min / max of the products are the products of min / max):
  // the same lo / hi, one multiplication each instead of one per vertex (speed only, bind 2026-10-28). The vertices
  // a sample loop clamps to are the clamped range (a range past the edge gives its edge vertex, as each take does).
  const bool raw = map->heightScale() > 0.0f;
  uint16_t rlo = 0xffff, rhi = 0;
  bool any = false;
  auto rawRow = [&](int vz, int xa, int xb) {  // take(vx, vz) for vx = xa .. xb
    if (xa > xb) return;
    vz = std::clamp(vz, 0, map->height());
    xa = std::clamp(xa, 0, map->width());
    xb = std::clamp(xb, 0, map->width());
    const uint16_t* r = map->heightRow(vz);
    for (int vx = xa; vx <= xb; ++vx) {
      rlo = std::min(rlo, r[vx]);
      rhi = std::max(rhi, r[vx]);
    }
    any = true;
  };
  auto rawDone = [&] {
    if (!any) return;
    lo = static_cast<float>(rlo) * map->heightScale();
    hi = static_cast<float>(rhi) * map->heightScale();
  };
  bool flat;
  if (!d.flattenSkirt) {  // AreaFlatness 0x5651f0
    if (raw) {
      for (int vz = z0; vz <= z1; ++vz) rawRow(vz, x0, x1);
      rawDone();
    } else {
      for (int vz = z0; vz <= z1; ++vz)
        for (int vx = x0; vx <= x1; ++vx) take(vx, vz);
    }
    flat = hi - lo <= d.maxGroundVariation;
  } else {  // EdgeFlatness 0x564f80: the ring just outside, measured from the ceiling of its lowest point
    if (raw) {
      rawRow(z0 - 1, x0 - 1, x1 + 1);
      rawRow(z1 + 1, x0 - 1, x1 + 1);
      for (int vz = z0; vz <= z1; ++vz) {
        rawRow(vz, x0 - 1, x0 - 1);
        rawRow(vz, x1 + 1, x1 + 1);
      }
      rawDone();
    } else {
      for (int vx = x0 - 1; vx <= x1 + 1; ++vx) {
        take(vx, z0 - 1);
        take(vx, z1 + 1);
      }
      for (int vz = z0; vz <= z1; ++vz) {
        take(x0 - 1, vz);
        take(x1 + 1, vz);
      }
    }
    float c = static_cast<float>(std::ceil(static_cast<double>(lo)));
    flat = std::max(hi - c, c - lo) <= d.maxGroundVariation;
  }
  if (!flat) caps &= ~3;
  float water = map->hasWater ? map->waterElevation : -10000.0f;
  if (water > lo) caps &= ~1;
  if (hi > water - fp.minWaterDepth) caps &= ~0xe;
  if (!caps) return 0;
  // deposits (no terrain-type test on this path): extractors need theirs; nothing else may come near one
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  int dep = DepositType(d.buildRestriction);
  if (dep) {
    if (!DepositInArea(w, dep, ox, oz, ox + fp.sizeX, oz + fp.sizeZ)) return 0;
  } else if (d.buildRestriction.empty() || d.buildRestriction == "RULEUBR_None") {
    if (DepositOverlaps(w, 1, sk.x0, sk.z0, sk.x1, sk.z1) || DepositOverlaps(w, 2, sk.x0, sk.z0, sk.x1, sk.z1))
      return 0;
  }
  return caps;
}

// Where a structure ordered at p stands: its footprint snapped to the cell grid, y on the ground, the water, or the
// sea bed (sim/build.cpp SnapStructurePosition has the original's reading).
template <class W>
Vec3 SnapStructurePosition(const W& w, const NamedFootprint& fp, Vec3 p) {
  int ox = RoundEven(p.x - fp.sizeX * 0.5f), oz = RoundEven(p.z - fp.sizeZ * 0.5f);
  Vec3 r{ox + fp.sizeX * 0.5f, p.y, oz + fp.sizeZ * 0.5f};
  if (const auto* map = w.map()) r.y = (fp.caps & 2) ? map->TerrainHeight(r.x, r.z) : map->SurfaceHeight(r.x, r.z);
  return r;
}

// FootprintFitsStructure 0x720d90 (sim/build.cpp LocationIsFree has the original's reading).
template <class W, class D>
bool LocationIsFree(W& w, const NamedFootprint& fp, const D& d, float x, float z) {
  if (!d.structure) {
    const int oz = static_cast<int16_t>(RoundEven(z - fp.sizeZ * 0.5f));
    const int ox = static_cast<int16_t>(RoundEven(x - fp.sizeX * 0.5f));
    unsigned caps = w.MobileCaps(fp, ox, oz);
    caps = w.FootprintFits(static_cast<uint8_t>(caps), fp, ox, oz);
    return caps != 0;
  }
  if (!StructureLayers(w, fp, d, x, z)) return false;
  int ox = RoundEven(x - fp.sizeX * 0.5f), oz = RoundEven(z - fp.sizeZ * 0.5f);
  if (w.AnyStructureSkirtOverlaps(SkirtRectOf(fp, d, x, z))) return false;
  return !w.AnyStructureIn(ox, oz, ox + fp.sizeX, oz + fp.sizeZ);
}

// CAiBrain::CanBuildStructureAt 0x57cbb0 with relation 3 (sim/build.cpp CanBuildStructureAt).
template <class W, class D>
bool CanBuildStructureAt(W& w, const NamedFootprint& fp, const D& d, float x, float z) {
  if (!LocationIsFree(w, fp, d, x, z)) return false;
  return d.structure || !w.LiveStructureSkirtOverlaps(SkirtRectOf(fp, d, x, z));
}

}  // namespace placement
}  // namespace moho
