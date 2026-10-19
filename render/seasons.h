// Seasons (Chris, 2026-10-18, through the visuals session: seasons as player options; the form agreed in
// docs/render-answers.md, "To visuals, section 57"). A season is the picture's alone: the simulation keeps the map's
// summer blueprints, and every player of a game may pick his own. It is taken in when a map is opened (its ground's
// materials, its ground cover and its props are loaded then: World::season, WorldPicture::SetSeason); a change shows
// from the next map.
//
// The art is the visuals track's, named per map in its remap.json beside "cover" and "mud":
//   "seasons": {
//     "autumn": {
//       "materials": {"<layer material>": "<autumn material>", ...},   (same layout as the summer ones; a material
//                                                                       without an 8k tier is read at its 4k one)
//       "cover": "<autumn cover json>",                                (the ground cover's copy, density and all)
//       "props": {"<prop blueprint path>": "<autumn blueprint path>"}, (draw-only: the picture takes the autumn
//                                                                       blueprint's display: mesh, levels, textures)
//       "mud": "<autumn mud material>"                                 (optional, as the map's "mud")
//     }
//   }
// What a season does not list stays summer's; a map without "seasons" (the stock Seton's) is drawn as it always was.
// What needs no art is the renderer's: leaves in the wind (FallingLeaves, below: off the broadleaf trees, and over the
// ground where the cover grows), and autumn's weather (weather.h, Climate::Autumn: more overcast, showers, morning fog).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "json.h"
#include "linalg.h"
#include "season.h"
#include "view.h"

namespace render {

// What a map's remap.json says a season changes: its entry's "seasons" block for that season.
struct SeasonLook {
  Season season = Season::Summer;
  bool listed = false;  // the map's entry has a block for this season
  std::unordered_map<std::string, std::string> materials;  // a layer's material -> the season's
  std::string cover;  // the ground cover's JSON for the season, below the remastered ground's folder ("": summer's)
  std::string mud;    // the deep tracks' mud material for the season ("": summer's)
  // a prop's blueprint path (as moho::Vfs::Normalize has it: lower case) -> the season's blueprint path
  std::unordered_map<std::string, std::string> props;
  // Reads `mapEntry["seasons"][<the season's name>]` (nothing for summer).
  void Read(const JsonValue& mapEntry, Season of);
  // The season's material for a layer's material, or "" where it has none.
  std::string Material(const std::string& name) const;
  // The blueprint a prop is drawn by in the season: the season's where it lists one, else `blueprint` itself.
  std::string Prop(const std::string& blueprint) const;
};

// A broadleaf tree by its blueprint's file name (birch, oak, maple, beech, aspen, ...; never a pine, spruce, fir or the
// like): what sheds its leaves in autumn besides the trees a season lists in its own look (World::Open).
bool Broadleaf(const std::string& blueprint);

// Leaves in the autumn wind (World::fallingLeaves; autumn, the modern look), of two kinds, both small cards in the shape
// of a leaf, lit as everything solid is (the sun through the shadow maps and the clouds, the sky, the small lights, the
// haze and the mist), with some of the sun coming through from behind, at least a pixel and a half (fewer where they
// would have to be drawn larger), fewer as the camera rises and none from the strategic zoom:
// - off the broadleaf trees (SetTrees): from each tree near what the camera sees a few at a time come off its crown,
//   flutter and spin down, carried along by the wind the trees sway in (its gusts, harder in rain), lie several seconds
//   where they land and are gone;
// - on the ground, where the ground cover grows (its density picture, Terrain::CoverDensity): a few fallen leaves lie
//   there, and a gust takes one now and then a few units along the wind, hopping and tumbling over the ground, before
//   it lies again and is gone. A map of pines alone (SETONS_R) has these only.
// Nothing is kept: each leaf follows from where it is, its number and the time, so nothing moves on the processor; the
// processor only chooses, each frame, which trees shed (the nearest standing ones in the picture) and the squares of
// ground round what the camera looks at.
class FallingLeaves {
 public:
  explicit FallingLeaves(gfx::Device* device) : device_(device) {}
  ~FallingLeaves();

  // A tree that sheds: where its foot is, how tall it is, its crown's radius and where its crown begins (a share of
  // its height), all world units; `key`: the host's number for it (World: the prop's place in the map's list).
  struct Tree {
    Vec3 foot;
    float height = 1, crown = 0.5f, low = 0.4f;
    uint32_t key = 0;
  };
  // The map's trees that shed this season (World::Open; none: only the leaves on the ground), and its size and water's
  // level (a leaf that lands on the water floats there; none lie on it at first).
  void SetTrees(std::vector<Tree> trees, float mapWidth, float mapHeight, float waterLevel);
  size_t Trees() const { return trees_.size(); }

  // Each frame it is on, before the scene's pass: the trees that shed now chosen (`standing(key)`: the tree still
  // stands as it was, not felled, burnt or gone), and the squares of ground round what the camera looks at where
  // `plants` (Terrain::CoverDensity's first picture; none: no leaves on the ground) says the cover grows. `rain`, `snow`
  // as World has them (the wind's strength: the trees' gusts, by the view's time, as they sway). False when nothing is
  // to be drawn.
  bool Prepare(const View& view, const Camera& camera, float rain, float snow, const std::function<bool(uint32_t)>& standing,
               gfx::Texture plants);
  // Into the scene's pass, with what is solid. `heights`, `heightUnit`: the ground's (Terrain::HeightTexture, HeightUnit).
  void Draw(const View& view, gfx::Texture heights, float heightUnit);

  // Tuning (viewer --leaves-*): how many (1: a few a tree), how big (1: 0.07 world units off the trees, 0.16 on the
  // ground, about as large as the leaves the crowns' and the autumn ground's art hold), how fast they sink and drift;
  // and how many lie on the ground (a share of the places where the cover grows; 0: none there).
  float density = 1, size = 1, sink = 1, drift = 1, groundDensity = 1;
  int treesDrawn = 0, leavesDrawn = 0, groundPlaces = 0;  // the last Prepare: trees shedding, leaf places gone through

  static constexpr int kTreesMost = 256;     // trees shedding in one frame at most
  static constexpr int kLeavesPerTree = 12;  // places for a leaf on each, each in its own cycle
  static constexpr float kSquare = 3;        // the ground's squares' size (world units)
  static constexpr int kPerSquare = 3;       // places for a leaf in each

 private:
  bool Ready();
  void Common(const View& view, const Camera& camera, float rain, float snow, float farthest, float share, float length, std::vector<float>* out);
  gfx::Device* device_;
  std::vector<Tree> trees_;
  float mapWidth_ = 0, mapHeight_ = 0, water_ = -1e9f;
  bool tried_ = false;
  gfx::Pipeline draw_, ground_;
  std::vector<float> constants_, groundConstants_;  // this frame's (LeafConstants)
  std::vector<std::pair<float, uint32_t>> near_;  // (Prepare's: the trees near enough, by their distance)
  gfx::Texture plants_;
  bool drawNow_ = false, groundNow_ = false;
};

}  // namespace render
