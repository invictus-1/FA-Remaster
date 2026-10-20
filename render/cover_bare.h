// [cover] Where the ground cover does not grow (grass.h, GroundCover::bare; Chris, 2026-10-19: "some parts of map don't
// need it like past trails", and plants standing on the stream). A picture over the whole map, two texels a world unit,
// 1 where it is bare:
//  - a stream (stream_water.h: the map's props whose finest level is a stream's water or its white water) and its wet
//    bank: its ribbon's triangles laid into a picture of its own on the processor when the map opens, and a margin round
//    them that thins out;
//  - the trails and bare patches through the meadow: where the map has a cover mask (remap.json's "cover_mask",
//    Terrain::CoverMask: the visuals session paints the streams, trails and roads), the mask, which CoverDensity lays into
//    the table itself; where it has none, where the cover's table (Terrain::CoverDensity's second picture, alpha: the
//    blades its layers give) says markedly fewer blades than it does a few units round about (a path a few units wide
//    through the grass, a patch of soil), the more the fewer. A wide stretch of thinner cover (a dry meadow, sand) keeps
//    what its table says: only what is thinner than round about is a trail.
// The cover's shaders leave a tuft or a plant out where the picture is above a number of its own, and lower it where
// the picture thins out. Made the first time it is wanted after a map opens (it dispatches: before the scene's pass),
// and again when the table's picture is made again.
#pragma once
#include <cstdint>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "models.h"
#include "textures.h"

namespace render {

class CoverBare {
 public:
  explicit CoverBare(gfx::Device* device) : device_(device) {}
  ~CoverBare();
  CoverBare(const CoverBare&) = delete;
  CoverBare& operator=(const CoverBare&) = delete;

  // A map opened: its size in world units. Forgets the last map's streams and picture.
  void SetMap(float mapWidth, float mapHeight);
  // A prop of the map (World::Open, after SetMap): when its finest level is a stream's (ModelLod::stream), its ribbon,
  // placed by `world`, is laid into the streams' picture (its mesh file read through `textures`). Others are left alone.
  void AddStream(const Model* model, const Mat4& world, TextureCache* textures);
  // This frame's picture (none: nothing is bare, no map): made the first time, and again when `remade` says the table's
  // picture `density` (Terrain::CoverDensity's second) was made again this frame (none: the streams alone). The map's
  // cover mask (`coverMask`, Terrain::CoverMask; none: it has none) is the trails' painter's word: with it the trails are
  // not looked for in the table (the mask is in the table already); without a table it is laid in here. The streams'
  // ribbons are kept bare in any case. Call between passes.
  gfx::Texture Picture(gfx::Texture density, gfx::Texture coverMask, bool remade);

  float margin = 0.6f;  // world units past a stream's ribbon still bare (its wet bank), thinning out to nothing
  float trails = 3.0f;  // how fast a trail is bare by how much fewer blades it has than round about (0: no trails)
  float around = 4.0f;  // world units: how far round about a place the table is read for what is usual there
  int streamsLaid = 0;  // (the ribbons laid since SetMap; their triangles)
  int trianglesLaid = 0;

 private:
  gfx::Device* device_;
  float mapWidth_ = 0, mapHeight_ = 0;
  int width_ = 0, height_ = 0;  // (the pictures' size: two texels a world unit)
  std::vector<uint8_t> streams_;  // (the streams' picture as laid: 255 on a ribbon)
  bool streamsChanged_ = false;
  gfx::Texture streamTexture_, picture_;
  bool made_ = false, withDensity_ = false, withMask_ = false;  // (the picture made since SetMap; with the table's picture, the mask)
  gfx::Pipeline pass_;
  int tried_ = 0;  // (its pass: 0 not yet, 1 made, -1 it cannot be)
};

}  // namespace render
