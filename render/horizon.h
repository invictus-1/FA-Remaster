// The weather on the horizon (World::horizonWeather; Chris, 2026-10-18 evening, "beyond the map's edge, part 2"):
// the sea beyond the map's edge (World::worldBeyond) is no longer flat and empty into the haze. Modern look, a time of
// day (the computed sky), off by default. Three parts, each by the weather of the moment:
//  - Winter ice, only while snow lies (World::snowLying): floes drifting on the sea past the edge, few and small near
//    it, more and larger farther out until they close into pack ice toward the horizon; flat white-grey plates on the
//    water with soft edges, lit as the ground is (lighting.h, ModernLight: the snow that lies on them too), rocking a
//    little as the swell runs under them, drifting slowly with the wind. A few small icebergs far out (solid, drawn
//    with the scene before the water, so the water foams round them and shows their sunken part). Never inside the
//    playable map: the floes begin a few units out, the icebergs well out.
//  - Fog banks on the far sea: low banks lying on the water beyond the edge, drifting, thicker toward the horizon,
//    at dawn, in damp and in cold weather; lit as the light in the air is (air.cpp: the sun's light sent on mostly
//    forward, so the sun behind a bank glows through it; the sky's light round about), seen through the air.
//  - Showers on the horizon: rain curtains or, in snow, snow squalls passing far out, grey veils from the cloud base
//    down to the sea, leaning and moving with the wind, more and darker in stormy weather.
// And the clouds' shadows: where the far sea goes over into the haze (lighting.h, BeyondFar) its own shading is lost,
// and with it the clouds' shadows on it; they are carried on over the far sea, fading with the distance.
//
// Where it is drawn (World::Render): the icebergs in the scene's pass after the units and scenery (DrawSolid); the
// floes and the shadows after the water, tested against the scene's depth (DrawOnWater); the fog banks and showers
// after them, into the scene's colour alone, testing the depth themselves (DrawInAir). Nothing of it with the game's
// camera hides the playable map: what would lie before it there is left out (for a cinematic camera, cinecam.h, it
// is the world as it is). Everything follows from the map, the weather's seed and the clock: the same for every
// player and every replay.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

#include "camera.h"
#include "gfx.h"
#include "scmap.h"
#include "view.h"

namespace render {

class Horizon {
 public:
  // [whitecaps] The ocean whose lifted waves the floes ride (ocean.h; World sets it each frame; none: on the still water).
  const class Ocean* ride = nullptr;
  explicit Horizon(gfx::Device* device) : device_(device) {}

  // What the weather is this frame, as the world has it (World::Begin).
  struct Weather {
    float hour = 12;           // the time of day
    float snowLying = 0;       // how much snow lies (0..1): the ice comes with it
    float rain = 0, snow = 0;  // what falls (0..1)
    float fog = 0, wet = 0;    // how thick the air is, how wet the ground (0..1)
    float mist = 0;            // the mist on low ground this frame (World::mistNow)
    float cloudCover = 0.3f;
    bool cold = false;         // a cold climate
    uint32_t seed = 1;         // the weather's seed (World::weatherSeed)
  };
  // For a frame: what is drawn and where, from the map, how far out the world beyond goes (world units), the weather,
  // the view's clock (seconds, as the clouds drift by) and the camera. False: nothing of it this frame.
  bool Begin(const MapData& map, float reach, const Weather& weather, float time, const Camera& camera, int width, int height);

  // In the scene's pass (its depth bound), after the units and scenery and before the water: the icebergs.
  void DrawSolid(const View& view);
  // In the scene's pass (its depth bound, tested, not written), after the water: the floes, and the clouds' shadows
  // carried over the far sea.
  void DrawOnWater(const View& view);
  // Into the scene's colour alone (Post::BeginColorOnly), after DrawOnWater: the fog banks and the showers, which read
  // the scene's depth themselves (`sceneDepth`); `heights`: the ground's heights (Terrain::HeightTexture, in units of
  // `heightUnit`), for where the sea is.
  void DrawInAir(const View& view, gfx::Texture sceneDepth, gfx::Texture heights, float heightUnit);

  // Which parts are drawn (the viewer's --horizon-parts: to look at or time one alone).
  bool ice = true, fog = true, showers = true, shadows = true;
  // The look (the viewer's --horizon-look name=value,...: Set).
  float floeSize = 16;        // the large floes' cells (world units across)
  float iceDrift = 0.08f;     // how fast the ice drifts with the wind (world units a second)
  float fogHeight = 14;       // how high a bank's middle stands over the water (world units)
  float fogThickness = 1;     // what the banks' density is multiplied by
  float showerSpeed = 1.2f;   // how fast the showers pass (world units a second)
  float shadowCarry = 0.4f;   // how much of the clouds' shadow is carried over the far sea where its own shading is lost
  // Sets those named in a list "floe=11,fog=1,..." (floe, drift, fogheight, fog, showerspeed, shadow); false and why.
  bool Set(const std::string& list, std::string* why);

  // Of the frame last begun, for the viewer to print: how much ice, fog and how many showers and icebergs.
  float iceNow = 0, fogNow = 0;
  int showersNow = 0, bergsNow = 0;

 private:
  bool Make();
  gfx::Device* device_;
  bool tried_ = false, made_ = false;
  gfx::Pipeline bergPipeline_, floePipeline_, bankPipeline_, showerPipeline_;
  gfx::Pipeline floeRide_;  // [whitecaps] (the floes riding the lifted waves: made the first time)
  bool floeRideTried_ = false;
  std::string floeRideSource_;
  gfx::Texture noise_;  // a tiling noise (two channels) the banks, the floes' outlines and the showers' streaks read
  static constexpr int kMostShowers = 16, kMostBergs = 16;
  struct Constants {
    float area[4];    // x, y the map's width and height; z how far out the world beyond goes; w the water's level
    float ice[4];     // x how much ice; y where the floes begin, z where the pack ice closes (world units out); w the floes' size
    float drift[4];   // xy how far the ice has drifted (world units); zw the fog banks
    float fog[4];     // x how thick the banks are (0: none); y their height; z where they begin (world units out); w their slow change
    float wind[4];    // xy the wind's way (x, z); z 1: keep off the playable map (the game's camera); w the clock (seconds)
    float right[4];   // the camera's right x tan(half the view's width)
    float up[4];      // its up x tan(half the view's height)
    float ahead[4];   // xyz its forward; w its near distance
    float ground[4];  // x the ground's height unit; y the clouds' shadows carried over the far sea (0..1); zw unused
  } k_ = {};
  struct Shower {
    float at[4];    // x, z where it meets the sea; its radius; how strong (0..1)
    float look[4];  // its top (world y); how far it leans per unit of height; 1 = snow; a number of its own
  } showers_[kMostShowers] = {};
  struct Berg {
    float at[4];     // x, its waterline's height now, z; its radius
    float shape[4];  // its height over the water; how far its top rises in a peak (share of its height); its turn; a number of its own
  } bergs_[kMostBergs] = {};
  int showerCount_ = 0, bergCount_ = 0;
  bool onWater_ = false, inAir_ = false;
  // the icebergs' places, found once a map (where the sea is deep enough), and for which map and seed
  float placedFor_[4] = {-1, -1, -1, -1};
  Berg placed_[kMostBergs] = {};
  int placedCount_ = 0;
  // MOHO64_DEBUG_HORIZON: where the showers and icebergs are, said when their number changes
  const bool debug_ = std::getenv("MOHO64_DEBUG_HORIZON") != nullptr;
  int toldShowers_ = -1, toldBergs_ = -1;
};

}  // namespace render
