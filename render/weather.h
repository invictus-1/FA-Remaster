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
// always fair), and the last quarter of a spell goes over into the next.
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

 private:
  gfx::Pipeline Make(bool lit, std::string* error);
  gfx::Pipeline MakeSplashes(std::string* error);
  gfx::Device* device_;
  gfx::Pipeline pipeline_, litPipeline_;
  bool litTried_ = false;
  gfx::Pipeline splashPipeline_;
  bool splashTried_ = false;
};

}  // namespace render
