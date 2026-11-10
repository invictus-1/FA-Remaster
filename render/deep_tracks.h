// Deep tracks (World::deepTracks; the modern look): where units drive and walk the ground is pressed, as it
// would be in snow, mud, dirt, sand or grass. Nothing of this is in the original (its tread marks and
// footprints are flat pictures laid for a second or a few, decals.h).
//
// What is pressed is kept in a field of two numbers a texel: how much the ground there has been pressed down
// (the trough) and how much has been pushed up beside it (the berm), each added up over every pass, so that a
// way driven often is pressed deeper and worn. Three levels of it, side by side in one target (the atlas):
//   0: 2048 x 2048 texels of 1/64 world unit round what the camera looks at (32 units: a tank's cleats show);
//   1: 2048 x 2048 texels of 1/16 unit round it too (128 units: the game's usual view);
//   2: 1024 x 1024 over the whole map (the way trodden, kept while the camera is elsewhere).
// Levels 0 and 1 follow the camera in whole texels and wrap round (a texel stands for every place a whole
// number of the level's widths away), so moving costs only the clearing of the strip that comes into view;
// each has a copy of its opposite edge on every side, so that the card's own filtering reads across the wrap.
//
// What presses it: the prints the game's scripts lay under units (CreateSplatOnBone: a tank's tread marks, a
// commander's footprints, chosen by the unit's blueprint and the map's terrain types), each by its own
// picture's shape (its alpha: the band of a tread and its cleats, the sole of a foot); a tread's pieces are
// laid end to end along the way, so the track has no gaps. While this is on the prints are not laid as
// pictures. Units that lay none press their own as they move: two bands under a vehicle; a walker its feet where
// they are set down (or, where its pose does not show them, prints left and right by turns as it goes); hover
// units and aircraft press nothing.
// [hdprints] A remastered unit may bring its own prints (Model::prints, its folder's <ID>_prints.bp): pictures of how deep the
// ground is pressed (white deepest) and their sizes in world units. Where a script lays a print, it still says where and
// when, and the unit's own picture takes the game's place (a walker's Foot, else its Tread or Tyre, at its own size); a
// vehicle that lays none presses its Tread or Tyre in place of the two bands, a piece at a time end to end; a walker its
// Foot where its feet are set down, turned with the foot.
// Tracks fade: falling snow fills them (in place of the old picture of trodden snow, tracks.h, while this is
// on), rain softens them, the wind blows sand smooth, grass rises again; mud and dirt keep them longest. How
// fast goes by the kind of ground there (Terrain::GroundKinds) and the weather.
// The ground's shader (terrain.cpp, under SURFACE_DEEP_TRACKS) reads the field by the kind of ground it draws;
// the ground cover's pressing pass (grass.cpp) lays the grass flat along it.
// [dents] Shell holes: where a shot strikes the ground (World::DentGround: the simulation's impacts on the terrain, else
// the blasts on the ground), a bowl blown out of it with a raised lip and the earth thrown round it (PressCrater), by
// its size. Kept in the field's third number, apart from the tracks: the height the holes make there in world units
// (below the ground negative), added up over every hole, as soft ground (snow, mud) holds it; the ground's shader takes
// less of it on firmer ground and none on rock (terrain.cpp). The picture only: the ground the units drive on and that
// is picked stays as the game has it. Holes fade as the tracks do, slower (a hole is deeper than a rut): falling snow
// fills them, rain softens them, mud keeps them longest. The holes of the last while are listed, so that one comes back
// into the levels round the camera when it comes back to them (at what is left of it).
// [churn] Holes one over another (churnedHoles; Chris's test of 2026-11-09: where many shells struck one spot "some parts
// will look like a hole while others look like a layer of mud over it"). Added up, as above, thirty holes on one patch
// came to twenty and more world units of depth in its middle, far past what the ground's shader takes for a hole (it
// looks one unit down for where the eye's ray meets them, and shades a hole's bottom by its walls): the middle read as one
// flat black sheet, with whole bowls only at its margin. Now a hole is dug into what is there instead: within its lip
// it takes the place of what was there (the older holes' walls, lips and the ruts under it are gone, as under a real
// shell), its bowl dug from the level the ground had round its middle (`base`: a small shot's scuff in a large hole's
// floor is a dimple in that floor, a shell of the same size in an older hole a whole new bowl a little lower), never
// deeper in all than kDeepestHole; beyond its lip the thrown earth lies over what is there by as much as there is of it
// (an older hole beside it is filled toward the new lip, nearest it most). So a patch struck many times is one field of
// whole bowls, the newest whole, the older cut by them, none deeper than a hole can be. A hole alone on untouched
// ground is to the last number what it was.
#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "linalg.h"

