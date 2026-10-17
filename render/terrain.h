// The ground: the map's heightfield drawn with level of detail, textured and lit the way the original
// game does it (its "TTerrain" and "TTerrainXP" looks).
//
// Geometry: the heights live in a texture; the mesh is one small grid of quads drawn many times, each
// time for a square patch of the map at some level of detail (level k: quads of 2^k cells). Which
// patches are drawn at which level is decided per frame from the camera (the scheme known as CDLOD):
// near ground gets small quads, far ground large ones, and within each level the vertices slide
// toward the next coarser grid as distance grows, so levels meet without cracks or popping.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "json.h"
#include "scmap.h"
#include "textures.h"
#include "view.h"

namespace render {

class Terrain {
 public:
  Terrain(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  // The richer ground of a remastered map (modern look): each layer's own roughness and crease
  // shading, and layers that meet by their height (pebbles stand out of sand, grass creeps into
  // the joints of rock) instead of fading evenly into each other. false: as a stock map is drawn.
  // Without the layers' third maps it changes nothing.
  bool richGround = true;
  // The picture of where lying snow is trodden away (tracks.h), set by the host each frame; none:
  // the snow lies untrodden.
  gfx::Texture tracksMap;
  // The world beyond the map's edge: how far out (world units) the ground goes on, as the map
  // laid out again mirrored on every side, which joins without a seam. 0: the map ends at its
  // edge. The host sets it with the view's `beyond` (World does).
  float beyondReach = 0;
  int MaterialLayers() const;  // how many of the map's layers have a third map

  // Builds everything for a map. `map` must stay alive while the terrain is used.
  bool Load(const MapData* map, std::string* error);
  // Into the scene, or (`shadowPass`) into a shadow map, where only depth is written.
  void Draw(const View& view, bool shadowPass = false);
  // The map's heights of the grid points [x0, x1] x [z0, z1] have changed (the simulation laid the
  // ground under a structure flat): drawn so from now on. Within a frame.
  void HeightsChanged(int x0, int z0, int x1, int z1);

  // On-screen size, in pixels, that one quad of the mesh should have: smaller means more triangles.
  float quadPixels = 6;
  // The remastered ground (remasters.h, kRemasteredTerrain): the open map's path, set before Load, by which its
  // remap.json is looked up; `ultraGround`: its 8k materials in place of the 4k.
  std::string mapPath;
  bool ultraGround = false;
  int RemasteredLayers() const { return remasteredLayers_; }
  // The map's water depth picture and the colour ramp the ground under water is tinted with
  // (decals lying under water take the same tint).
  gfx::Texture WaterMap() const { return waterMap_; }
  gfx::Texture WaterRamp() const { return waterRamp_; }
  // Numbers from the last Draw.
  int drawnPatches = 0;
  int drawnTriangles = 0;

 private:
  struct Level {
    int nodeCells = 0;         // side of a node of this level, in cells
    int nx = 0, nz = 0;        // nodes along x and z
    std::vector<float> lo, hi; // lowest and highest ground in each node
  };
  struct Patch {
    float x, z, quad, level;
  };

  bool Select(const View& view, int level, int ix, int iz);
  void AddNode(const View& view, int level, int ix, int iz);
  void AddPatch(const View& view, int level, float x, float z, float size, float lo, float hi);
  void InWorld(float x0, float z0, float x1, float z1, float lo, float hi, Vec3* a, Vec3* b) const;
  int tileX_ = 0, tileZ_ = 0;  // which copy of the map is being gone through (Draw)

  gfx::Device* device_;
  TextureCache* textures_;
  const MapData* map_ = nullptr;
  gfx::Pipeline pipeline_, shadowPipeline_;
  gfx::Buffer gridVertices_, gridIndices_;
  uint32_t gridIndexCount_ = 0;
  gfx::Texture heightTexture_, normalMap_, maskLow_, maskHigh_, waterMap_, waterRamp_;

 public:
  // The ground's normals (x in alpha, z in green, as the terrain's shader reads them), for others (ray_scene.h).
  gfx::Texture NormalMap() const { return normalMap_; }
  // The ground's colour for what does not draw it (the ray tracing scene, ray_scene.h): over the whole map at half
  // its size (RGBA8, as stored: the shaders make it linear), each layer by its texture's average laid over the
  // others as the shader lays them, the upper layer at a level that suits the picture. Made the first time it is
  // asked for once a map is loaded (it dispatches: between passes). Its alpha: how grassy the ground is there (by how
  // green each layer's average is; the ground cover reads it).
  gfx::Texture GroundColours();
  // The ground's heights as the shader reads them (one per grid point), and the world units of their largest value.
  gfx::Texture HeightTexture() const { return heightTexture_; }
  // The visuals track's plants for this map's ground (remap.json: the map's entry names its "cover", the card atlas's
  // JSON with its density table by the map's layers): the JSON (null where the map has none), its files' paths
  // below kRemasteredTerrain.
  const JsonValue* Cover() const { return hasCover_ ? &cover_ : nullptr; }
  // Where the plants grow, over the whole map at half its size, by the cover's table blended as the layers are:
  // `a` RGBA8 (blades, clumps, flowers, dry stalks), `b` RGBA8 (ferns, the share of a clump taken from the dry cells,
  // the share of clumps that are weeds, 0); near trees (`trees`: R8 over the whole map, 1 within the table's radius
  // of a tree) as its near_trees says, not on the layers it names. Made the first time it is asked for once a map is
  // loaded (it dispatches: between passes). False where the map has no cover.
  bool CoverDensity(gfx::Texture trees, gfx::Texture* a, gfx::Texture* b);
  float CoverTreeRadius() const { return coverTreeRadius_; }  // (the table's near_trees radius, world units)
  float HeightUnit() const { return map_ ? 65535.0f * map_->heightScale : 1.0f; }

 private:
  gfx::Texture albedo_[10], normal_[9];
  float albedoScale_[10] = {}, normalScale_[9] = {};  // (the layers' scales, the remastered ground's applied)
  bool normalUp_[9] = {};    // (a remastered material's normal: +y up in its texture, the stock layers' turned)
  int remasteredLayers_ = 0;
  void Remaster(const MapData& map, std::vector<std::string>* arh);
  void ReadCover(const MapData& map, const std::string& path);
  JsonValue cover_;
  bool hasCover_ = false;
  float coverA_[10][4] = {}, coverB_[10][4] = {};  // (each layer's row of the table: as the two pictures hold it)
  float coverNear_[4] = {1, 1, 1, 1}, coverFern_ = 0, coverTreeRadius_ = 6;
  gfx::Pipeline coverPass_;
  gfx::Texture coverA_tex_, coverB_tex_;
  gfx::Pipeline groundColoursPass_;
  gfx::Texture groundColours_;  // (GroundColours, made again for each map)
  // A remastered map's third map per layer (beside `<name>_albedo.dds` a `<name>_arh.dds`: red what
  // its creases let in of the sky's light, green how rough it is, blue how high it stands), all
  // nine as the layers of one texture; `material_[i]`: layer i has one.
  gfx::Texture materials_;
  bool material_[9] = {};
  int shaderKind_ = 0;  // 0: TTerrain (4 strata), 1: TTerrainXP (8 strata), 2: TTerrainGlow (TTerrain, the second stratum glowing)
  std::vector<Level> levels_;
  std::vector<float> ranges_;  // per level: the distance up to which it is used
  std::vector<Patch> patches_;
};

}  // namespace render
