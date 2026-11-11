// Decals: pictures laid on the ground - a map's painted details, the scorch mark an explosion leaves.
//
// A decal is not a mesh. After the ground is drawn and before anything stands on it, each decal's box
// is drawn over the picture; for every pixel in it the ground's place in the world is worked out from
// the picture's depth, and where that place lies inside the decal's footprint the decal's texture is
// blended over the ground, lit as the ground under it. So a decal follows the ground exactly, at
// whatever level of detail the ground was drawn, and never climbs onto a unit.
//
// Three kinds: albedo decals (a picture over the ground's colour); normal decals, which tilt how the
// ground takes the light and so sculpt cliffs and erosion into it - here by multiplying the lit
// ground with (the light it would take tilted) / (the light it took); and lights (a glow added).
// The map editor's remaining kinds (glow, water masks) are not drawn.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "hot_metal.h"
#include "linalg.h"
#include "tarmacs.h"  // [tarmac]
#include "textures.h"
#include "view.h"

namespace moho {
class Vfs;
}

namespace render {

// [padsnow] How much snow lies on a mark's place, as the ground lays it (HLSL: MarkSnowLying(world, ground, fallen), with
// MODERN_RELIEF or DECAL_SNOW_GROUND; with the latter it reads snowShelter, snowWind, snowTune and snowMore, which the shader
// declares before it). The decals' and the remastered pads' (tarmacs.cpp) shaders take it.
extern const char kMarkSnowLying[];

class Decals {
 public:
  // [whitecaps] The ocean whose lifted waves the pictures on the water ride (ocean.h; World sets it each frame; none: they lie
  // at the water's level, as before).
  const class Ocean* ride = nullptr;
  // The resource deposits a game has marked on the ground (the script's splats mass_marker and
  // hydrocarbon_marker): where, which, and from how far the mark is drawn (its cutoff). For the
  // icons that take the marks' place from far up (world.cpp).
  struct Deposit {
    Vec3 position;
    bool hydrocarbon = false;
    float cutoff = 0;
  };
  const std::vector<Deposit>& Deposits() const { return deposits_; }
  // Is that texture one of the two marks a game lays on its resource deposits? (They belong to the
  // map: every army sees every deposit from the first moment, as the original shows them.)
  static bool IsDepositMark(const std::string& texture, bool* hydrocarbon = nullptr);
  // Snow and the marks: the host says each frame how much snow lies (0..1) and how hard it snows
  // (0..1). A mark laid on lying snow (a tread's print, a footprint, a scorch) shows ON the snow,
  // and new snowfall covers it in about a minute and a half of heavy fall; a mark that was there
  // before the snow lies under it like the ground.
  float snowLying = 0, snowing = 0;
  // [decalfade] How a decal fades out with distance (ui's request 33; World::decalFadeFraction, set by the world each
  // frame with `lod`). Negative (the viewer's default): as before - by its distance from the eye times the camera's LOD
  // scale, drawn whole up to 0.8 of its cutoff and fading to nothing at it (a GUESS). 0..1: as the original's
  // CWldTerrainDecal::GetLODAlpha (0x89d400), this being its ren_DecalFadeFraction (0.5 stock; FAF's gamemain.lua sets
  // 0.25 at every game's start): measured as the original measures detail (Lod), a decal is drawn whole up to
  // fadeFraction x its cutoff and fades evenly to nothing at the cutoff; one with a near cutoff (SetNearCutoff) instead
  // comes in from nothing at fadeFraction x that to whole at it, and is left out beyond its cutoff.
  float fadeFraction = -1;
  // [decalfade] The original's measure of detail at a decal (with fadeFraction 0..1): the width of the world its view
  // spans across at the decal's depth (GeomCamera3::Init 0x4705ed: through the near and the far plane's widths), times
  // the camera's LOD scale (SetLODScale 0x470b90). From the camera the picture is placed by (photomode: the whole
  // photo's): its eye, the way it looks, the world it spans across at a depth of 1 (2 tan(fovY / 2) x its aspect) and
  // its LOD scale (Camera::lodScale; the distance as before is multiplied by it too).
  struct Lod {
    Vec3 eye, forward{0, 0, -1};
    float widthPerDepth = 1, scale = 1;
  } lod;
  // [decalfade] A place's measure as the fade takes it (compared with a cutoff): as before, its distance from `eye`
  // times the LOD scale; with fadeFraction 0..1, the original's (Lod). For what takes a decal's place from far up (the
  // deposits' icons, world.cpp).
  float Measure(Vec3 p, Vec3 eye) const;
  // [decalfade] Gives the decal `id` (LastAdded) a near cutoff (the map's nearCutoffLOD; 0: none): with fadeFraction
  // 0..1 it is then drawn only from that far on (above). As before (fadeFraction negative) it changes nothing.
  void SetNearCutoff(uint32_t id, float nearCutoff);
  // The remastered effects look (World::effectsLook; set by the world each frame, so the map's own decals, laid
  // when it opens, stay the game's): a script's decal is read from the visuals track's own picture of it where it
  // has made one (/remastered, effect_files.h), and lit over the relief beside it where there is one (its
  // "_albedo" named "_normals": a two-channel normal map, x in red, y in green pointing up the picture, OpenGL's).
  bool remastered = false;
  // Battle scars (World::battleScars; Chris, 2026-10-17: "build it up now"): a scorch or a crater a script lays (a
  // decal whose name has "scorch" or "crater" in it: deaths, blasts, shells, a nuke's) stays past the time the
  // game gives it, weathering over `scarWeathering` seconds after that time to `scarFloor` of its strength, and
  // goes only when there are more than `mostScars`: then the oldest fades out over ten seconds. So a battlefield
  // keeps the marks of the fighting on it. Set by the world each frame; scars laid while it was off keep the
  // game's time.
  bool battleScars = false;
  // [scars] The marks' relief lit as the ground (World::markRelief, set by the world each frame; the modern look only;
  // Chris, 2026-10-20): a normal decal (a crater's dent, a trench, a cliff's erosion) changes the ground's light by how
  // much more or less of the modern light - the sun through its and the clouds' shadows, the sky from all sides - its
  // tilted ground takes than the flat, so that with the sun down it stays near 1 (the original's formula, which has no
  // sky, darkened a decal's whole square at night, down to 0.35, and lit its slopes toward the moon white: the dark
  // squares in snow, the white rims and doubled rings of old craters by night). Under lying snow the relief softens as
  // the snow fills it (as a picture's mark: laid before the snow it is under it, laid on it it shows until new snow
  // covers it). A normal picture made as a plain normal map (colour in its blue channel: x in alpha, y in green and red,
  // no share in red to say where it applies, as the remastered Seton's trenches and old craters are) applies at full
  // strength, its flat parts changing nothing (read as the game's, its red - the slope - made one side of every trench
  // a dark strip). Off: as before.
  bool modernRelief = false;
  void SetModernRelief(bool on);  // (each frame: its shader made the first time it is on)
  int plainNormals = 0;           // (the last Draw: normal decals taken as plain normal maps)
  // [decalsnow] The snow ground (World::snowGround; terrain.h, Terrain::SnowShelterLaid): while the ground lays its snow from
  // its shelter and materials, a mark lays none of its own. Where the snow lies over a mark (laid before the snow, or covered
  // by new snow since), the mark thins out by as much as lies there, so that the ground's own snow shows through it -
  // sheltered under trees, drifted, textured by its materials - and the mark itself is lit bare; the snow over it reckoned as
  // the ground's (deeper in the lee and hollows, thinner and patchier under trees, on steep, windward and scoured ground: the
  // shelter's and the wind's numbers, terrain.cpp's SnowAround), and so is the relief's softening under it (modernRelief).
  // Before, a mark laid the one constant snow colour over its whole picture, which showed as its shape on the ground's snow.
  // Set by the world each frame before Draw; no shelter: as before.
  struct SnowGround {
    gfx::Texture shelter;           // (none: the ground lays no snow of its own now)
    float wind[2] = {1, 0};         // the way the wind blows (as Terrain::snowWind)
    float tune[4] = {1, 1, 1, 1};   // Terrain::snowTune
    bool materials = false;         // the snow's materials drawn (the ground's wider, mottled margin)
    float mapWidth = 0, mapHeight = 0;
  } snowGround;
  // (the snow ground's pipelines, made the first time a shelter is given: before the scene's pass)
  void PrepareSnowGround();
  int mostScars = 2000;
  float scarWeathering = 600, scarFloor = 0.5f;
  int Scars() const { return scars_; }  // (scars there now, the ones fading out included)
  // [settled] Settled scars (World::settledScars, set by the world each frame; the review of 2026-11-09, ground 14; Chris:
  // a place that looks lived in and fought over). As above, the scars are kept as single pictures, each drawn and lit by
  // itself, and only `mostScars` of them: in a long game the scars of the first battles went as the most was passed, and
  // where a battle piled dozens on one spot the ground there was lit once for every layer (500 scorches on one patch
  // seen from close: 2.6 ms of the card's time at 1920 x 1080, 2.4 times the ground's own). With this on:
  //   - a scar that has weathered to its floor, and its glow gone out (hot metal), is SETTLED: it leaves the list the
  //     most counts and is kept, by where it lies (squares of kSettledCell world units, so that a frame looks only at the
  //     squares in view), for the whole game, up to `mostSettled`. Past `mostScars` the oldest scar no longer fades out:
  //     it settles as it is (once its glow is out) and goes on weathering there. So the most only bounds the fresh ones.
  //   - it settles while no view shows it (not shown for half a second: off the screen, or beyond its cutoff), so that
  //     what changes when it does (its place in the order, below) is never seen to change; only one that settles
  //     because the most is passed may do so in view, where before it faded out altogether. (`settleInView`: at once,
  //     for tests.)
  //   - where the settled scars in view cover much (layer by layer more than 0.6 of the scene's pixels: a patch shelled
  //     for minutes, seen from close) they are laid, unlit, into one picture of the scars as large as the scene's
  //     (LaySettled, before Draw: first what the place itself leaves of any of them - the slope, the shell holes - once a
  //     pixel; then each scar's box draws its colour and how much of it is there over what is there, and a second time
  //     its relief's lean and its share of the snow: three texture reads a layer), and that picture is then lit ONCE over
  //     the ground, as a mark is (Draw). The light of a mark is its colour times the light plus what is the same for
  //     every mark, so one lighting of the layers laid over each other is the layers lit one by one and laid over each
  //     other; only where scars with different relief overlap does the lean of the upper one count by how much of it is
  //     there, in place of each lit by its own (500 on one patch: 0.5 % of the picture's pixels differ by more than 2 of
  //     255; scars apart from each other: none by more than 2). The same 500: 1.05 ms. Under that cover their own picture
  //     would cost more than it saves (three passes over the whole scene, 0.13 ms): each settled scar in view is then
  //     drawn as a picture is, to the last pixel as before it settled (`litOnce`: 1 always their own picture, 0 never,
  //     -1 as it pays; tests).
  // A settled scar is in everything else a scar as before: faded by its distance as its cutoff says, gone on steep
  // ground and over a shell hole, under or on the snow by when it was laid. A crater's dent (a normal decal) settles too
  // and is drawn as before (it multiplies the ground's light: the order does not count).
  // What changes when a scar settles is its place in the order: the settled ones stand together where the lowest
  // texture of them stands among the pictures (their own picture is lit there; drawn one by one they are drawn there),
  // so a settled scar lies under every scar of a higher texture not yet settled, where before the two were drawn in
  // the order of their textures, the older over the newer as often as not. (60 scars on a patch, half of them settled:
  // 0.2 % of the picture's pixels differ by more than 2 of 255, none by more than 13.)
  // Why not a picture kept in the ground's own space, as the deep tracks' field (the review's sketch): every scar has
  // its own cutoff (10 to 600 world units: a bullet's scuff is drawn from close only, a death's scorch from far), its own
  // relief, and a size from 0.04 to 28 world units (in a late 4v4 half of the 1083 under 0.5 units across) with a
  // picture of 128 and more texels across: no texture over the map can hold them (a 0.5-unit scuff wants 250 texels a
  // unit close by; 4 a unit, 4096 x 4096 on a 20 km map, would blur every scar the moment it settled), only a picture
  // made for where the camera is and what it shows - which this is, each frame, at the screen's own detail, from the
  // scars' own pictures. What is kept for the whole game is the list (some 130 bytes a scar), not a texture: so a lost
  // graphics device loses none of it (KeepScars).
  // The scars' picture: two targets of four half floats a pixel and one of one byte, as large as the scene (1920 x 1080:
  // 35 MB; made when first needed, let go ten seconds after no view needed one).
  // false: as before, to the last pixel. Scars settled while it was on go back among the others when it is turned off.
  bool settleScars = false;
  bool settleInView = false;
  int litOnce = -1;
  void SetSettleScars(bool on);  // (each frame, after battleScars: its shaders made the first time it is on)
  int mostSettled = 60000;
  int Settled() const { return settledCount_; }  // (settled scars kept now)
  int settledLaid = 0;                           // (the last frame: settled pictures laid into the scars' picture)
  static constexpr float kSettledCell = 32.0f;
  // [settled] The settled scars' picture for this frame (above): what has weathered to its floor settles, and the settled
  // pictures in view are laid into a picture of their own, in passes of its own: so called by the world after the
  // ground is drawn and BEFORE the pass Draw is in (`sceneDepth` as Draw's: the scene's depth with the ground in it,
  // not bound to be drawn into). Not called in a frame: Draw draws no settled scar.
  void LaySettled(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now);
  // [settled] (World::Keep / Give, [devicelost]) The battle scars a world made anew on another graphics device takes
  // over, with settleScars: every scar past the time the game gave it, settled or not (those not yet past it the scripts'
  // effects lay again, Effects::Keep), as it was laid: its picture's name, place, size, when, and how much of the lying
  // snow covers it. KeepScars reads this object's own memory alone, nothing of the device; GiveScars gives them to a
  // map just opened, before its first frame: they are laid with that frame (LaySettled), when the host has set the looks
  // their pictures are read by (they are the game's, not the map's own). Without settleScars: none, as before.
  struct KeptScar {
    std::string texture;
    Vec3 centre;
    float sizeX = 0, sizeZ = 0, heading = 0, cutoff = 0;
    double born = 0;
    float seconds = 0, snow = 1;
    bool normal = false, settled = false;
  };
  void KeepScars(double now, std::vector<KeptScar>* out) const;
  void GiveScars(const std::vector<KeptScar>& scars) { givenScars_ = scars; }
  static bool IsScar(const std::string& texture);
  // Hot metal (World::hotMetal, hot_metal.h; set by the world each frame, with `heat` for its timing): a scorch or a
  // crater laid while it is on (IsScar) has its middle glowing and cooling, seconds to a minute by its size, from when
  // it was laid. ForEachHot: those glowing now, f(centre, size across, age, seconds, id).
  bool hotMetal = false;
  const HotMetal* heat = nullptr;
  template <class F>
  void ForEachHot(double now, F&& f) const {
    if (!hotMetal || !heat) return;
    for (const Decal& d : decals_) {
      if (!d.burn) continue;
      const float age = static_cast<float>(now - d.born), across = 2 * std::max(std::fabs(d.halfX), std::fabs(d.halfZ));
      const float seconds = heat->CraterSeconds(across);
      if (age >= 0 && age < seconds) f(d.centre, across, age, seconds, d.id);
    }
  }
  // The picture a script's mark is named by (a path, or a short name looked up in the game's two
  // folders of them); none: not there.
  gfx::Texture Picture(const std::string& texture) { return Resolve(texture, nullptr, nullptr); }
  // [tarmac] Remastered tarmacs (tarmacs.h; World::remasteredTarmacs, set by the world each frame: the modern look with the
  // remastered effects): a pad the game laid whose picture the visuals track has made a material for is drawn in that
  // material (its "_normals" twin, the game's relief for it, left out), after the map's own pictures and before the marks
  // laid since, so that scorches and tread marks lie on it. `tarmacs` takes the world's settings for them. A pad laid by
  // the original effects (the effects stand's right side: `addingOriginal` while Effects lays it) keeps the game's
  // pictures. false: every pad the game's, as before.
  bool remasteredTarmacs = false;
  Tarmacs tarmacs;
  bool addingOriginal = false;
  // [steep] the effects stand's --fx-decal: laid as the game lays one, not as one of the map's own, though it is there
  // before the first frame (the map's own keep their whole footprint on steep ground, the game's fade out there)
  bool addingGames = false;
  // [tarmac] The pads drawn in their materials now (with remasteredTarmacs), each with its fade by its time alone: for
  // the ground cover cleared under them (Tarmacs::ClearCover).
  void TarmacPads(double now, std::vector<Tarmacs::Pad>* out);
  int tarmacsDrawn = 0;  // [tarmac] (in the last Draw)
  Decals(gfx::Device* device, const moho::Vfs* vfs, TextureCache* textures) : tarmacs(device, textures), device_(device), vfs_(vfs), textures_(textures) {}
  bool Init(std::string* error);

