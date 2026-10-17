// The order lines as the original draws them while Shift is held (its command graph; ui request 30, the
// interface track's helper read every number from the original): the interface works out the legs between the
// orders' places, which units each carries, their states and colours, and hands them over each frame; here
// each leg is drawn as the original draws it:
//  - a flat ribbon on a cubic Hermite curve between its two places (tangents given, scaled by
//    c = min(0.25 |b - a|, curveSmoothness x width), curveSegments + 1 points), its sides at
//    (T.z, 0, -T.x) x width / 2 from the curve (T the curve's direction, not made unit length again), so it
//    lies flat over the ground's slope between its ends without following the ground;
//  - as wide as the units on it say (baseWidth: min(sqrt(units), 10), the interface's) and two pixels' worth more
//    at its nearer end: (baseWidth + 2 max(w(a), w(b))) x lineScale, w(p) the world units a screen pixel spans at p;
//  - its texture across the ribbon (V 0 to 1), along it every width x uvAspect (U), moving with the game's ticks at
//    animRate (patrols' chevrons: 12.5 widths a second);
//  - the texture times the leg's colour, blended over the picture, nothing where the texture's alpha is 0, never
//    hidden by hills or units (no depth test), and where two legs cross the one drawn first keeps the pixel (the
//    original's stencil: here each leg a little nearer in depth than the one after it).
// Then the waypoints' icons: a flat square on the ground at each node, u along +x, v along +z, half its side
// clamp(size / w, markMinPixels, markMaxPixels) pixels' worth, its texture times its colour (none: a solid square),
// over the lines, no depth test. No arrowheads (the original's are only on a path preview it never shows).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "textures.h"
#include "view.h"

namespace render {

struct OrderLeg {
  Vec3 a, b;           // its two places (world units, on the ground)
  Vec3 ta, tb;         // the curve's directions there (unit length; zero: a straight leg)
  float baseWidth = 1; // world units: min(sqrt(units on it), 10)
  uint32_t color = 0xffffffff;  // 0xAARRGGBB, its state's (normal, selected, highlighted)
  int texture = -1;    // into OrderLines::textures (none: drawn plain)
  float uvAspect = 1;  // the texture's U along the ribbon: uvAspect for each width of length
  float animRate = 0;  // the texture's U moving by this many each game tick
};

struct OrderMark {
  Vec3 position;       // the node's place on the ground
  int texture = -1;    // into OrderLines::textures (none: a solid square)
  uint32_t color = 0xffffffff;  // 0xAARRGGBB
  float size = 1;      // world units: the node's size times the order's waypoint scale
};

class OrderLines {
 public:
  OrderLines(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  ~OrderLines();
  bool Init(std::string* error);

  // Set by the host (WorldPicture::SetOrderLines), each stays as set until set again (empty: none shown).
  std::vector<std::string> textures;  // the textures' paths ("/textures/ui/common/game/orderline/orderline_generic.dds")
  std::vector<OrderLeg> legs;         // drawn in this order (the interface's: highlighted, selected, then the rest)
  std::vector<OrderMark> marks;
  // The original's console variables (ui_WaypointLineScale, ui_CurveSmoothness, ui_CurveSegments,
  // ui_MinWaypointSize, ui_MaxWaypointSize), as the game's own Lua sets them.
  float lineScale = 1, curveSmoothness = 35;
  int curveSegments = 32;
  float markMinPixels = 7, markMaxPixels = 100;

  // Into the window's pass, after the finished picture (it writes the window's depth, which nothing drawn after it
  // reads: the icons and the interface draw without it). `view`: the
  // window's own (no jitter: World::screenView_); `ticks`: the game's time in ticks (ten a second), fractions in.
  void Draw(const View& view, double ticks);
  int drawn = 0;  // (the last Draw: legs drawn)

 private:
  struct Vertex {
    float position[3];
    float uv[2];
    uint32_t color;  // RGBA8 as bytes
    float depth;     // where it lies in the window's depth (nearer for the legs drawn first)
  };
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline legPipeline_, markPipeline_;
  gfx::Buffer legBuffer_, markBuffer_;
  std::vector<Vertex> legVertices_, markVertices_;
  std::vector<gfx::Texture> loaded_;  // (textures, as loaded)
  std::vector<std::string> loadedPaths_;
};

}  // namespace render
