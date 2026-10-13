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
enum class Climate { Temperate, Cold, Dry };  // rain and storms; snow instead of rain; mostly clear

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

class Precipitation {
 public:
  explicit Precipitation(gfx::Device* device) : device_(device) {}
  bool Init(std::string* error);
  // Into the scene's pass, over everything (it is tested against the scene's depth). `rain` and
  // `snow`: how hard each falls, 0 (not at all) .. 1.
  void Draw(const View& view, const Camera& camera, float rain, float snow);

 private:
  gfx::Device* device_;
  gfx::Pipeline pipeline_;
};

}  // namespace render
