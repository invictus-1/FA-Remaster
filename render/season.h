// [seasons] The seasons a map may be drawn in (Chris, 2026-10-18: a player's option; seasons.h says what each changes
// and where its art is named). Its own small header, for hosts that only set it (world_picture.h).
#pragma once
#include <string>

namespace render {

// Winter is reserved: its block of a map's remap.json is read in the same form, but its lying snow and its cold
// climate are still to come.
enum class Season { Summer = 0, Autumn = 1, Winter = 2 };
const char* SeasonName(Season season);  // "summer", "autumn", "winter"
// A season by its name as the viewer and a settings file give it ("summer", "autumn" or "fall", "winter"; any case).
bool SeasonNamed(const std::string& name, Season* out);

}  // namespace render
