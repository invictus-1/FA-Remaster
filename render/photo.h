// (photomode) Photos larger than the window, made over frames (WorldPicture::StartPhoto), and the focus by a pixel.
//
// A photo is drawn as one picture of its own size where that fits the card's memory (the device's photo window,
// gfx.h SetPhotoWindow: the swap chain keeps the window's size, so the window goes on being shown meanwhile). Larger
// than that it is drawn in tiles: each a picture seen from the photo's own eye, turned toward its part of the photo
// (yawed and pitched from the photo's way of looking), as large as the memory allows, with a guard band round it that
// is never used and a band where neighbours are cross-faded. Since every tile has the same eye, a line of sight of the
// photo meets the same things in every tile that sees it: the photo is put together exactly, each of its pixels read
// in the tiles at the point its line of sight passes through (a sharp cubic filter), with no seams of parallax.
//
// What reads the pixels round a pixel sees only its own tile: the glow (bloom) spreads over a share of the tile, not of
// the photo, so a tiled photo's glow is tighter round the lights; the lens's blur and the shadows in creases (ambient
// occlusion, contact shadows) miss what lies past a tile's edge, which is why each tile has its guard band. The light
// in the air and the volumetric clouds are worked out along each pixel's own line of sight and agree. What the world
// places round the camera (the sun's shadows' cascades, the lamps' shadows, the grass, the rain's box, the bounce
// light's probes) is placed for the photo's own camera in every tile (World::photoTiled), so those agree too.
#pragma once
#include <cstdint>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"

namespace render {

struct PhotoTile {
  Camera camera;         // the photo's eye, turned toward this part of the photo
  int x0, y0, x1, y1;    // the part it gives, in the drawn photo's pixels (the bands round it cross-faded)
  float focalShare = 1;  // its focal length over the photo's (its lens's aperture is made the photo's: f-number times this)
  // where a point of the drawn photo is in this tile's picture: (a, b, c) = rows times (x, y, 1); x a / c, y b / c
  float toTile[3][3] = {};
};

struct PhotoPlan {
  int width = 0, height = 0;            // the photo as it comes out
  int by = 1;                           // drawn this many times larger again: each pixel the mean of by x by drawn
  int drawnWidth = 0, drawnHeight = 0;  // width * by, height * by
  int tileWidth = 0, tileHeight = 0;    // each picture drawn (one picture: the drawn photo itself)
  int columns = 1, rows = 1;
  int blend = 0;                        // drawn pixels either side of a tile's edge cross-faded with its neighbour
  std::vector<PhotoTile> tiles;         // by rows from the top; one: the photo's own camera, nothing to put together
  bool Tiled() const { return tiles.size() > 1; }
};

// The plan for a photo of `width` x `height` (made at most 16384 a side, in proportion) as `camera` sees it, drawn
// `supersample` times larger again (0: 2 where one picture that size fits the memory, else 1; asked for: 1 .. 4).
// `budget`: the card's memory one drawn picture may take, `perPixel` its bytes a pixel. `guard`: drawn pixels round
// each tile that are drawn but never used.
PhotoPlan PlanPhoto(const Camera& camera, int width, int height, int supersample, double budget, double perPixel, int guard);

// The photo put together from its tiles (RGBA bytes, each tileWidth x tileHeight, in the plan's order) as 0xAARRGGBB,
// rows from the top; `threads` at once.
void AssemblePhoto(const PhotoPlan& plan, const std::vector<std::vector<uint8_t>>& tiles, std::vector<uint32_t>* out, int threads);
// One picture drawn `by` times larger made smaller: each pixel the mean of the by x by drawn (RGBA bytes in).
void ShrinkPhoto(const std::vector<uint8_t>& drawn, int drawnWidth, int by, int width, int height, std::vector<uint32_t>* out);

// A camera at `eye` whose axes are these (unit length, at right angles, up = back x right as Camera::Axes has them):
// its heading, pitch and roll found for them; its target `distance` ahead.
Camera CameraWithAxes(Vec3 eye, Vec3 right, Vec3 up, Vec3 back, float distance);

// What is under a pixel: the scene's depth read there on the card (the water's surface where the line of sight meets it
// first, as the lens sees it), as the distance along the line of sight, as the lens measures it; 0 for the sky. A
// frame of its own between two of the game's, drawn into a small photo window and read back (some milliseconds).
class PhotoFocus {
 public:
  explicit PhotoFocus(gfx::Device* device) : device_(device) {}
  ~PhotoFocus();
  // `depth`: the scene's depth (`samples` a pixel, `depthWidth` x `depthHeight`) as `camera` drew it; `x`, `y` the
  // pixel in it. `lensMeasure`: the lens's own auto-focus at the middle (the ninth nearest of 25 points round it, dof.cpp)
  // in place of the pixel. `waterLevel`: the water's height (below -1e8: none).
  float Read(gfx::Texture depth, int depthWidth, int depthHeight, float x, float y, bool lensMeasure, const Camera& camera, float waterLevel);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  int state_ = 0;  // 0 not made yet, 1 made, -1 it cannot be
};

}  // namespace render
