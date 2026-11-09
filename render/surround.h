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
  std::vector<uint16_t> heights_;  // samples_ * samples_, the north row first; joined to the map's edge
  std::vector<float> edges_[4];    // the map's own heights along its edges, a grid point each: north, south, west, east
  std::vector<Part> parts_;
  int samples_ = 0, mips_ = 1, drawn_ = 0;
  float origin_[2] = {0, 0}, step_ = 1, unit_ = 1.0f / 128, reach_ = 0;
  float mapSize_[2] = {1, 1}, sea_ = -1e9f, mapSea_ = -1e9f, mapHighest_ = 0, highest_ = 0;
  bool made_ = false, tried_ = false, failed_ = false, now_ = false;
};

}  // namespace render
