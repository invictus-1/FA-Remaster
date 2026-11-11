// (surround) The land round the map (Chris, 2026-11-09: "sculpt an entire terrain outside the map so the map actually
// looks like it exists vs emptiness or a mirrored exterior outside of the map square"): a map may bring a sculpted
// surround, its own ground past its four edges, and with World::mapSurround that is drawn there in place of the map
// laid out again mirrored (terrain.h, beyondReach). A picture only: nothing of it is in the game.
//
// The files, beside the map's own (x.scmap): x_surround.json and the picture of heights it names.
//   { "version": 1,
//     "near": { "heights": "x_surround_heights.png",   16-bit grey PNG, `samples` across and down, the north row and
//               "samples": 1537,                       the west column first
//               "units_per_sample": 2.0,               world units from one sample to the next
//               "origin": [-1024, -1024],              the first sample's place (world x, z)
//               "height_scale": 0.0078125 },           world units of a value's 1
//     "water": { "level": 17.5 } }
// The map's own square lies somewhere inside; what the picture holds there is not used (the map's heights are).
//
// Where the two meet: the surround's heights are taken toward the map's own edge heights over the first `seam` world
// units outside the edge (the map's edge is the game's and stays), and how far they were apart is said when the map is
// opened. A curtain hangs along the edge from the map's own edge heights, each grid point's, and every part of the
// surround has a skirt, so no gap opens between ground drawn at different finenesses.
//
// Its ground is drawn as far as the picture reaches, finer near the map and coarser far from it (parts of 128 world
// units, each at one fineness by how far from the map it lies; only those in view). The first look ("block-out"): its
// colours are the map's own ground's where that is low by the water, level, high and steep (BeyondLand::Palette), by
// the surround's heights and slopes, going over from the map's own colours at the edge; lit by the sun and the sky as
// the ground is (wet, puddles and snow where the map's own lie), shadowed by its own hills and the map's (each point
// looks toward the sun over the heights), then dimmed and lost with the distance as all that lies beyond the edge
// (lighting.h, BeyondMap).
//
// The detail look (2026-11-10; the visuals session's detail pass, visuals/renderer_reply_lod.md section 120): the json
// may name three more files beside the heights, and with them the surround is the map's own ground and scenery:
//     "masks": { "file": "x_surround_masks.png",    8-bit RGBA PNG over the heights' square, the north row first:
//                "samples": 1537,                   which of the map's ground layers lies where. Red, green, blue
//                "r": 1, "g": 2, "b": 3, "a": 7,    and alpha are the shares of the layers of these numbers (the
//                "rest": 0 },                       map's own: 0 its lower layer, 1 to 8 its strata), what the four
//                                                   leave of 255 the share of layer "rest"
//     "forest": "x_surround_forest.png",            8-bit grey PNG over the same square: 255 closed forest
//     "forest_floor": 5,                            (may be left out; here or among the masks' numbers) the layer laid
//                                                   under the forest by that picture
//     "forest_colour": [0.165, 0.155, 0.095],         (may be left out) the far forest's crowns, as stored colours
//     "decor": "x_surround_decor.bin"               the scenery standing on it (scmap.h, LoadDecor: the map's own
//                                                   decor format); drawn by the world (World::surroundScenery)
//   Its GROUND is then drawn in the map's own layers (the ground's own albedos, normals and third maps at the map's
//   repeats: Terrain::BindLayers), laid by the masks in the layers' order and as the ground's shader lays them (a
//   layer with a third map comes through by its height first, and brings its roughness and crease shading); the upper
//   layer, which lies over the map only, as its mean where each layer shows on the map (Terrain::UpperByLayer). Within
//   `ownColours` world units of the edge the map's own masks and upper layer (read on past the edge, mirrored) go over
//   into the surround's, share by share, so that at the edge the two grounds are the same ground. Cheap far off: a
//   layer that has no share at a pixel is not read, and the layers' normals only within `fineReach` of the eye. No
//   relief, no deep tracks, no mud mask, no wet twins and no snow materials: lit, wet, snowed and dimmed with the
//   distance as the block-out is (ModernLight, BeyondMap), with the wet band along the water as the map's shores have.
//   Its FOREST: the layer "forest_floor" under it; where single trees stand (the scenery) their shade on the ground;
//   and where none stand (farther out than the scenery goes, or with the scenery off) the forest as one canopy: dark
//   green crowns some five world units across with the floor in their shade between them (as the map's own woods look
//   from above), each crown's sunny side lighter, the grain let go of as a crown gets smaller than a pixel.
//   Without "masks" in the json, or with `blockOut` set: the block-out, as before.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "scmap.h"
#include "view.h"

