// [shadowmask] The sun's shadow on the ground worked out once a visible pixel (World::shadowMask, viewer --shadow-mask; off
// by default).
//
// Without it the ground's shader works out the sun's shadow for itself (lighting.h, SunShadow): with soft shadows a search
// of 16 taps for what casts it and 16 more for its soft edge, on every pixel of the ground the scene draws, and the ambient
// occlusion's contact shadows once more for every pixel to know where the sun reaches. With it, before the scene, the
// ground's depth alone is drawn into a target of its own (one sample a pixel: Terrain::Draw's depth pipeline, the ground
// beyond the map's edge too, about 0.05 ms at 4K), and one compute pass works out from it, for each pixel, the shadow maps'
// share of the sun on the ground there (its place from the depth, the way the ground's shader looks the shadow up along
// from the map's normal map: the same SunShadowMaps, soft or not). The ground's shader reads that instead (lighting.h,
// SunShadowSeen), and so do the contact shadows (ambient_occlusion.cpp, where the pixel shows the ground); the models, the
// ground cover, the marks on the ground, the water and the rest keep their own SunShadow, and the scene's own depth, which
// the marks, the range rings, the ground cover and the ambient occlusion read while it fills, is not touched.
//
// Where the mask does not serve it says so (below 0) and the ground works its shadow out as before: where there is no
// ground, at the picture's edge, and where the ground's depth is not smooth round the pixel (a crest in front of farther
// ground, a crease): there the scene's samples of a pixel may lie on other ground than its middle.
//
// The mask is kept in a buffer of halves, two pixels to a word, row after row (the ground's shaders have every texture
// register taken: they read it at the shader buffers' slot 0, which they do not use otherwise).
#pragma once
#include <string>
#include <utility>

#include "gfx.h"
#include "view.h"

namespace render {

class SunMask {
 public:
  explicit SunMask(gfx::Device* device) : device_(device) {}

  // Its pictures at this size (made or remade as needed); false where it cannot be had (the device has no card buffers,
  // its shader could not be made, or the picture is larger than kMostPixels: a photo's).
  bool Size(int width, int height);
  // The pass the ground's depth is drawn in (the scene's view; depth alone, one sample a pixel, cleared).
  gfx::PassDesc DepthPass() const;
  // After that pass: the mask worked out, by `view` (whose shadow maps must be bound at kShadowMapSlot, and whose matrix is
  // the depth pass's). Sets view->constants.shadowMask so that the ground reads it.
  void Compute(View* view);
  gfx::Texture Depth() const { return depth_; }
  gfx::Buffer Rows() const { return rows_; }
  int Words() const { return words_; }  // (a row's words in Rows)
  int Height() const { return height_; }
  // How smooth the depth must be round a pixel for the mask to serve there: the most its depth may differ from the mean of
  // its two neighbours on either axis, as a share of its depth (depth is zNear / distance: on a flat surface it changes
  // evenly across the picture, so this is 0 there whatever its slope).
  float smoothness = 0.002f;
  // Cheaper soft shadows (viewer --shadow-mask-share): the search for what casts a shadow shared by each 2 x 2 pixels, each
  // looking with four of its sixteen taps (sun_mask.cpp); the soft edge's taps each pixel's own. The same look but where a
  // shadow's soft edge is (another draw of its noise).
  bool shareSearch = false;
  // The ground, set by the world before Compute: its normal map (Terrain::NormalMap) and the map's size.
  gfx::Texture groundNormals;
  float groundSize[2] = {1, 1};
  // The largest picture it is made for (5120 x 2880): a photo's larger frames work their shadows out as before.
  static constexpr long long kMostPixels = 5120LL * 2880LL;
  // (checks) Where it leaves the shadow to the ground, the ground takes the sun half (viewer --shadow-mask check; with
  // --inspect shadow, the sun's shadow alone, those places show mid grey).
  bool check = false;

  // [views] What it keeps for one view (its pictures at that view's size) swapped with another's (views.h, ViewSide).
  void SwapView(SunMask& o) {
    std::swap(depth_, o.depth_), std::swap(rows_, o.rows_);
    std::swap(width_, o.width_), std::swap(height_, o.height_), std::swap(words_, o.words_);
  }
  // [views] A view's own let go of (a class without a destructor of its own: the world's go with the device).
  void ReleaseView() {
    if (depth_) device_->Destroy(depth_);
    if (rows_) device_->Destroy(rows_);
    depth_ = {};
    rows_ = {};
    width_ = height_ = words_ = 0;
  }

 private:
  struct Constants {
    float fromScreen[4][4];  // (x, y, depth, 1) of the picture's -1..1 square -> the world's place (homogeneous)
    float size[4];           // width, height, the buffer's words a row, smoothness
    float ground[4];         // the map's width and height
    float more[4];           // shareSearch
  };
  bool Pipeline();
  gfx::Device* device_;
  gfx::Pipeline compute_;
  int made_ = 0;  // 0 not tried, 1 made, -1 cannot be
  gfx::Texture depth_;
  gfx::Buffer rows_;
  int width_ = 0, height_ = 0, words_ = 0;
};

}  // namespace render
