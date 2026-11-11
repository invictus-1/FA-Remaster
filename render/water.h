// The water surface of a map: one sheet at the water's elevation, coloured the way the original game's
// high-quality water is, as far as this stage can (see water.cpp for what is still missing).
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "ocean.h"
#include "ripples.h"
#include "scmap.h"
#include "surf.h"
#include "textures.h"
#include "view.h"

namespace render {

class Water {
 public:
  Water(gfx::Device* device, TextureCache* textures) : ripples(device), surf(device), ocean(device), device_(device), textures_(textures) {}

  // Ripples on the modern look's water (ripples.h; World::waterRipples): a splash at (x, z), `radius` world units
  // across its middle (a shell's about 0.5), `strength` 1 for a usual shell, up to about 4 for a blast: rings run
  // out from it, cross others and die out in two or three seconds. Anyone may call it between World::Begin and
  // World::Render; it does nothing where there is no water or while the option is off. (World::AddFrame already
  // adds what a game's units and shots do in the water.)
  void AddRipple(float x, float z, float radius, float strength) { ripples.Add(x, z, radius, strength); }
  Ripples ripples;
  // The water's pipeline that reads the ripples' field, and the field's own, made now if they are not yet (World::Begin,
  // each frame the option is on): making them takes a quarter of a second, better at the first frame than in the
  // middle of a game at its first splash.
  void PrepareRipples();
  // The water's winter pipelines (winter_water.h; World::winterWater), made now if they are not yet (World::Begin, each
  // frame the option is on, so that the first snow brings no pause): `ripples` the one that also reads the ripples'
  // field. The water draws with them while the view's winterWater.x is above 0 (snow lies).
  void PrepareWinter(bool ripples);
  // [surf] Surf and sea spray (surf.h; World::surf): the shores' breaking waves are drawn by the water's surf pipelines
  // (made by PrepareSurf, from World::Begin each frame the option is on, before the first frame needs them: with the
  // ripples' field or without it, and the winter ones while winter water is on); the spray by `surf` itself.
  Surf surf;
  // [bigtest] With World::waterDetail: the bed under the shallows drawn soaked as the wet beach just above the water (dark at
  // the waterline, as before by some 0.8 units of depth: kSoakDepth; Chris's big test, 2026-11-01: past the dark wet band the
  // shallows came up bright again and the water looked lifted off the beach). False: the bed as before (viewer
  // --no-shore-soak).
  bool shoreSoak = true;
  // [soakfix] (Chris's second test, 2026-11-04: the soak "better, still weird at times", as he turns the camera "it looks like
  // it all shifts at once") With it: the soak and the ground's wet band by the map's own heights (its ground at its finest,
  // between the grid points), not by the ground as drawn. That is drawn at a level of detail by the camera's distance
  // (terrain.h, CDLOD), its points sliding onto a coarser grid as the camera moves off, by tenths of a unit: on a flat beach
  // where the soak and the band ended slid along the whole shore at every move of the camera. The ground's pass then soaks
  // the bed under the shallows itself (to its wet band's 0.6, gone by kSoakDepth of depth), the water's only darker again on
  // an exposed beach. The view's shore.x is 2. False: as before (viewer --soak-old).
  bool soakFixed = true;
  // [mirrorfine] (the scan of 2026-11-11, finding 13) With ray-traced reflections (World::rayReflections): the mirror image
  // of what stands far off across the water (an island, a shore) with soft edges. It was two rays a pixel, each meeting
  // the island or the sky, all or nothing, and seen low across the sea it broke into flat bars a pixel high and dozens
  // long. Now the share of a pixel's water whose rays meet the island, found by halving toward the lean from which they
  // pass over it (water.cpp, the modern shader's mirror). False: the two rays, as before (viewer --no-mirror-fine). Built
  // into the modern pipelines (MIRROR_FINE): a change makes them again (KeepVariants), once.
  bool mirrorFine = true;
  // [edgesea] Past the map's edge the modern water's surface dims over a band of its own (the view's beyondMore.x;
  // World::surroundEdgeBandWater), built into its pipelines (EDGE_SEA) as mirrorFine is. Set by World::Begin from
  // surroundEdgeBandWater > 0 (true, as its default is, so that the pipelines made at the load are not made again at
  // the first frame). False: over the land's band, the very shader of before.
  bool ownEdgeBand = true;
  void PrepareSurf(bool ripples, bool winter, bool wet = false);  // ([shore] wet: World::waterDetail's wet beach, SURF_WET)
  // [ocean] The wind sea worked out on the card (ocean.h; World::oceanWaves): the water draws with its ocean pipelines while
  // ocean.Active(), each variant (with the ripples' field or not, winter, the surf, the wet beach) made by PrepareOcean the
  // first time the options on now may want it (World::Begin, each frame the option is on).
  Ocean ocean;
  void PrepareOcean(bool ripples, bool winter, bool surf, bool wet, bool lift);