  // A decal: `texture` is a path, or a script's short name ("Scorch_generic_002_albedo": looked for
  // in /env/common/decals and /env/common/splats). `centre` is on the ground; `sizeX`, `sizeZ` its
  // whole extent in world units; `heading` turns it about the upright axis (radians).
  // `cutoff`: farther from the camera than this it is not drawn (0: always; measured and faded as `fadeFraction` says).
  // `born`, `seconds`: when it was made and how long it stays (seconds <= 0: for good); it fades
  // out over its last five seconds, or over the second half of its time when that is shorter (a
  // tank's tread marks last a second). Returns false when the texture is not there.
  bool Add(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The same for a normal decal (its texture: the tilt along its own x in alpha, along its own z in
  // green, how much of it to apply in red).
  bool AddNormal(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The same for a "Water Albedo" decal (the campaign's objective areas): in its own colours, unlit
  // (the original's TDecalsWaterAlbedo: times the map's light multiplier), and where the ground under
  // it lies under water, on the water's surface (drawn by the second Draw, after the water).
  bool AddWater(const std::string& texture, Vec3 centre, float sizeX, float sizeZ, float heading, float cutoff, double born = 0, float seconds = 0);
  // The number of the decal the last Add, AddNormal or AddWater laid (from 1), for Remove.
  uint32_t LastAdded() const { return lastId_; }
  // Takes that decal away at once (a script's DecalHandle:Destroy: an objective done, a building's
  // tarmac laid again with a life time when it is killed).
  void Remove(uint32_t id);
  // [restart] (World::RestartGame, the interface's request 45 R1) A new game on the same map: what the game laid let go of,
  // the map's own kept. Every decal laid after the one numbered `lastOfMap` (LastAdded as the map was opened) goes, and
  // every light on the ground (they have no number), the resource deposits' marks after the first `depositsOfMap`, and
  // this frame's marks. The snow that fell on the marks is forgotten, and the marks laid before the next frame are taken
  // as laid before the first (as for a map just opened). The numbers go on counting.
  void ForgetGame(uint32_t lastOfMap, size_t depositsOfMap);
  // The ground's height (not the water's) at a place, given by the host: a decal whose place is
  // given far from the ground (the campaign's objective areas are made at a height of 0) is put
  // on the ground there. `waterLevel`: where the water's surface is (the map's; none: very low).
  std::function<float(float, float)> groundHeight;
  float waterLevel = -1e9f;
  // A glow on the ground that adds its light for `seconds`, coloured by `ramp` along that time:
  // the light an explosion throws on its surroundings (the original's "light particle", there a
  // flat sheet drawn over everything; here it lies on the ground). `texture` and `ramp` are paths;
  // `size` is half its edge.
  bool AddLight(const std::string& texture, const std::string& ramp, Vec3 centre, float size, double born, float seconds);

  // A mark for this frame only, drawn in its own colours whatever the light (the ring under a
  // selected unit). Forgotten after the next Draw.
  void AddMark(gfx::Texture texture, Vec3 centre, float sizeX, float sizeZ, float heading);
  // The map's water: its depth picture and colour ramp (Terrain::WaterMap, WaterRamp) and the map's
  // size in world units. Pictures lying under water are then tinted as the ground there is.
  void SetWater(gfx::Texture waterMap, gfx::Texture waterRamp, float mapWidth, float mapHeight) {
    waterMap_ = waterMap;
    waterRamp_ = waterRamp;
    mapWidth_ = mapWidth;
    mapHeight_ = mapHeight;
  }
  // [steep] The ground's heights (Terrain::HeightTexture, one sample a map corner: the map's size + 1 each way) and what
  // one of its values is in world units (Terrain::HeightUnit): the slope under a picture the game laid is read from it.
  void SetGround(gfx::Texture heights, float unit, float mapWidth, float mapHeight) {
    groundHeights_ = heights;
    groundUnit_ = unit;
    heightsWidth_ = mapWidth + 1;
    heightsHeight_ = mapHeight + 1;
  }
  // [churn] Shell holes under the marks (deep_tracks.h's holes; World::churnedHoles, set by the world each frame before
  // Draw; Chris's test of 2026-11-09: "some parts will look like a hole while others look like a layer of mud over
  // it"). A picture the game lays (a shot's scorch, kept as a battle scar) is a flat picture thrown straight down on the
  // ground's own surface: it knows nothing of the holes the ground's shader draws there, so a scorch lay over a hole
  // like a black sheet - at the height of the ground before the hole, lit flat, hiding the bowl's walls and its shade -
  // while the hole beside it, whose scorch had faded or lay elsewhere, read as a hole. Where the ground shows a hole
  // (as deep as it holds it there: by its kind, all of it under lying snow) a picture the game laid now gives way, from
  // 0.03 world units of depth to nothing at 0.2: the hole's own torn, dark earth shows in the bowl, and the burnt ground
  // stays round it, on its lip and on the flat between the holes. A scuff (a small gun's) keeps its scorch. The map's
  // own pictures and the pads are left as they are. `field` none: as before.
  struct Holes {
    gfx::Texture field;             // DeepTracks::Atlas (none: no holes are read)
    gfx::Texture kinds;             // Terrain::GroundKinds (none: all as dirt)
    TrackFieldConstants constants;  // DeepTracks::Constants
  } holes;
  // Draws the decals into the current pass, which must be the scene's colour alone (no depth
  // target); `sceneDepth` is the scene's depth with the ground in it. `overWater`: the second
  // drawing, after the water: only the "Water Albedo" decals' parts that lie over water.
  void Draw(const View& view, const Camera& camera, gfx::Texture sceneDepth, double now, bool overWater = false);
  bool AnyWater() const { return waterDecals_ > 0; }

  float glow = 2.5f;  // the modern look's brightness of a light on the ground (as Particles::glow)
  int drawn = 0;  // in the last Draw
  int Count() const { return static_cast<int>(decals_.size() + marks_.size()) + settledCount_; }

 private:
  struct Decal {
    gfx::Texture texture, ramp;  // ramp: only a light has one
    Vec3 centre;
    float halfX, halfZ, heading, cutoff;
    double born;
    float seconds;
    bool normal = false;
    bool mark = false;
    bool water = false;  // a "Water Albedo" decal
    uint32_t id = 0;     // (laid by Add: from 1)
    float snowAtBirth = 0;  // snowFallen_ when it was laid (see `snowLying`)
    gfx::Texture relief;    // a remastered picture's own relief (`remastered`), or none
    bool ours = false;      // the picture is the visuals track's (`remastered`)
    bool scar = false;      // a battle scar (`battleScars`), and when it has faded out (0: it has not begun to)
    double scarGone = 0;
    bool burn = false;      // a scorch or crater laid with hot metal on (`hotMetal`): its middle glows while fresh
    int tarmac = -1;        // [tarmac] a pad's picture: its name among tarmacKeys_ (-1: not a pad's)
    bool original = false;  // [tarmac] laid by the original effects (`addingOriginal`)
    bool mapOwn = false;    // [tarmac] one of the map's own (laid before the first frame)
    float nearCutoff = 0;   // [decalfade] (SetNearCutoff)
    mutable double seenAt = -1e30;  // [settled] when a view last showed it (Show; seconds of the wall's clock, wall_)
    uint16_t name = 0xffff;         // [settled] a scar's picture's name among scarNames_ (KeepScars)
  };
  // [decalfade] How much of a decal its distance from the eye leaves (fadeFraction, lod): 0..1, or -1 where it is not
  // drawn at all (beyond its cutoff).
  float LodAlpha(const Decal& d, Vec3 eye) const;
  struct Instance {  // as the shader reads it
    float centre[3], halfHeight;
    float halfX, halfZ, cosHeading, sinHeading;
    float alpha, age, light, glow;  // age: 0..1 of a light's time; light: 1 = adds its light, 2 = a normal decal;
                                    // glow: a light's brightness, a picture's 1 when it has its own relief
  };
  // The picture a decal is named by (none: not there), and with `remastered` its relief where there is one.
  // `ours`: it is the visuals track's (a normal decal of theirs: two channels, y up the picture).
  gfx::Texture Resolve(const std::string& texture, gfx::Texture* relief, bool* ours, std::string* found = nullptr);
  // [scars] (modernRelief) the files of the normal decals' pictures (the game's, not the visuals track's), by texture;
  // and whether each is a plain normal map (its blue channel not empty), read once, the first time it is wanted
  std::unordered_map<uint32_t, std::string> normalFiles_;
  std::string lastFile_;  // (the last Add's)
  std::unordered_map<uint32_t, bool> plainOf_;
  bool Plain(uint32_t texture);
  gfx::Pipeline normalsModern_;
  bool reliefTried_ = false;
  // [decalsnow] the pictures' and the relief's pipelines with the snow ground's (DECAL_SNOW_GROUND), made the first time
  gfx::Pipeline picturesSnow_, normalsModernSnow_;
  bool snowTried_ = false, reliefSnowTried_ = false;
  float snowFallen_ = 2;    // all the snow that ever fell, in coverings of a mark (it only grows)
  double snowClock_ = -1;   // when it was last moved on
  bool settled_ = false;    // false until the first frame: marks laid before it are the map's own, old
  uint32_t nextId_ = 1, lastId_ = 0;
  int waterDecals_ = 0;     // "Water Albedo" decals there now
  int scars_ = 0;           // battle scars there now

  gfx::Device* device_;
  const moho::Vfs* vfs_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, lights_, normals_;
  gfx::Pipeline waterRide_;  // [whitecaps] (the pictures on the water riding the lifted waves: made the first time)
  bool rideTried_ = false;
  gfx::Pipeline hot_;  // (hot metal: a fresh mark's glow, added, its alpha left alone)
  gfx::Buffer cube_;
  gfx::Texture waterMap_, waterRamp_;
  gfx::Texture groundHeights_;  // [steep]
  float heightsWidth_ = 1, heightsHeight_ = 1, groundUnit_ = 0;
  float mapWidth_ = 0, mapHeight_ = 0;
  std::vector<Decal> decals_;
  std::vector<Deposit> deposits_;
  std::vector<Decal> marks_;  // this frame's
  struct Visible {
    uint32_t texture, ramp, relief;
    int kind;  // drawn in this order: 0 pictures, 1 normal decals, 2 lights, 3 marks
    Instance instance;
    bool mapOwn = false;  // [tarmac] (with remastered pads drawn: the map's own pictures before them, the rest after)
    bool settled = false;  // [settled] not a decal: the place the settled scars' picture is lit at
    bool old = false;      // [settled] a settled scar drawn as a picture is (too few in view for their own picture)
    uint32_t id = 0;       // [settled] (a settled picture's own number: laid in the order they were made)
  };
  std::vector<Visible> visible_;
  // [settled] (settleScars) The settled scars of one square of the ground (the oldest first), and what a frame has to
  // know of them all to pass the square over: how far a scar of it reaches past its middle, the lowest and highest of
  // their middles, the farthest cutoff (anyCutoff false: one of them has none).
  struct Cell {
    std::vector<Decal> scars;
    float reach = 0, low = 1e30f, high = -1e30f, cutoff = 0;
    bool anyCutoff = true;
  };
  std::unordered_map<uint64_t, Cell> cells_;
  int settledCount_ = 0;
  static uint64_t CellKey(float x, float z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(x / kSettledCell)))) << 32) |
           static_cast<uint32_t>(static_cast<int32_t>(std::floor(z / kSettledCell)));
  }
  // How much of a battle scar is there by its age (weathering after the game's time; fading out once the most is passed).
  float ScarAlpha(const Decal& d, double now) const;
  bool Glowing(const Decal& d, double now) const;  // (hot metal: its middle still glows)
  void Settle(Decal&& d);                          // (into its square; past mostSettled the oldest of all goes)
  void Unsettle(double now);                       // (settleScars turned off: all back among the others)
  // A decal as the shader draws it this frame, or false: not in view, beyond its cutoff, or not yet there.
  bool Show(const Decal& d, const View& view, double now, bool overWater, Visible* out);
  std::vector<Visible> settledVisible_;       // this frame's settled pictures, in the order they are laid
  std::vector<Visible> settledDents_;         // this frame's settled dents in view (drawn by Draw as before)
  bool settledNow_ = false;                   // LaySettled has been called for the frame Draw draws
  bool settledLit_ = false;                   // (and has laid a picture for Draw to light)
  uint32_t settledFirst_ = 0;                 // (the lowest texture among those laid: where their picture is lit, Draw)
  struct SettledPicture {                     // the scars' picture at one size of the scene (a side picture has its own)
    int width = 0, height = 0;
    gfx::Texture colour, lean, place;         // (place: what the slope and the holes leave of a scar at each pixel)
    double usedAt = 0;                        // (wall_)
  };
  std::vector<SettledPicture> settledPictures_;
  int settledPicture_ = -1;                   // (this frame's)
  gfx::Pipeline settledLay_[2][2];            // [colour; lean and snow][without, with the snow ground's]
  gfx::Pipeline settledLight_[2];             // [without, with the snow ground's]
  gfx::Pipeline settledPlace_;
  bool settledTried_[2] = {};
  bool settledSnow_ = false;                  // (this frame's are laid, and so lit, with the snow ground's)
  bool settledPays_ = false;                  // (the scars in view had their own picture last frame: litOnce)
  double wall_ = 0;                           // (the wall's clock at this drawing, seconds: Decal::seenAt)
  std::vector<std::string> scarNames_;        // (the scars' pictures' names as the scripts gave them: Decal::name)
  uint16_t ScarName(const std::string& texture);
  std::vector<KeptScar> givenScars_;          // (GiveScars: laid by the next LaySettled)
  void LayGivenScars();
  bool SettledPipelines(bool snow);
  void CameraConstants(const View& view, const Camera& camera, float* twenty) const;
  void SnowConstants(const float* cameraConstants, float* thirtyTwo) const;
  // [tarmac] the pads' names (Tarmacs::Key) and the material each is drawn in (-2: not looked up yet); this frame's pads
  std::vector<std::string> tarmacKeys_;
  std::vector<int> tarmacMaterial_;
  std::vector<Tarmacs::Pad> pads_;
  int TarmacOf(const std::string& texture);
  int TarmacMaterial(const Decal& d);
  bool TarmacPad(const Decal& d, double now, const View* view, Tarmacs::Pad* pad);
  std::vector<Instance> batch_;
};

}  // namespace render
