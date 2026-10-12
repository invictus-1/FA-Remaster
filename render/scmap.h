// The map file (.scmap, versions 56 and 60) as the renderer needs it: the heightfield, the ground
// texture layers and their masks, lighting, water, decals and props. (The simulation has its own,
// smaller reader in src/sim/terrain.*; this one keeps what that one skips.)
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "linalg.h"

namespace render {

struct MapLayer {
  std::string texture;  // virtual path, empty when the layer is unused
  float scale = 4;      // world units one repeat of the texture covers
};

struct MapLighting {
  float multiplier = 1;
  Vec3 sunDirection{0, 1, 0};  // toward the sun
  Vec3 sunAmbience;
  Vec3 sunColor{1, 1, 1};
  Vec3 shadowFill;
  Vec4 specular;
  float bloom = 0;
  Vec3 fogColor;
  float fogStart = 0, fogEnd = 0;
};

struct MapWater {
  bool present = false;
  float elevation = 0, elevationDeep = 0, elevationAbyss = 0;
  Vec3 surfaceColor;
  Vec2 colorLerp;
  float refractionScale = 0;
  float fresnelBias = 0, fresnelPower = 0;
  float unitReflection = 0, skyReflection = 0;
  float sunShininess = 0, sunStrength = 0;
  Vec3 sunDirection;
  Vec3 sunColor;
  float sunReflection = 0, sunGlow = 0;
  std::string cubemap, ramp;
  float normalRepeat[4] = {};
  struct Wave {
    Vec2 movement;
    std::string texture;
  } waves[4];
};

struct MapDecal {
  int id = 0;
  int type = 0;  // 1 albedo, 2 normals, ... (the map editor's decal types)
  std::string textures[2];
  Vec3 scale, position, rotation;
  float cutoffLod = 0, nearCutoffLod = 0;
  int army = -1;
};

struct MapProp {
  std::string blueprint;
  Vec3 position;
  Vec3 axisX, axisY, axisZ;  // orientation
  Vec3 scale;
};

struct MapData {
  int version = 0;
  int width = 0, height = 0;  // in cells; the heightfield has (width + 1) x (height + 1) points
  float heightScale = 1.0f / 128;
  std::vector<uint16_t> heights;
  std::string shader;  // "TTerrain", "TTerrainXP", ...
  std::string background, skyCubemap;
  std::vector<std::pair<std::string, std::string>> environmentCubemaps;  // name ("<default>", "<aeon>", ...), file
  MapLighting lighting;
  MapWater water;
  MapLayer albedo[10];  // 0 lower, 1..8 the blended strata, 9 upper (macro)
  MapLayer normal[9];   // 0 lower, 1..8 strata
  std::vector<MapDecal> decals;
  // textures stored inside the map file (.dds bytes)
  std::string normalMap;           // the terrain's own normals
  std::string maskLow, maskHigh;   // blend weights of strata 1..4 and 5..8
  std::string waterMap;
  std::vector<uint8_t> terrainTypes;  // width * height
  std::vector<MapProp> props;

  float HeightAt(int x, int z) const;         // a heightfield point, clamped to the map
  float Height(float x, float z) const;       // bilinear between points
};

// False with a reason in `error` when the file is not a map this reader understands.
bool LoadMap(std::string_view file, MapData* out, std::string* error);

}  // namespace render