  // `map` must stay alive while the water is used. Maps without water load fine and draw nothing.
  bool Load(const MapData* map, std::string* error);
  // Call after the ground and everything else under the surface has been drawn.
  void Draw(const View& view);
  // The modern look's water (water.cpp): into a pass that has the scene's colour ONLY, with the
  // picture as it was before the water (`behind`, one sample a pixel) and the scene's depth to read.
  // Without several samples a pixel it draws as Draw does.
  void DrawModern(const View& view, const Camera& camera, gfx::Texture behind, gfx::Texture sceneDepth);
  // [floatfoam] The map's ground for the modern water (Terrain::HeightTexture and HeightUnit; before DrawModern): its foam
  // along the waterline is left out under what floats (AddAfloat) where what lies behind the water stands well off the
  // ground in water deep enough to float in (the floats of an amphibious unit afloat, a hand's breadth under the surface,
  // showed as flat white blocks). None: the foam as before.
  void SetGround(gfx::Texture heights, float heightUnit) {
    ground_ = heights;
    groundUnit_ = heightUnit;
  }
  // [fxmirror] The effects' mirror image for the modern water (fx_mirror.h; World::waterFxMirror sets it each frame, before
  // DrawModern): the picture of what is drawn after the water, drawn mirrored about its level (none: the water mirrors
  // none of it, as before). `fxMirrorReach`: how far off (world units) what it shows is taken to be, for how far the
  // waves' tilt moves and smears it; `fxMirrorStrength`: 1, as light mirrors (from the game's height that is a fiftieth:
  // next to nothing); more: looked down on, the image of what glows as if the water never mirrored less than that many
  // fiftieths (8: a tracer's image shows from the game's height; from a low eye nothing changes).
  gfx::Texture fxMirror;
  // (Chris, 2026-11-10, shown the game's pitch at 1 and at 8: "Stronger (8)". 8 is the default; from a low eye, and for
  // smoke, 8 draws what 1 draws - it is a floor, not a multiplier. 1: as light mirrors, viewer --fx-mirror-strength 1.)
  float fxMirrorReach = 4.0f, fxMirrorStrength = 8.0f;
  bool fxMirrorView = false;  // (for checking: the mirrored picture itself in place of the water; viewer --fx-mirror-view)
  // [floatfoam] What floats, for that: the units (World::AddFrame, each unit seen: its place, its turn and its model's bounds,
  // scaled), those in the water over water deep enough to float in kept; listed anew for each view (World::Begin:
  // ClearAfloat), the nearest 64 to the eye used. The foam is left out only under them, so that a rock keeps its band.
  void ClearAfloat() { afloat_.clear(); }
  void AddAfloat(Vec3 position, const Quat& orientation, Vec3 lo, Vec3 hi);

 private:
  gfx::Device* device_;
  TextureCache* textures_;
  bool patchesTried_ = false;  // [foampatch] (the foam patches looked for, once)
  const MapData* map_ = nullptr;
  void DrawWith(const View& view, const Camera* camera, gfx::Texture behind, gfx::Texture sceneDepth);
  gfx::Pipeline ModernPipeline(bool ripples, std::string* error, bool winter = false, bool surf = false, bool wet = false, bool ocean = false,
                               bool lift = false);
  gfx::Pipeline pipeline_, modern_;
  gfx::Pipeline modernRipples_;  // (the modern water reading the ripples' field: made the first time it is wanted)
  bool ripplesTried_ = false;
  bool mirrorFineBuilt_ = true;  // [mirrorfine] (the mirror's way the modern pipelines were made with)
  bool ownEdgeBandBuilt_ = true;  // [edgesea] (and the edge's band)
  void KeepVariants();
  gfx::Pipeline PlainPipeline(std::string* error);  // [edgesea] (the classic look's water)
  gfx::Pipeline modernWinter_[2];  // (winter water: without and with the ripples' field; made the first time wanted)
  bool winterTried_[2] = {false, false};
  gfx::Pipeline modernSurf_[8];  // [surf] (by 1 the ripples' field, 2 winter, [shore] 4 the wet beach; each made the first time wanted)
  bool surfTried_[8] = {false, false, false, false, false, false, false, false};
  // [ocean] (by 1 the ripples' field, 2 winter, 4 the surf, 8 the wet beach, [lift] 16 lifted; each made the first time wanted)
  gfx::Pipeline oceanPipes_[32];
  bool oceanTried_[32] = {};
  gfx::Pipeline OceanPipeline(int key);
  gfx::Buffer vertices_;
  gfx::Texture waterMap_, waves_[4], sky_;
  gfx::Texture ground_;  // [floatfoam] (SetGround)
  float groundUnit_ = 0;
  struct Afloat {  // [floatfoam] (AddAfloat: its box's middle, its way forward, its half length and half beam; how far from the eye)
    float x, z, forwardX, forwardZ, halfLength, halfBeam, away;
  };
  std::vector<Afloat> afloat_;
};

}  // namespace render
