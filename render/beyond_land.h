// (beyondland) The world beyond the map's edge, part 1 (Chris, 2026-10-18, of the snow showcase's horizon: "it seems too
// sharp like nothing is there maybe we can add some more stuff so it doesn't seem too empty"): a softer edge, and land
// far off round the horizon. World::beyondLand; modern look, with the world beyond the edge (World::worldBeyond).
//
// The softer edge is the shaders' (lighting.h's BeyondFar, sight.cpp), by the view's `beyondLand` (SetView): past the
// edge the ground dims and greys little by little instead of behind a dark line, and the fog of war goes on there from
// what it is at the edge.
//
// The land far off: coastlines, hills and ridges of mountains from about a map's width beyond the map's own mirrored
// continuation (Terrain::beyondReach) out to several map-widths, made up from the map itself and the same every time it
// is opened. Which way it lies is read from the map's edges: where an edge holds land, more land lies beyond it that
// way; where it holds sea, open sea, with an island now and then; an island map in open sea has little round it, a map
// with no water land all round. It is one mesh, a ring of ground round the map whose points lie closer together near
// it and farther apart out toward the horizon, and only its triangles above the sea are drawn. It is coloured from the
// map's own ground (the map's colours where its ground is low by the water, level, high and steep: Terrain's
// GroundColours by its heights), lit by the sun and the sky as the ground is, shadowed by its own hills (each point
// looks toward the sun over the ring's heights), capped with snow on its highest ridges and lower down the more snow
// lies (World::snowLying), and lost with the distance in the haze the world beyond the edge is lost in (lighting.h,
// BeyondBack) or, above the horizon, in the sky's own haze there. It never lies on the map, nor between the game's
// camera and the map. No props.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gfx.h"
#include "scmap.h"
#include "view.h"

namespace render {

class Terrain;

class BeyondLand {
 public:
  // The look, changeable between frames (the viewer's --beyond-land-look name=value,...: Set). The land's own numbers
  // (from `begin` on) take hold when it is made again (Forget).
  float band = 10;           // the soft edge: over how many world units past the edge the ground dims and greys
  float edgeShadow = 0.45f;  // how dark the soft shadow right along the edge is (0 none .. 1 black) ...
  float edgeWidth = 2.5f;    // ... and over how many pixels it fades (to a third), at any zoom
  float edgeGrey = 0.35f;    // how much of its colour the ground past the edge loses
  float haze = 2400;         // over how many world units from the eye the land far off is lost in the haze by 1/e ...
  float hazeHeight = 700;    // ... near the sea; the haze thins with the height, to 1/e over this many world units
  float begin = 0.15f;       // where the land may begin: this many map sizes beyond the map's mirrored continuation
  float end = 4.0f;          // how far out it goes: this many map sizes past the map's edge
  float hills = 35;          // how high its hills rise over the sea (world units)
  float mountains = 220;     // how high its highest ridges rise at most (world units)
  float islands = 1;         // how often an island rises out of open sea (0 never)
  float farLand = 1;         // how much the far end takes the map's edges all round instead of those that way: far out,
                             // land may lie off a sea edge too, as much as the map's edges hold land at all (0 never)
  float snowline = 0.55f;    // with no snow lying: the share of the ridges' height above which snow lies
  float softness = 0.06f;    // how soft the hills' shadows are (the slope over which a shadow's edge fades)
  // Sets those named in a list "haze=2400,mountains=170,..." (the names as above); false and why when one is wrong.
  bool Set(const std::string& list, std::string* why);

  // A map has been opened (World::Open, right after its ground is loaded, before a game changes its heights; `path`
  // its file, "/maps/x/x.scmap"): what the land far off is made from is read from it, and the land is made for it the
  // next time it is wanted. The same map makes the same land every time: its shapes come from the map file's name and
  // size (so that a map being worked on keeps its horizon), which way it lies from what the map's edges hold.
  void Forget(const MapData& map, const std::string& path);
  // Into the view's constants (`beyondLand`): the soft edge. World::Begin, with the world beyond the edge on (first;
  // then, with a time of day, `thickAir`: how thick the weather makes the air, the share of light lost per world unit,
  // which out there hides all of the land in time: the view's own is held back near the map).
  void SetView(ViewConstants* c);
  float thickAir = 0;
  // Before the scene's pass, every frame (`on`: the option, with the world beyond the edge, in the modern look, not
  // for a side picture): the land made for the map the first time it is wanted, and its colours from the map's ground
  // (that dispatches, so outside a pass).
  void Prepare(gfx::Device* device, bool on, Terrain& terrain);
  // Into the scene's pass, after what stands on the map and before the clouds and the sky (the small lights bound).
  bool On() const { return now_; }
  void Draw(const View& view);
  int Triangles() const { return static_cast<int>(indexCount_ / 3); }

 private:
  void Make(float reach);
  bool MakePipelines();
  gfx::Device* device_ = nullptr;
  gfx::Pipeline pipeline_, palettePass_;
  gfx::Buffer vertices_, indices_;
  gfx::Texture heights_, palette_;
  uint32_t indexCount_ = 0;
  bool made_ = false, tried_ = false, failed_ = false, paletteMade_ = false, now_ = false;
  int rings_ = 0, round_ = 0;              // the ring's points: out from the map, and round it
  float middle_[2] = {0, 0};               // the map's middle (world x, z)
  float inner_ = 1, outer_ = 2;            // the innermost and outermost rings' radius
  float sea_ = 0, top_ = 0, start_ = 0;    // the sea's level, the highest the land may rise over it, where it may begin
  float mapSize_[2] = {1, 1};
  // what is read of the map when it is opened (Forget): whether one is, its seed, its highest ground over the sea, and
  // for 256 directions round its middle how much of its edge that way is land and how high that land stands
  static constexpr int kDirections = 256;
  bool read_ = false;
  uint32_t seed_ = 0;
  float highest_ = 0;
  std::array<float, kDirections> landward_{}, highness_{};
};

}  // namespace render
