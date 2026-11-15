// The Options' graphics-memory strip's numbers (ours; Chris's wish of 2026-11-14: "a small vram calculator spot that
// tells you how much vram it'll eat and if too much is on it'll recommend to lower a few settings"; ui/REQUESTS.md 57,
// the renderer's WorldPicture::EstimateCardMemory and CardMemory, render 106f227f). The strip itself is Lua: the Options
// dialog's hook (data/hook/lua/ui/dialogs/options.lua). The original game has no such thing.
#include "user/user_state.h"

#include <string>

#include "script/script_state.h"
#include "user/bindings.h"
#ifdef MOHO64UI_WORLD_PICTURE
#include "render/world_picture.h"
#endif

namespace moho::ui {
namespace {

#ifdef MOHO64UI_WORLD_PICTURE
// (a field of the choice's table: its number, or `otherwise`; a truth value counts as 1 / 0)
double Field(lua_State* L, int t, const char* name, double otherwise) {
  lua_pushstring(L, name);
  lua_gettable(L, t);
  double v = otherwise;
  if (lua_isnumber(L, -1)) v = lua_tonumber(L, -1);
  else if (lua_isboolean(L, -1)) v = lua_toboolean(L, -1) ? 1 : 0;
  lua_pop(L, 1);
  return v;
}

// moho64ui_CardMemoryEstimate(choice) -> { total = MB, margin = MB, parts = { {id, name, mb, measured}, ... } }: the
// renderer's estimate of the card's memory for a set of settings (pure arithmetic; any time, no picture needed).
// `choice` holds plain values as the renderer's CardMemoryChoice: width, height, upscale, temporalUpscale,
// frameGeneration, samples, textureDetail (0 High, 1 Medium, 2 Low), rayTracing, shadowSize (4096 / 2048 / 1024; 0
// off), softShadows, atmosphere, groundDetail, weatherEffects, surround, gameSize ("small", "medium", "large").
// A part with mb 0 and measured false is NOT COUNTED YET (never free: the strip says so).
int l_CardMemoryEstimate(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  render::WorldPicture::CardMemoryChoice c;
  c.width = static_cast<int>(Field(L, 1, "width", c.width));
  c.height = static_cast<int>(Field(L, 1, "height", c.height));
  c.upscale = static_cast<float>(Field(L, 1, "upscale", c.upscale));
  c.temporalUpscale = Field(L, 1, "temporalUpscale", c.temporalUpscale ? 1 : 0) != 0;
  c.frameGeneration = Field(L, 1, "frameGeneration", c.frameGeneration ? 1 : 0) != 0;
  c.samples = static_cast<int>(Field(L, 1, "samples", c.samples));
  c.textureDetail = static_cast<int>(Field(L, 1, "textureDetail", c.textureDetail));
  c.rayTracing = Field(L, 1, "rayTracing", c.rayTracing ? 1 : 0) != 0;
  c.shadowSize = static_cast<int>(Field(L, 1, "shadowSize", c.shadowSize));
  c.softShadows = Field(L, 1, "softShadows", c.softShadows ? 1 : 0) != 0;
  c.atmosphere = Field(L, 1, "atmosphere", c.atmosphere ? 1 : 0) != 0;
  c.groundDetail = Field(L, 1, "groundDetail", c.groundDetail ? 1 : 0) != 0;
  c.weatherEffects = Field(L, 1, "weatherEffects", c.weatherEffects ? 1 : 0) != 0;
  c.surround = Field(L, 1, "surround", c.surround ? 1 : 0) != 0;
  lua_pushstring(L, "gameSize");
  lua_gettable(L, 1);
  if (lua_isstring(L, -1)) {
    const std::string size = lua_tostring(L, -1);
    if (size == "small") c.gameSize = render::WorldPicture::CardMemoryChoice::GameSize::Small;
    else if (size == "medium") c.gameSize = render::WorldPicture::CardMemoryChoice::GameSize::Medium;
  }
  lua_pop(L, 1);
  const render::WorldPicture::CardMemoryEstimate e = render::WorldPicture::EstimateCardMemory(c);
  lua_newtable(L);
  lua_pushstring(L, "total");
  lua_pushnumber(L, e.totalMB);
  lua_rawset(L, -3);
  lua_pushstring(L, "margin");
  lua_pushnumber(L, e.marginMB);
  lua_rawset(L, -3);
  lua_pushstring(L, "parts");
  lua_newtable(L);
  for (size_t i = 0; i < e.parts.size(); ++i) {
    const auto& p = e.parts[i];
    lua_newtable(L);
    lua_pushstring(L, "id");
    lua_pushstring(L, p.id ? p.id : "");
    lua_rawset(L, -3);
    lua_pushstring(L, "name");
    lua_pushstring(L, p.name ? p.name : "");
    lua_rawset(L, -3);
    lua_pushstring(L, "mb");
    lua_pushnumber(L, p.megabytes);
    lua_rawset(L, -3);
    lua_pushstring(L, "measured");
    lua_pushboolean(L, p.measured ? 1 : 0);
    lua_rawset(L, -3);
    lua_rawseti(L, -2, static_cast<int>(i) + 1);
  }
  lua_rawset(L, -3);
  return 1;
}
#endif

// moho64ui_CardMemoryNow() -> { total = MB, budget = MB, used = MB } | nil: the card's memory as the picture shown
// sees it now (the renderer's CardMemory: the card's own, or MOHO64UI_CARD_IMITATION's; the budget the system gives
// the program; what it uses). nil without a picture of the world on this thread (a run without the renderer, the
// front end of a run without a window, whose menu background draws on its own thread).
int l_CardMemoryNow(lua_State* L) {
  double total = 0, budget = 0, used = 0;
  const auto& ask = UserState::From(L)->config().cardMemoryNow;
  if (!ask || !ask(&total, &budget, &used) || total <= 0) return lua_pushnil(L), 1;
  lua_newtable(L);
  lua_pushstring(L, "total");
  lua_pushnumber(L, total);
  lua_rawset(L, -3);
  lua_pushstring(L, "budget");
  lua_pushnumber(L, budget);
  lua_rawset(L, -3);
  lua_pushstring(L, "used");
  lua_pushnumber(L, used);
  lua_rawset(L, -3);
  return 1;
}

}  // namespace

void RegisterCardMemoryBindings(lua_State* L) {
#ifdef MOHO64UI_WORLD_PICTURE
  SetGlobal(L, "moho64ui_CardMemoryEstimate", l_CardMemoryEstimate);
#endif
  SetGlobal(L, "moho64ui_CardMemoryNow", l_CardMemoryNow);
}

}  // namespace moho::ui