namespace render {

class TextureCache;
struct Model;

// What the shaders that read the field are told, laid out as float4s.
struct TrackFieldConstants {
  // per level: x, z of its window's first texel (whole texels of the world: a place's x / the texel's size);
  // 1 / its texel's size (world units); its texels across (0: the level is not there)
  float level[3][4] = {};
  // per level: the atlas texel its data begins at (x, y); 1 = it wraps round (levels 0 and 1), 0 = it is clamped
  // (level 2, over the map); its texel's size (world units)
  float atlas[3][4] = {};
  // x, y: 1 / the atlas's width and height; z: 1 = there is a field to read; w: unused
  float info[4] = {};
};

class DeepTracks {
 public:
  DeepTracks(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  ~DeepTracks();
  // Its pipelines and the atlas (World: the first time it is on).
  bool Init(std::string* error);
  bool Ready() const { return static_cast<bool>(atlas_); }
  // The map (world units): the field emptied, the map-wide level sized to it.
  void SetMap(float width, float height);

  // Whether it presses anything (World sets it each frame: the option, the modern look, a map).
  bool on = false;
  // The picture a script names a print by (World: Decals::Picture); none: not there.
  std::function<gfx::Texture(const std::string&)> picture;
  // The ground's own height at a place (not the water's), and the water's level (none: very low): what is
  // under water is not pressed by the units' outlines.
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;

  // A frame: the levels placed round `target` (what the camera looks at); `clock` the game's seconds.
  void Begin(Vec3 target, double clock);
  // A unit on the ground this frame, by its blueprint's file (read once: whether it hovers, flies or walks),
  // where it is, how it is turned and its model's size (world units). It presses its outline where it goes,
  // unless it lays prints of its own; a walker presses its feet where they are set down (`feet`: where the bones
  // FootBones gives are now, world units; none, or none seen setting down: prints left and right by turns).
  // ([hdprints] `model`: its own prints, Model::prints, where it has them; `footTurns`: how far each foot is turned from
  // the way the unit faces, radians, as decals' headings; none: not turned)
  void AddUnit(uint32_t id, const std::string& blueprintPath, Vec3 position, const Quat& orientation, Vec3 size,
               const Vec3* feet = nullptr, int footCount = 0, const Model* model = nullptr, const float* footTurns = nullptr);
  // A print a script lays under a unit (CreateSplatOnBone): its picture's name, its middle on the ground, its
  // size (world units) and heading (radians, as decals take it). True: it is pressed into the field (the caller
  // lays no picture of it); false: off, or its picture is not there.
  bool AddPrint(uint32_t id, Vec3 centre, float sizeX, float sizeZ, float heading, const std::string& texture);
  // [dents] A shell's hole: its middle on the ground and its size (world units across, as the scorch the game lays for
  // it: Blasts::FromImpact; a small gun's 0.2 is a scuff, an artillery shell's 1 to 3 a hole).
  void PressCrater(Vec3 at, float size);
  // Draws what pressed since the last time into the field, clears what came into the levels' windows and lets
  // the field fade, a level at a time (its own pass: before the scene's). `snowing`, `raining` 0..1: how hard;
  // `kinds`: Terrain::GroundKinds (none: all as dirt).
  void Draw(float snowing, float raining, gfx::Texture kinds);

  gfx::Texture Atlas() const { return atlas_; }
  // The bones of a walker's model that stand for its feet (eight at most), found once a model: those its
  // blueprint's Footfall names (the bones the game's scripts lay footprints at), else those named as feet, else the
  // lowest bones of its legs (most of the game's bots have no foot bones: their legs end in a shin); one a foot.
  const std::vector<uint32_t>& FootBones(const Model* model, const std::string& blueprintPath);
  // For the shaders: where the levels are this frame (all zero without a field).
  TrackFieldConstants Constants() const;

  int stamped = 0;  // (the last Draw: quads drawn into the field, the wrap's copies counted)
  int faded = -1;   // (the last Draw: the level it faded, -1 none)
  int craters = 0;  // [dents] (holes listed now)
  // [churn] A new hole takes the place of what is within its lip, dug from the ground's level there (above). False: every
  // hole added to what is there, as before (World::churnedHoles, the viewer's --holes-added).
  bool churnedHoles = true;
  // [churn] The holes' height at a place as the listed holes leave it one over another (world units; for a new hole's
  // level, and for checking the field by numbers).
  float HolesAt(float x, float z) const;
  static constexpr float kDeepestHole = 2.0f;  // [churn] (world units below the ground: the field's holes never deeper)

 private:
  static constexpr int kLevels = 3;
  // A stamp: a turned rectangle pressed in. shape 0: a picture's alpha; 1: a soft disc; 2: two bands along
  // its length (a vehicle's tracks); 5: [dents] a shell's hole (trough: its depth, berm: its lip's height, world units).
  struct Stamp {
    float x, z;            // its middle (world)
    float halfX, halfZ;    // half its size along its own x and z (world): the shape itself, without the berm's room
    float heading;         // as decals: its own +z toward (sin, cos)
    float trough, berm;    // how much it presses down, and pushes up beside it
    int shape;
    float bandX, bandHalf; // shape 2: the bands' middles and half widths, as shares of halfX; a tread's piece: the
                           // rows of its picture its alpha covers (first, last, as shares of its height)
    gfx::Texture texture;  // shape 0
    bool tread = false;    // shape 0: a piece of a tread's line (cut sharp at its ends, its berm to its sides only)
    bool hd = false;       // shape 0: [hdprints] a unit's own print (its red how deep, white deepest)
    float stripX0 = 0, stripX1 = 1;  // shape 0: the columns of its picture it is (a share of its width; [treadlines])
    // [dents] a hole pressed again as a level's window comes back over it: that level alone (-1: all), and only within
    // these whole texels of the world (what was cleared: the rest of the level holds it still)
    int level = -1;
    float clip[4] = {-1e9f, -1e9f, 1e9f, 1e9f};
    float base = 0;        // shape 5: [churn] the level its bowl is dug from (world units; at or below the ground)
  };
  struct Vertex {
    float x, y;              // in the atlas, -1..1
    float u, v;              // across the stamp (the berm's room included), -1..1
    float trough, berm;      // its amounts
    float across, tread;     // shape 0: its texels across in this level (for the picture's levels); 1 = a tread's piece
    float shape, bandX, bandHalf, margin;  // margin: the quad's size over the shape's
    float x0, y0, x1, y1;    // the atlas texels it may write (its level's region)
    float stripX0, stripX1;  // shape 0: the columns of its picture it is
    float wx = 0, wz = 0;    // [dents] the corner in whole texels of the world, and what of them it may write
    float clipX0 = -1e9f, clipZ0 = -1e9f, clipX1 = 1e9f, clipZ1 = 1e9f;
  };
  struct Crater {            // [dents] a hole of the last while: where, how large, turned how, how much of it is left
    float x, z, size, heading;
    float kept = 1;
    float base = 0;          // [churn] the level the ground had round its middle when it struck (as it was then: x kept now)
  };
  struct Level {
    int size = 0;            // texels across (its data, without the copied edges)
    float texel = 1;         // world units
    bool wraps = false;
    int atlasX = 0;          // where its region begins in the atlas (a wrapping one: at its copied edge)
    int originX = 0, originZ = 0;  // its window's first texel, in whole texels of the world
    bool placed = false;
    double fadedAt = -1;     // when it was last faded (the game's clock; -1 never)
  };
  struct UnitInfo {
    bool hovers = false, walks = false, flies = false;
    std::vector<std::string> feet;  // the bones its blueprint's Footfall names (FootBone)
  };
  struct Moving {            // a unit pressing its outline: where it last pressed, the frame it was last seen
    float x, z;
    uint32_t seen;
  };
  struct Feet {              // a walker's feet
    float height[8] = {};    // each one's height over the ground last frame
    float pressedAt[8] = {}; // the height it was set down at (it is set down again once it has risen from there)
    bool falling[8] = {};    // it has been going down since it was last set down
    bool pressed[8] = {};    // it has been set down and not risen since
    int count = 0;
    uint32_t seen = 0;
    float x = 0, z = 0;
    double stepped = -1e30;  // when a foot was last set down
    float walked = 0;        // (its feet not seen setting down: how far it went since its last print, and on which side)
    float side = 1;
  };
  struct Line {              // a line of tread prints: where its last piece ends (its front), when it was last continued
    float x, z;
    double clock;
    gfx::Texture texture;
    int phase = 0;           // [hdprints] a unit's own tread: which part of its picture the last piece was (parts: PressPrint)
    int strip = -1;          // [treadlines] which of its picture's treads it is (-1: all of the picture)
  };
  struct Picture {           // a print's picture, and the rows of it its alpha covers (as shares of its height from
    gfx::Texture texture;    // its top: the game's short treads lie at the bottom of a large square)
    float first = 0, last = 1;
    bool hd = false;         // [hdprints] a unit's own print (all of it the piece; its red how deep)
    // [treadlines] a unit's own tread or tyre picture's separate treads, side by side (as shares of its width; none when
    // it is one): each is laid as a line of its own, so that on a bend each follows its own way, the outer one longer
    std::vector<std::pair<float, float>> strips;
  };
  // [hdprints] A unit's own prints, by its blueprint (copied from its model's, Model::prints, the first time it is seen).
  struct OwnPrint {
    std::string texture;
    float width = 0, length = 0;
  };
  struct OwnPrints {
    OwnPrint tread, tyre, foot;
  };
  // A walker's footprint (`sole`: [hdprints] its own picture, sizeX x sizeZ; none: a soft disc by its width).
  void Footprint(Vec3 at, float heading, float width, float weight, const Picture* sole = nullptr, float sizeX = 0, float sizeZ = 0);
  // A print pressed by its picture (AddPrint's, once the picture is known): `foot` one where it is laid, else a tread's piece
  // continuing its line. ([treadlines] `strip`: which of the picture's treads, at `centre`, sizeX wide; `weight`: the
  // whole picture's, when one of its treads; else its own)
  void PressPrint(uint32_t id, Vec3 centre, float sizeX, float sizeZ, float heading, const Picture& pic, bool foot, int strip = -1,
                  float weight = 0);

  void Quads(const Stamp& s);
  void Quad(const Stamp& s, int level);
  Stamp CraterStamp(const Crater& c) const;  // [dents]
  // [dents] The listed holes that reach into whole texels of the world [x0, x1) x [z0, z1) of level 0 or 1, pressed again
  // there alone (that part of the level was just cleared), at what is left of them.
  void PressCratersAgain(int level, int x0, int z0, int x1, int z1);
  void ClearQuad(int level, int x0, int z0, int x1, int z1);  // whole texels of the world, [x0, x1) x [z0, z1)
  void RegionQuad(int level, float shape);                       // the level's whole region
  void Emit(const Vertex corners[4]);
  const UnitInfo& Info(const std::string& blueprintPath);
  const Picture& PictureOf(const std::string& name, bool hd = false);
  float RegionX1(int level) const;

  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline stamp_, clear_, fade_;
  gfx::Pipeline crater_;  // [churn] (a hole dug into what is there: churnedHoles)
  gfx::Buffer buffer_;
  gfx::Texture atlas_;
  int atlasWidth_ = 0, atlasHeight_ = 0;
  float mapWidth_ = 0, mapHeight_ = 0;
  Level levels_[kLevels];
  bool fresh_ = true;        // to be cleared when next drawn into
  double clock_ = 0;
  uint32_t frame_ = 0, fadeCount_ = 0;
  double lastPressed_ = -1e30;  // (when anything last pressed: long after it, nothing is left to fade)
  std::vector<Stamp> stamps_;   // since the last Draw
  std::vector<std::array<int, 5>> clears_;  // level, x0, z0, x1, z1 (x1 <= x0: the whole region)
  std::vector<Vertex> vertices_;
  std::unordered_map<std::string, UnitInfo> infos_;
  std::unordered_map<uint32_t, bool> walkers_;     // by unit: whether its blueprint walks (its prints are footprints)
  std::unordered_map<uint32_t, Moving> moving_;
  std::unordered_map<uint32_t, Feet> feet_;
  std::unordered_map<const Model*, std::vector<uint32_t>> footBones_;
  std::unordered_map<uint32_t, double> printers_;  // units that lay prints: when they last did
  std::unordered_map<uint32_t, std::vector<Line>> lines_;
  std::unordered_map<std::string, Picture> pictures_;
  std::unordered_map<std::string, OwnPrints> ownPrints_;      // [hdprints] by blueprint
  std::unordered_map<uint32_t, const OwnPrints*> unitPrints_;  // [hdprints] by unit (those that have any)
  std::vector<Crater> craters_;  // [dents] the holes of the last while (the oldest first)
  double cratersFaded_ = -1;     // [dents] (when their `kept` was last brought on)
  const bool debug_ = std::getenv("MOHO64_DEBUG_TRACKS") != nullptr;  // (MOHO64_DEBUG_TRACKS=1: what it drew, once a second)
  int debugStamps_ = 0, debugQuads_ = 0, debugFades_ = 0;
  // (MOHO64_DEBUG_TRACKS=2: also the field itself on the ground: red the trough, green the berm, blue the level read;
  // 3: what the ground's shader makes of it: red how deep, green the berm, blue how much relief shows)
  const int debugView_ = debug_ ? std::max(std::getenv("MOHO64_DEBUG_TRACKS")[0] - '1', 0) : 0;
};

}  // namespace render
