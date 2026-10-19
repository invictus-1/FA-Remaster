// Sunlight through gaps (the visuals session's proposals 1 and 2, Chris 2026-10-18): light shafts and drifting cloud
// shadows. Modern look, a time of day; each off by default (World::lightShafts, World::driftingShadows; in the game's
// "Atmosphere" setting).
//
// Drifting cloud shadows (World::driftingShadows): the clouds' field broken into heaps of many sizes (view.h,
// CloudFieldBroken), from wide patches of weather down to small fair-weather clouds a few dozen units across, so that from
// every height of the camera some shadows lie in view and drift through it with the wind, changing slowly as they go;
// soft at their edges; over the sea too (the water darkens in them, view's cloudDrift.w). The sky's own cloud layer and
// the volumetric clouds are cut from the same field (sky.cpp, CloudStepsBroken; clouds.cpp, Coverage), so the shadows
// stay under the clouds that cast them. Worked out once a frame into a picture round what the camera sees (Drift), with
// the volumetric clouds' own shadows folded in, which every surface and the light in the air then read in the clouds'
// own shadows' place (one reading where the flat layer's field was three noises at every pixel).
//
// Light shafts (World::lightShafts; needs the light in the air, World::volumetricFog):
//  - Through the canopy: the low band of air over the ground (a few units, where the trees stand) holds the morning's
//    haze and the mist; its sunlight is worked out here finely, along each pixel's line of sight through the sun's shadow
//    maps, at half the picture's width and height, every other place (as a chessboard's black squares; the eight marched
//    places of each four by four start their steps at eight places along a step, and are averaged again with their
//    neighbours at the same distance), and laid over the scene. Gaps in a forest's crowns then let bright shafts down
//    through the shadowed haze, and the trunks and crowns throw dark ones through the lit haze. The light in the air
//    (air.h) leaves the band's sunlight near the eye to this pass (its coarse cells would blur the shafts away); the haze
//    and its other light stay its own. The haze in the band follows the mist, the wet and the fog: strongest on a misty
//    morning and after rain.
//  - Below breaks in the clouds: low haze under the clouds' layer (thickest where the clouds are broken and the sun is
//    low), whose sunlight the same pass works out along each line of sight through the clouds' shadow (the volumetric
//    clouds' own picture of it, or one of the flat layer's made once a frame, 512 texels square round the eye): soft
//    bands of light under the gaps over the land and the sea, brightest toward the sun. Looking toward the horizon (a low
//    camera) they fan out from the sun as rays; from the game's usual height, looking down, a line of sight crosses many
//    gaps and they read as a soft warm glow with broad faint bands.
// Everything follows from the map, the clock and the weather: the same for every player and replay.
#pragma once
#include <string>
#include <vector>

#include "camera.h"
#include "gfx.h"
#include "linalg.h"
#include "view.h"

namespace render {

// The shaders' helpers for the low band (after the view's block): the ground's height under a place (the map's heights,
// mirrored past its edge as the world beyond lays them; the water's level where that is higher), how much of a place's
// sunlight in the air is the shafts' pass's, and the hazes. The heights are read from SHAFT_HEIGHTS (a register the
// shader says before this block).
extern const char kShaftBandShaderBlock[];

class Sunlight {
 public:
  explicit Sunlight(gfx::Device* device) : device_(device) {}
  ~Sunlight();

  // The look, changeable between frames (the viewer's --sunlight-look name=value,...: Set).
  struct Look {
    // drifting cloud shadows
    float shadow = 0.8f;      // how much of the sun's light a cloud's shadow takes
    float edge = 0.03f;       // how soft a shadow's edge is (in the clouds' field's values either side of its outline)
    float sea = 0.3f;         // how much darker the sea goes in a cloud's shadow (its mirror of the sky as well)
    float drift = 1.0f;       // how fast the clouds drift, against the sky's own (5.5 world units a second)
    // light shafts
    float top = 6;            // the low band's top over the ground (world units; the trees are 2.5 to 4 high)
    float open = 0.3f;        // of the band's haze, the share over open ground (the woods hold all of it: damp, still air)
    float forward = 0.35f;    // how much of the band's light goes on forward (0: alike to all sides; the game's camera sees
                              // the low sun from the side, so less forward than the light in the air's 0.6)
    float haze = 0.003f;      // the haze at the ground in the band (share of light lost per world unit), clear and dry
    float misty = 0.01f;      // more of it with full mist, wet ground or fog
    float under = 0.0015f;    // the low haze under broken clouds at the ground, at its most (broken clouds, a low sun)
    float low = 40;           // the height it thins to 1/e over (world units)
    float lowForward = 0.6f;  // how much of its light goes on forward
    float lowReach = 3000;    // how far from the eye the shafts' pass works out its sunlight through the clouds' shadow
                              // (0: the light in the air does, a soft glow without bands)
    float reach = 2.5f;       // how far from the eye the band's pass reaches, in the game camera's distances
    float farthest = 600;     // and at most (world units); a cinematic camera's: this
    float fadeFrom = 350, fadeTo = 550;  // the pass fades out as the game's camera rises (its distance), the light in the air taking all again
    int steps = 24;           // steps along each line of sight through the band, at most
    float spacing = 2;        // and a step every so many world units (a steep line of sight through the band needs few);
                              // (airfix) 2, not 1: the eight starts of each four by four still put a place every quarter
                              // unit along the way once averaged, the picture the same but for a grain of 0.8 in 255, and
                              // the sun's shadow maps are read half as often (the close view in the woods: 0.02 ms less)
    float strength = 0.8f;    // what the pass's light is multiplied by (1: as the light in the air reckons it; a little
                              // less, so that a misty morning's woods are not washed out)
  };
  Look look;
  // Sets those named in a list "top=6,haze=0.0015,..." (shadow, edge, sea, drift, top, open, forward, haze, misty, under,
  // low, lowforward, lowreach, reach, farthest, fadefrom, fadeto, steps, spacing, strength); false and why.
  bool Set(const std::string& list, std::string* why);

