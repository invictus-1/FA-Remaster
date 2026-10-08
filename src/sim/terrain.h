// The map (.scmap): heightfield, terrain types, water levels and the map's props. Only what the
// simulation needs is kept; textures, decals and lighting are for the renderer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/entity.h"

namespace moho {

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
  // Terrain elevation at a world position (bilinear between grid points).
  float TerrainHeight(float x, float z) const;
  // Terrain or water surface, whichever is higher.
  float SurfaceHeight(float x, float z) const;
  uint8_t TerrainType(int x, int z) const;  // per game unit cell
  void SetTerrainType(int x, int z, uint8_t t);

  bool hasWater = false;
  float waterElevation = 0, waterElevationDeep = 0, waterElevationAbyss = 0;
  std::vector<MapProp> props;

 private:
  int version_ = 0;
  int width_ = 0, height_ = 0;
  float heightScale_ = 1.0f / 128;
  std::vector<uint16_t> heights_;  // (width+1) * (height+1)
  std::vector<uint8_t> types_;     // width * height
};

}  // namespace moho