namespace render {

class BeyondLand;
class Terrain;

class Surround {
 public:
  float seam = 16;         // over how many world units outside the edge its heights are taken toward the map's edge
  float ownColours = 40;   // over how many world units outside the edge the map's own colours go over into its own
  float softness = 0.06f;  // how soft its shadows are (the slope over which a shadow's edge fades)
  // (the detail look) The first look kept: its ground in the map's colours by height and slope although the map brings
  // its ground's masks (viewer --surround-blockout).
  bool blockOut = false;
  float fineReach = 600;   // how far from the eye the layers' own normals are read (they fade out over its last 40 percent)
  // The layer laid under the forest where the json does not say ("forest_floor"); -1: none (viewer
  // --surround-forest-floor).
  int forestFloor = -1;
  // The far forest's canopy: how much of it is drawn (0: none, the ground's layers alone), and from how far past the
  // map's edge to how far the single trees of the scenery thin out to none (the world sets them: where they stand the
  // ground has their shade, not the canopy; 0, 0: no scenery is drawn, the canopy everywhere the forest is).
  float canopy = 1;
  float treesFrom = 0, treesTo = 0;
  // From how far from the eye to how far the scenery's trees are left out (the world sets them as it lists the scenery:
  // World::surroundSceneryFrom): there too the forest is the canopy.
  float sightFrom = 0, sightTo = 0;

  // A file beside the map's, by its name; none when there is none.
  using ReadFn = std::function<std::optional<std::string>(const std::string& name)>;
  // A map has been opened (World::Open; `json`: its x_surround.json): the surround read and joined to the map's edge.
  // False and why when it cannot be used (then the map has none).
  bool Open(const MapData& map, const std::string& json, const ReadFn& read, std::string* why);
  // The map before is gone (with what was made for its surround).
  void Forget();
  bool Have() const { return !heights_.empty(); }
  // It can be drawn (false once its pipeline could not be made: the map laid out again takes its place).
  bool Usable() const { return !heights_.empty() && !failed_; }
  // (the detail look) Whether the map brings its ground's masks (and they could be read), and its scenery's file beside
  // the map's (empty: none).
  bool HasMasks() const { return !masks_.empty(); }
  const std::string& Decor() const { return decor_; }
  // How far the ground at a place was moved by joining it to the map's edge (world units, + up; 0 farther out than
  // `seam`): the scenery's file has its heights from the surround as it came, and stands on the ground as drawn by
  // this much more. Kept only where the json names scenery, until LetGoAsCame (the world, once it has placed it).
  float JoinMoved(float x, float z) const;
  void LetGoAsCame() {
    asCame_.clear();
    asCame_.shrink_to_fit();
  }
  bool Detailed() const { return detailNow_; }  // (the last Prepare: drawn in the map's own layers)
  // How far past the map's edges it reaches at least (world units).
  float Reach() const { return reach_; }
  // Its ground's height at a place (world units), as it is drawn.
  float HeightAt(float x, float z) const;
  // Its heights for the shaders that read the ground beyond the edge (one value a sample, times HeightUnit; none
  // before it was first drawn), the first sample's place and the samples' spacing.
  gfx::Texture Heights() const { return texture_; }
  float HeightUnit() const { return unit_ * 65535.0f; }
  const float* Origin() const { return origin_; }
  float Step() const { return step_; }
  int Samples() const { return samples_; }

  // Before the scene's pass, every frame (`on`: the option, with the world beyond the edge, in the modern look, not
  // for a side picture): what is drawn made the first time it is wanted; the map's colours (that dispatches).
  void Prepare(gfx::Device* device, bool on, Terrain& terrain, BeyondLand& colours);
  bool On() const { return now_; }
  // Into the scene's pass, right after the map's ground (the small lights bound).
  void Draw(const View& view);
  int Triangles() const { return drawn_; }

 private:
  struct Part {
    Vec3 lo, hi;
    uint32_t first = 0, count = 0;
  };
  void Make();
  gfx::Device* device_ = nullptr;
  gfx::Pipeline pipeline_;
  gfx::Buffer vertices_, indices_;
  gfx::Texture texture_, palette_, ground_;
  // (the detail look) its pipeline (made the first time it is wanted); the masks and the forest as read (Open) and as
  // textures with smaller levels (Make); which layers the masks' four shares and what they leave are of; the layer
  // under the forest and the canopy's colour as the json gives them; the map's upper layer where each layer shows
  // (Terrain::UpperByLayer, asked once); the ground whose layers are bound (Prepare's)
  gfx::Pipeline detail_;
  bool detailTried_ = false, detailNow_ = false, upperAsked_ = false;
  std::vector<uint8_t> masks_, forest_;
  int masksSamples_ = 0, forestSamples_ = 0;
  gfx::Texture masksTexture_, forestTexture_;
  int kinds_[5] = {1, 2, 3, 7, 0};
  int floorGiven_ = -1;
  float canopyColour_[3] = {0.165f, 0.155f, 0.095f};  // (measured against SETONS_R's own woods from the farthest zoom)
  float upperOf_[9][4] = {};
  std::string decor_;
  std::vector<uint16_t> asCame_;  // (its heights before the join, for JoinMoved; empty: let go of, or no scenery)
  Terrain* terrain_ = nullptr;
  std::vector<uint16_t> heights_;  // samples_ * samples_, the north row first; joined to the map's edge
  std::vector<float> edges_[4];    // the map's own heights along its edges, a grid point each: north, south, west, east
  std::vector<Part> parts_;
  int samples_ = 0, mips_ = 1, drawn_ = 0;
  float origin_[2] = {0, 0}, step_ = 1, unit_ = 1.0f / 128, reach_ = 0;
  float mapSize_[2] = {1, 1}, sea_ = -1e9f, mapSea_ = -1e9f, mapHighest_ = 0, highest_ = 0;
  bool made_ = false, tried_ = false, failed_ = false, now_ = false;
};

}  // namespace render