  // When a map is opened: where its trees stand (x, y, z, and the top's height over its foot: World's list for the
  // wildlife), for the woods' share of the band's haze (a picture over the map, four units a texel).
  void SetWoods(const std::vector<Vec4>& trees, float mapWidth, float mapHeight);
  gfx::Texture Woods() const { return woods_; }

  // For a frame (World::Begin, after the sky's constants are set): the view's cloudDrift, shaftBand and shaftGround.
  // `drifting` / `shafts`: the options this frame (the modern look, a time of day); `placing`: the camera the frame is
  // placed for (World::Placing); `mist`, `wet`, `fog`: the weather now (0..1); the map's size, its heights' unit and its
  // water's level (-1e9: none). Off, they stay 0 and nothing changes.
  void Begin(View* view, bool drifting, bool shafts, const Camera& placing, float mist, float wet, float fog, float mapWidth,
             float mapHeight, float heightUnit, float waterLevel);
  bool ShaftsNow() const { return shaftsNow_; }
  // The drifting cloud shadows' picture (after the clouds' Prepare, before anything reads the clouds' shadows this frame,
  // between passes: it begins one of its own): the heaps' soft shadows round what the camera sees, at the clouds'
  // underside (1024 texels across, 1.5 world units a texel from the game's height), with the volumetric clouds' own
  // shadows (`cloudsOwn`, clouds.h; none without them) folded in; the view's driftPicture set. Then bind DriftPicture
  // at kCloudShadowSlot in their place for everything that reads the clouds' shadows (view.h, CloudShadow: one reading
  // where four noises were). False: none this frame (the option off, no clouds), the clouds' own stay.
  bool Drift(View* view, gfx::Texture cloudsOwn);
  gfx::Texture DriftPicture() const { return driftDrawn_ ? drift_ : gfx::Texture{}; }
  // After the solid scene and the water (between passes: it begins passes of its own): the band's sunlight along each
  // line of sight. `airDust`, `airFloor`, `airThinsOver`, `airSun`, `airForward`: the light in the air's numbers (air.h:
  // dust, the floor its haze thins above, thinsOver, sunInAir, sunForward), so that the band's light is reckoned as the
  // volume would. The textures it reads: the scene's depth, the ground's heights, the sun's shadow maps, the volume of
  // the light in the air, the volumetric clouds' shadows and the smoke's shadow (either may be none).
  void March(const View& view, const Camera& camera, gfx::Texture sceneDepth, gfx::Texture heights, gfx::Texture sunShadows,
             gfx::Texture airVolume, gfx::Texture cloudShadows, gfx::Texture smokeShadows, float airDust, float airFloor,
             float airThinsOver, float airSun, float airForward);
  // Into the scene's colour (Post::BeginColorOnly), after March: the band's light, brought up to the picture's size.
  void Lay(const View& view, gfx::Texture sceneDepth);

 private:
  bool Make();
  bool Size(int width, int height);
  gfx::Device* device_;
  bool tried_ = false, made_ = false, shaftsNow_ = false, marched_ = false;
  bool driftNow_ = false, driftTried_ = false, driftDrawn_ = false;
  Camera placing_;                 // the camera the frame is placed for (Begin)
  gfx::Pipeline driftPipe_;
  gfx::Texture drift_;             // the drifting cloud shadows' picture (R16F, Drift)
  gfx::Pipeline march_, smooth_, lay_, clouds_pipe_;
  gfx::Texture rough_, smoothed_;  // the band's light at half the size: as marched, and averaged with its neighbours
  gfx::Texture woods_;             // how much of a place is woods (0..1), over the map
  gfx::Texture clouds_;            // the clouds' shadow round the eye this frame, at their underside (R16F, March)
  int width_ = 0, height_ = 0;     // (the picture's size they were made for)
  struct Constants {
    float right[4];   // xyz the camera's right x tan(half the view's width); w its near distance
    float up[4];      // xyz its up x tan(half the view's height); w the steps
    float ahead[4];   // xyz its forward; w what the light is multiplied by
    float air[4];     // x the air's dust at its floor, y that floor's height, z 1 / the height it thins over, w the sun's light in the air
    float more[4];    // x the band's forward scattering; y, z the half-size picture's width and height; w the steps' spacing
    float cloud[4];   // the clouds' shadow picture: x, y its middle; z 1 / its width; w 1 = drawn this frame
  } k_ = {};
};

}  // namespace render
