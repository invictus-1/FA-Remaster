// The map (.scmap): heightfield, terrain types, water levels and the map's props. Only what the
// simulation needs is kept; textures, decals and lighting are for the renderer.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/entity.h"

namespace moho {

// Terrain elevation at a world position from a raw heightfield ((w + 1) * (h + 1) samples, w * h cells, each
// sample * scale = height): the one bilinear rule, used by TerrainMap::TerrainHeight and by copies of the
// heightfield the interface keeps between beats (ui request 25).
float TerrainHeightOf(const uint16_t* raw, int w, int h, float scale, float x, float z);
// The same rule over any sample source: at(ix, iz) = the clamped sample (ix, iz) times the scale (TerrainMap::HeightAt).
// (The placement snapshots keep the heightfield in bands of rows: sim/sim_snapshot.cpp.)
template <class At>
float TerrainHeightBilinear(const At& at, float x, float z) {
  float fx = std::floor(x), fz = std::floor(z);
  int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  float tx = x - fx, tz = z - fz;
  // STIMap::GetElevation 0x44fb90: lerp along z first, then x; the x87 part runs with precision control
  // 24 bits on the sim thread, so every step rounds like a float op
  float A = at(ix, iz), B = at(ix, iz + 1);
  float C = at(ix + 1, iz), D = at(ix + 1, iz + 1);
  float a = A + (B - A) * tz;
  float b = C + (D - C) * tz;
  return a + (b - a) * tx;
}

class TerrainMap {
 public:
  struct MapProp {
    std::string blueprint;  // "/env/evergreen/props/trees/groups/pine06_groupa_prop.bp"
    Vec3 position;
    float rotation[9];      // three axis vectors (x, y, z)
    Vec3 scale;
  };

  // Parse an .scmap file (version 56 or 60). Returns false and sets `error` when malformed.
  bool Load(const std::string& data, std::string* error);

  int width() const { return width_; }    // in game units (= heightfield cells)
  int height() const { return height_; }
  int version() const { return version_; }
  // Raw height sample at grid point (x, z), clamped to the map.
  float HeightAt(int x, int z) const;
  // CHeightField::SetElevationRect: one grid vertex, stored truncated to the height scale.
  void SetHeightAt(int x, int z, float h);
  // Terrain elevation at a world position (bilinear between grid points).
  float TerrainHeight(float x, float z) const;
  // Terrain or water surface, whichever is higher.
  float SurfaceHeight(float x, float z) const;
  uint8_t TerrainType(int x, int z) const;  // per game unit cell
  void SetTerrainType(int x, int z, uint8_t t);
  // For a copy kept outside the beat (ui request 25): the raw heightfield ((width + 1) * (height + 1) samples),
  // its scale, and a count of height writes so far (it changes when SetHeightAt runs, e.g. a building flattens
  // the ground); read them only between beats.
  const uint16_t* heightData() const { return heights_.data(); }
  const uint16_t* heightRow(int vz) const { return heights_.data() + static_cast<size_t>(vz) * (width_ + 1); }  // (vz in 0..height)
  float heightScale() const { return heightScale_; }
  // Whether SetHeightAt(i, j, h) over [x0, x1] x [z0, z1] (clipped as SetHeightAt clips) would change a vertex.
  bool WouldChangeHeights(int x0, int z0, int x1, int z1, float h) const;
  uint64_t changes() const { return changes_; }
  // Every rectangle of grid vertices whose heights were set since the map was loaded, in order (render request 25:
  // the drawn ground follows FlattenMapRect). Inclusive, clamped to the map; read between beats.
  struct HeightChange {
    int x0, z0, x1, z1;
  };
  const std::vector<HeightChange>& heightChanges() const { return heightChanges_; }
  void NoteHeightChange(int x0, int z0, int x1, int z1);  // (FlattenMapRect, after its SetHeightAt loop)

  bool hasWater = false;
  float waterElevation = 0, waterElevationDeep = 0, waterElevationAbyss = 0;
  std::vector<MapProp> props;

 private:
  int version_ = 0;
  int width_ = 0, height_ = 0;
  float heightScale_ = 1.0f / 128;
  std::vector<uint16_t> heights_;  // (width+1) * (height+1)
  std::vector<uint8_t> types_;     // width * height
  uint64_t changes_ = 0;           // SetHeightAt calls
  std::vector<HeightChange> heightChanges_;
};

}  // namespace moho
