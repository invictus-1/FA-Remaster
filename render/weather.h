// Weather for the computed sky (sky.h): what falls. Rain and snow are drawn as many small streaks
// or flakes in a box of air around what the camera looks at; each one's place follows from its
// number and the time alone, so nothing is kept or moved on the processor, and the box goes along
// with the camera while the drops stay where they are in the world. The box is as large as the
// camera is far, so there is as much to see from near as from far (this is rain as a picture of
// rain, not a count of real drops).
//
// What weather does to surfaces (wet, snow lying) and to the air (fog) is in the lighting every
// shader shares (lighting.h, by the view's `weather` values); the clouds are sky.h's.
#pragma once
#include <cstdint>
#include <string>

#include "camera.h"
#include "gfx.h"
#include "view.h"

namespace render {

// What the weather is at a moment: how much of the sky clouds cover, how hard it rains or snows,
// how thick the air is (each 0..1), and how bright a lightning's flash is just now (0: none).
struct WeatherNow {
  float clouds = 0.3f, rain = 0, snow = 0, fog = 0, flash = 0;
};
// rain and storms; snow instead of rain; mostly clear; [seasons] the temperate one in autumn (World: an autumn map's
// in place of Temperate): more overcast, showers that come and go, fog, and on its mornings fog (AutumnMorningFog)
enum class Climate { Temperate, Cold, Dry, Autumn };

// The weather of a game at `seconds` of its clock. It follows from the seed and the time alone:
// whoever asks with the same two gets the same weather, so every player of a game sees the same,
// a replay shows what the game showed, and nothing needs to be kept or sent. Time is cut into
// spells of `spell` seconds; each spell has one kind of weather, drawn by the seed (the first is
// always fair), and the last 45 % of a spell goes over into the next ([onset]: rain or snow coming, the clouds gather
// first and it begins once they have; going, it eases off first and the clouds break up after).
WeatherNow WeatherAt(uint32_t seed, double seconds, Climate climate = Climate::Temperate, float spell = 300);
// A storm's lightning alone: how bright its flash is at that moment, for rain that hard.
// `stroke`: which stroke it is (a number of its own for every stroke of a game), for whoever draws
// its bolt: the same number as long as the flash lasts.
float LightningAt(uint32_t seed, double seconds, float rain, uint32_t* stroke = nullptr);
// A number 0..1 from a seed and two whole numbers (always the same for the same three).
float WeatherDraw(uint32_t seed, uint32_t a, uint32_t b);
// (photomode) The kinds of weather WeatherAt's spells are drawn from, by number: 0 clear, 1 fair, 2 cloudy, 3 overcast,
// 4 rain, 5 storm, 6 fog, 7 snow, 8 showers (autumn's: rain that comes and goes; as one kind, a steady shower); each
// as WeatherAt gives it through such a spell (no flash). A number out of range: the nearest kind.
int WeatherKindCount();
const char* WeatherKindName(int kind);
WeatherNow WeatherKindOf(int kind);
// [snowhitch] Whether WeatherAt's spells in that climate can bring snow (a kind with snow that the climate draws: the cold
// one's), for what the snow wants made ahead of it (World::SnowAhead).
bool ClimateSnows(Climate climate);
// [seasons] Autumn's morning fog: how thick it lies at `hour` (0..24), 0..1, on top of what the weather draws: it
// gathers before daybreak, is thickest from about six to eight and has burnt off by half past ten; how thick, by the
// game's seed (every player of a game sees the same).
float AutumnMorningFog(uint32_t seed, float hour);

class Precipitation {
 public:
  explicit Precipitation(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // Into the scene's pass, over everything (it is tested against the scene's depth). `rain` and
  // `snow`: how hard each falls, 0 (not at all) .. 1. `lit` (World::litRain, the modern look): each drop or flake lit
  // where it is, by the sun through its shadow and the clouds' and by the small lights (a lamp's beam through the
  // lamp's shadow), sent on mostly forward (a drop between the eye and a light shines), on top of the sky's light.
  void Draw(const View& view, const Camera& camera, float rain, float snow, bool lit = false);
  // [rainreactive] Into the temporal upscaler's reactive mask (Post::Reactive's pass, R8): what the last Draw drew, each
  // drop marking the mask by how much it covers, so that FSR 3 shows this frame's drops (it gathered them away: thin,
  // faint and fast, under the mask's cutoff). Not the lit drops' layer near the ground. Once after each Draw.
  void DrawReactive(const View& view);
  // [falltrail] Into the temporal upscaler's transparency and composition mask (Post::Composition's pass, R8), before
  // DrawReactive: the last Draw's flakes (the snow; not the rain, nor the lit layer near the ground), each as its path of
  // late, `trailPixels` of the picture's pixels back from where it is. There FSR 3 holds what it gathered over the frames
  // before to what this frame shows nearby, and makes no lock (its hold on a thin bright feature): without it a share of
  // each flake's light stayed where the flake had been, a faint white line along its path that took a second and more to
  // go (Chris's test, 2026-11-09: "white trails that follow the snow"; measured over open water at 1080p, 2026-11-10: of
  // the light left 8 to 32 frames behind a flake, over what a picture without history shows there, a third less at 100 %
  // and a half to two thirds less at 59 %: less, not none). As often as wanted after a Draw, until DrawReactive.
  void DrawComposition(const View& view);
  float trailPixels = 6;
  // (whether DrawComposition would draw anything: no snow in the last Draw, no mask and no pass for it)
  bool WantsComposition() const { return drawn_ && drawnSnow_ > 0 && trailPixels > 0; }
  // Rain's splashes (World::rainSplashes): where rain meets what faces up and the picture shows it, small crowns that
  // rise and fade; found from `sceneDepth` (the scene's, read: a pass into the scene's colour alone).
  void DrawSplashes(const View& view, const Camera& camera, float rain, gfx::Texture sceneDepth);
  // [hitches] The lit drops' and the splashes' shaders made now (World::readAhead: the first frame their looks are on),
  // not when rain first falls in a game.
  void PrepareLit();
  void PrepareSplashes();

  // (photofix) A photo's frame (WorldPicture::StartPhoto) draws what falls as the window shows it: `pixelScale` of this
  // picture's pixels make one of the window's (a drop's width, its least length and a splash's least size are the
  // window's), and the picture shows `pictureShare` of the window's view (a tile: part of it; the splashes, so many to
  // the window's view, go by it). 1 and 1 in the game's own frames: as before.
  float pixelScale = 1, pictureShare = 1;
  // [rainunder] how much of the fall keeps to under the clouds (the world sets it: a storm's, World::stormCells): none
  // where the clouds' shadow (view.h, CloudShadow) shows open sky above; 0: everywhere, as before
  float underClouds = 0;
  // [fallsize] (Chris's test, 2026-11-09: with FSR 1 on "white trails that follow the snow", the snow looked faster) how many
  // of this picture's pixels make one of the window's, up and down, when an upscaler brings the picture up (the share it is
  // drawn at; 1 without one, and for whoever wants the widths in the picture's own pixels, as before): a flake's and a
  // drop's width are the WINDOW's pixels, as in a photo's frame (pixelScale), so a flake is as large on the screen with FSR
  // 1 or FSR 3 at 59 % as without (it was 1.7 times as wide and high, with 2.3 to 2.5 times the light: measured at 1080p,
  // 2026-11-10); never thinner than 0.9 of the picture's own pixels to each side (the rain's width: a streak that thin
  // holds together without anti-aliasing; a flake's light then changes 7 % from frame to frame with FSR 1, 5 % without).
  float windowShare = 1;
  // [fallpace] (Chris's test, 2026-11-09: "snow seems to speed up as I zoom in") The fall as fast on the SCREEN at every
  // zoom: each box on the ladder of sizes ([rainladder]) falls as fast as it is large (the rain's 30 and the snow's 2.5
  // world units a second at the box of 181, the camera 113 away; a box's drops keep that speed, so none changes its pace
  // during a zoom: the faster ones fade in as the camera rises), and a flake sways as wide as its own box asks, not as the
  // zoom of the moment does (during a zoom every flake moved sideways with the zoom, as fast as it fell in a quick one).
  // Measured at 1080p, 2026-11-10: the flakes move down the screen at 20, 20, 23, 25 and 17 pixels a second from 25, 50,
  // 100, 200 and 400 units away (with one speed in the world: 74, 45, 25, 14 and 5). False: one speed in the world at
  // every zoom ([rainpace], 2026-11-06) and the sway by the zoom, as before.
  bool steadyPace = true;

 private:
  gfx::Pipeline Make(bool lit, std::string* error);
  // (`reactive`: into a mask, by the masks' pipeline; `trail` over 0: [falltrail] each as its path, that many pixels back)
  void Fall(const View& view, const Camera& camera, float rain, float snow, bool lit, bool reactive, float trail = 0);
  bool ReactiveReady(bool trail = false);  // ([rainreactive], [falltrail]: a mask's pipeline, made the first time it is wanted)
  gfx::Pipeline MakeSplashes(std::string* error);
  gfx::Device* device_;
  gfx::Pipeline pipeline_, litPipeline_;
  bool litTried_ = false;
  gfx::Pipeline splashPipeline_;
  bool splashTried_ = false;
  gfx::Pipeline reactivePipeline_;  // [rainreactive]
  gfx::Pipeline trailPipeline_;     // [falltrail] (the shader's TRAIL variant: each flake as its path)
  bool reactiveTried_ = false, trailTried_ = false, drawn_ = false;
  Camera drawnCamera_;
  float drawnRain_ = 0, drawnSnow_ = 0;
};

}  // namespace render
