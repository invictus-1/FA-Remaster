// Draws models: every frame the caller lists what is in the world (a model, where, in which colour);
// each pass then draws what of that list it can see, sorted by mesh, each mesh's instances together.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "models.h"
#include "textures.h"
#include "view.h"

namespace render {

class MeshRenderer {
 public:
  MeshRenderer(gfx::Device* device, TextureCache* textures) : device_(device), textures_(textures) {}
  bool Init(std::string* error);

  // Empties the list.
  void Begin();
  // `world` places the model's own units in the world (so it includes the model's scale).
  // `teamColor` is rgb in 0..1; `built` is the fraction complete (1 for a finished unit).
  void Add(const Model* model, const Mat4& world, Vec3 teamColor, float built = 1);
  // Draws the list as `view` sees it: into the scene, or (`shadowPass`) into a shadow map, where
  // only depth is written. `environment` is the cube map the original look reflects in shiny parts.
  void Draw(const View& view, gfx::Texture environment, bool shadowPass = false);

  // Level of detail is chosen by size on screen, not by distance: a model wishes for
  // `trianglesPerPixel` triangles for every pixel of the circle it covers and gets the level nearest
  // to that wish. So every model costs about the same per pixel, whatever its budgets and however
  // many levels it has. With levels a quarter of each other, the finest is used from about 180
  // pixels across.
  float trianglesPerPixel = 0.55f;
  // The quality setting: scales that wish. 1 is full quality; 0.5 suits a slower card; a very large
  // value forces every model's finest level.
  float detail = 1;
  float minPixels = 1.5f;         // smaller than this on screen: not drawn (a strategic icon's job)
  float shadowMinPixels = 12;     // smaller than this: casts no shadow
  float shadowDetail = 0.25f;     // shadows are drawn from coarser levels: this share of the triangles
  // Numbers from the last scene (not shadow) pass.
  int drawnInstances = 0;
  int drawnTriangles = 0;
  int shadowTriangles = 0;        // summed over the shadow passes since the last scene pass

 private:
  struct Instance {
    float row0[4], row1[4], row2[4];  // the top three rows of the world matrix
    float color[4];                   // team colour, w: fraction complete
  };
  struct Entry {
    const Model* model;
    Vec3 centre;   // of its bounding sphere, in the world
    float radius;
    Instance instance;
  };
  struct Item {
    const ModelLod* lod;
    const Instance* instance;
  };
  gfx::Device* device_;
  TextureCache* textures_;
  gfx::Pipeline pipeline_, blended_, shadow_;
  bool countedScene_ = true;
  std::vector<Entry> entries_;
  std::vector<Item> items_;
  std::vector<Instance> batch_;
};

}  // namespace render
