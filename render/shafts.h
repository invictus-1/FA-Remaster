// A beam of light seen in the air: the shaft a lamp's light makes where the air is not clear
// (dust by night, more in rain, snow and fog). Each is one flat shape turned toward the camera,
// narrow at the lamp and as wide as the beam at its far end, brightest along its middle and at the
// lamp, adding its light to the picture. It lights nothing: the light on the ground is the lamp's
// (lights.h, AddBeam); this is only what the eye sees of the beam on its way. Where a shaft runs
// into the ground or a unit it fades out over a short way instead of ending in a hard line.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

class Shafts {
 public:
  explicit Shafts(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  void Begin() { shafts_.clear(); }
  // From the lamp to where the beam ends; half its width at each end; its light (linear).
  void Add(Vec3 from, Vec3 to, float halfWidthFrom, float halfWidthTo, Vec3 color);
  // A piece of a lightning bolt: as bright at one end as at the other.
  void AddBolt(Vec3 from, Vec3 to, float halfWidth, Vec3 color);
  // Into the scene's colour alone, after everything solid and the water: the shafts test the
  // scene's depth themselves (`sceneDepth`; `zNear` is the camera's) to fade where they meet it.
  void Draw(const View& view, gfx::Texture sceneDepth, float zNear);
  float soften = 0.5f;  // over how many world units (along the line of sight) a shaft fades where it meets something solid
  int Count() const { return static_cast<int>(shafts_.size()); }

 private:
  struct Shaft {
    float from[4], to[4], color[4];
  };
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
  std::vector<Shaft> shafts_;
};

}  // namespace render
