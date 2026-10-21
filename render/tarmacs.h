// [tarmac] Remastered tarmacs (World::remasteredTarmacs; Chris, 2026-10-19: "We should remake it too so its more detailed
// and nicer as well"): the pads the game lays under its buildings, drawn in the visuals track's materials.
//
// The game lays a structure's pad as two decals (StructureUnit's CreateTarmac: the blueprint's Display.Tarmacs Albedo as an
// "Albedo" decal, its Normal as an "Alpha Normals" one, both Width x Length round the structure, turned as it is), and
// lays them again with a life time when the structure dies. Where the remastered ground's remap.json names a material for
// the Albedo picture (its top level "tarmacs": {"Tarmacs/Tar8x_01_albedo": "tarmac_uef_8x", ...}, each material in
// "materials" as the ground's are: albedo with the pad's coverage in its alpha, normal, ARH), the pad is drawn in that
// material in place of the game's two pictures, over `extent` times its size round the same middle (room for a worn rim):
//   - lit with the modern light: its own normal on the ground's slope, its creases (occlusion) and roughness;
//   - wet and glossy in rain: the water gathers in its joints and low places first (lighting.h takes it as a puddle);
//   - its relief close by, as the ground's (terrain.cpp's GroundParallax, its march copied here): its slabs stand up out
//     of the gravel and its joints sink as the eye passes, within the same reach, fading out the same way; its depth by
//     the size it is drawn at over its material's real_size_m;
//   - deep tracks scuff it (hard ground: darker where wheels and treads pass, the snow on it packed along them);
//   - the marks laid on it later (scorches, craters, the game's tread marks) lie over it as over the ground.
// It fades as the game fades the pad (a dead structure's, laid again with a life time). The ground cover is cleared under
// it by its coverage (ClearCover: a picture over the grass's square that the grass reads). The original look, the original
// effects and the pads no material is named for (Aeon, Cybran, Seraphim until their art comes) keep the game's own.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "deep_tracks.h"
#include "gfx.h"
#include "linalg.h"
#include "textures.h"
#include "view.h"

namespace render {

class Tarmacs {
 public:
  Tarmacs(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  ~Tarmacs();

  // The game's name of a decal's picture as the table is keyed: lower case, forward slashes, without the folder the
  // script's short names are looked up in and without ".dds" ("tarmacs/tar8x_01_albedo"). Empty: not a tarmac's.
  static std::string Key(const std::string& texture);
  // The material a game picture is drawn in (an "_albedo" picture, or its "_normals" twin: the same pad), or -1: none
  // (not named, or its files cannot be read). The table is read the first time it is asked.
  int MaterialOf(const std::string& key);
  // How large the material draws a pad (its "extent": 1 the game's Width x Length), and how deep its relief stands,
  // world units from its lowest to its highest point, for a pad `width` world units wide (the size it is drawn at over
  // its real_size_m, times `relief` and `depth`; at most a quarter of a world unit as the ground's).
  float Extent(int material) const;
  float Depth(int material, float width) const;
  bool Usable(int material) const {  // (false once its files could not be read)
    return material >= 0 && material < static_cast<int>(materials_.size()) && !materials_[static_cast<size_t>(material)].failed;
  }

  // A pad to draw: its middle on the ground; half its size along its own x and z, the extent taken in (negative:
  // mirrored, as a decal's size); its heading's cosine and sine; how much of it is there (its fade) and how much of the
  // lying snow covers it (as a decal's mark, decals.h); its relief's depth (Depth); its material.
  struct Pad {
    Vec3 centre;
    float halfX, halfZ, cosHeading, sinHeading;
    float alpha, snow, depth;
    int material;
  };
  // This frame's settings, set by the host before Draw (Decals takes them from the world): the ground's relief close
  // by (World::groundParallax: on, its reach and how much it shades itself, Terrain's numbers); the picture of the
  // ground's tracks (Terrain::tracksMap: the trodden snow's, or deep tracks' field with `field` saying where its
  // levels are); the tier (the ground's: 8k with ultraGround).
  bool parallax = false;
  float parallaxReach = 20, parallaxShadows = 1;
  gfx::Texture tracksMap;
  TrackFieldConstants field;
  bool ultra = false;
  // [padsnow] The snow ground (decals.h, Decals::snowGround), set by the decals before Draw: while the ground lays its snow
  // from its shelter, a pad lays none of its own: it thins out by as much snow as lies over it (its own share, as a mark's;
  // reckoned as the ground reckons it: MarkSnowLying), so that the ground's snow shows through, and the pad is lit bare.
  // `snow`: the decals' snow constants (the wind's way and the map's size, the tuning, the materials). No shelter: as before.
  gfx::Texture snowShelter;
  float snow[12] = {};
  float relief = 0.12f;  // world units of relief from lowest to highest at a pad's real size (viewer --tarmac-relief)
  float depth = 1;       // times this (the ground's parallaxDepth, --ground-parallax-tune's first)

  // Draws `pads` into the decals' pass (the scene's colour alone; `sceneDepth` the ground's depth), with the decals'
  // camera block (decals.cpp: the camera's axes and the map's water, `camera16`) and their box (`cube`). Each material's
  // files are read the first time a pad of it is drawn. Returns how many were drawn.
  int Draw(const View& view, const float* camera16, gfx::Texture sceneDepth, gfx::Texture waterMap, gfx::Texture waterRamp, gfx::Buffer cube,
           const std::vector<Pad>& pads);
  // The ground cover cleared under the pads: their coverage drawn over the grass's square (GroundCover::Area: its first
  // cell's x, z, a cell's size, cells a side) into a picture of its own, which the grass reads (GroundCover::cleared).
  // Between passes. None: no pad there (or it cannot be drawn).
  gfx::Texture ClearCover(const View& view, const float* area, const std::vector<Pad>& pads);
  int drawn = 0;  // in the last Draw
  // [hitches] Read ahead (World::readAhead, set by the host each frame): a material's pictures are wanted as soon as a pad
  // in it is laid anywhere (Want, from Decals: in view or not), read on the textures' own thread (FetchLater) and made one
  // a frame (Ahead, once a frame), so that the first frame a pad of it is in view (zooming in to it) finds them made and
  // does not read them then; and the pads' pipelines are made the first frame they are on, not at the first pad drawn.
  // What is made is what Load would make at that first frame (the same pictures at the same tier).
  bool readAhead = false;
  void Want(int material);
  void Ahead();

 private:
  struct Material {
    std::string name, base;  // base: the files' path up to "albedo_4k.dds"
    float realSize = 0, extent = 1;
    bool has8k = false;
    gfx::Texture albedo, normal, arh;
    std::string tier;  // the tier read ("": not yet)
    bool failed = false;
    std::string aheadTier;  // [hitches] (the tier read ahead, Want; "": not wanted)
  };
  std::string TierOf(const Material& m) const { return ultra && m.has8k ? "8k" : "4k"; }  // [hitches] (Load's)
  bool ReadTable();
  bool Load(Material& m);
  bool Pipelines();
  gfx::Device* device_;
  TextureCache* textures_;
  bool tableRead_ = false;
  std::unordered_map<std::string, int> table_;  // by Key: the material
  std::vector<Material> materials_;
  int made_ = 0;  // the pipelines: 0 not made yet, 1 made, -1 they cannot be
  gfx::Pipeline pipeline_, cover_;
  gfx::Pipeline snowPipeline_;  // [padsnow] (DECAL_SNOW_GROUND: made the first time the snow ground is given)
  int snowMade_ = 0;
  bool SnowPipeline();
  gfx::Texture coverField_;
  std::vector<float> batch_;
};

}  // namespace render
