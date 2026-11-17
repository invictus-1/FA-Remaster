// The Options' graphics-memory strip's numbers (ours; Chris's wish of 2026-11-14: "a small vram calculator spot that
// tells you how much vram it'll eat and if too much is on it'll recommend to lower a few settings"; ui/REQUESTS.md 57,
// the renderer's WorldPicture::EstimateCardMemory and CardMemory, render 106f227f). The strip itself is Lua: the Options
// dialog's hook (data/hook/lua/ui/dialogs/options.lua). The original game has no such thing.
#include "user/user_state.h"

#include <algorithm>
#include <string>

#include "script/script_state.h"
#include "user/bindings.h"
#ifdef MOHO64UI_WORLD_PICTURE
#include "render/world_picture.h"
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
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

#ifdef _WIN32
// The memory of the hardware adapter with the most of its own, in MB (0: none listed): the fallback where the
// renderer's own WorldPicture::AdapterMemoryMB cannot tell (below). INFERRED to be the card the renderer draws on (on
// a PC with one card, or a laptop whose stronger card has more memory, it is the same).
double LargestAdapterMB() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return 0;
  double best = 0;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
    DXGI_ADAPTER_DESC1 d = {};
    if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      best = (std::max)(best, d.DedicatedVideoMemory / 1048576.0);
    adapter->Release();
  }
  factory->Release();
  return best;
}
#endif

// moho64ui_CardMemoryNow() -> { total = MB, budget = MB, used = MB [, adapter = true] } | nil: the card's memory as the picture shown
// sees it now (the renderer's CardMemory: the card's own, or MOHO64UI_CARD_IMITATION's; the budget the system gives
// the program; what it uses). nil without a picture of the world on this thread (a run without the renderer, the
// front end of a run without a window, whose menu background draws on its own thread).
int l_CardMemoryNow(lua_State* L) {
  double total = 0, budget = 0, used = 0;
  const auto& ask = UserState::From(L)->config().cardMemoryNow;
  bool adapter = false;
  if (!ask || !ask(&total, &budget, &used) || total <= 0) {
    // (no picture of the world on this thread: the card's own size alone, from the system's list; budget and use 0)
    total = budget = used = 0;
    // (the renderer's own pick of the adapter, asked of DXGI without a device: WorldPicture::AdapterMemoryMB, render
    // d64904d3, made for this on 2026-11-17; where it cannot tell (0: not Windows, or a build without the renderer)
    // the largest hardware adapter of the system's list stands in, INFERRED the same card)
    static const double listed = [] {
      double mb = 0;
#ifdef MOHO64UI_WORLD_PICTURE
      mb = render::WorldPicture::AdapterMemoryMB();
#endif
#ifdef _WIN32
      if (mb <= 0) mb = LargestAdapterMB();
#endif
      return mb;
    }();
    total = listed;
    if (total <= 0) return lua_pushnil(L), 1;
    adapter = true;
  }
  lua_newtable(L);
  if (adapter) {
    lua_pushstring(L, "adapter");
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
  }
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

// moho64ui_SuggestedPreset() -> { preset = 0..4, texture_level = 0..2, shadow_quality = 1..3, reason = text } | nil: what
// the renderer suggests for this card (the option keys' own values: remaster_preset, texture_level, shadow_quality),
// by its memory and ray tracing alone. nil without a picture of the world on this thread. It changes nothing: the
// Options' strip shows it and the player may take it (Chris, 2026-11-17: recommended, never set by itself; max stays
// the default).
int l_SuggestedPreset(lua_State* L) {
  int preset = -1, texture = 0, shadow = 3;
  std::string reason;
  const auto& ask = UserState::From(L)->config().suggestedPreset;
  if (!ask || !ask(&preset, &texture, &shadow, &reason) || preset < 0) return lua_pushnil(L), 1;
  lua_newtable(L);
  lua_pushstring(L, "preset");
  lua_pushnumber(L, preset);
  lua_rawset(L, -3);
  lua_pushstring(L, "texture_level");
  lua_pushnumber(L, texture);
  lua_rawset(L, -3);
  lua_pushstring(L, "shadow_quality");
  lua_pushnumber(L, shadow);
  lua_rawset(L, -3);
  lua_pushstring(L, "reason");
  lua_pushstring(L, reason.c_str());
  lua_rawset(L, -3);
  return 1;
}

}  // namespace

void RegisterCardMemoryBindings(lua_State* L) {
#ifdef MOHO64UI_WORLD_PICTURE
  SetGlobal(L, "moho64ui_CardMemoryEstimate", l_CardMemoryEstimate);
#endif
  SetGlobal(L, "moho64ui_CardMemoryNow", l_CardMemoryNow);
  SetGlobal(L, "moho64ui_SuggestedPreset", l_SuggestedPreset);
}

}  // namespace moho::ui
