#include "sim/entity_grid.h"
#include "sim/prop_motor.h"
#include "sim/landnav.h"
#include "sim/sim.h"
#include "sim/script_bits.h"
#include "core/dmath.h"
#include "sim/formation.h"
#include "sim/transport.h"
#include "sim/air.h"
#include "sim/collision.h"
#include "sim/combat.h"
#include "sim/intel.h"
#include "sim/build.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "core/backtrace.h"
#include "core/crash.h"
#include "core/hostfs.h"
#include "core/log.h"
#include "core/vfs.h"
#include "script/script_state.h"
#include "script/threads.h"
#include "sim/commands.h"
#include "sim/motion.h"
#include "sim/navigation.h"
#include "sim/ai_brain.h"
#include "sim/ai_personality.h"
#include "sim/ai_platoon.h"
#include "sim/ai_threat.h"
#include "sim/ai_build.h"
#include "sim/session.h"
#include "sim/sim_cache.h"
#include "sim/terrain.h"
#include "sim/units.h"
#include "sim/vecmath.h"

namespace moho {
namespace {

const char kSimKey = 0;

Sim* S(lua_State* L) { return Sim::From(L); }

// ---- globals -------------------------------------------------------------------------------

int l_ListArmies(lua_State* L) {
  lua_newtable(L);
  int i = 0;
  for (const auto& a : S(L)->armies()) {
    lua_pushstring(L, a->name.c_str());
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}

int l_GetArmyBrain(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army %s", lua_tostring(L, 1) ? lua_tostring(L, 1) : "?");
  PushObject(L, a->brain);
  return 1;
}

// The simulation's own SessionIsReplay (score.lua asks it once an army is in focus).
int l_SessionIsReplay(lua_State* L) {
  lua_pushboolean(L, S(L)->isReplay);
  return 1;
}

// sim+0xa88 + 1, -1 stays -1 (0x75ce80). It is the focus of the last Sync: a pending change is not seen before
// the Sync that applies it (Sim::RequestFocusArmy).
int l_GetFocusArmy(lua_State* L) {
  lua_pushnumber(L, S(L)->focusArmy == -1 ? -1 : S(L)->focusArmy + 1);
  return 1;
}

// FAF's sim global SetFocusArmy(index0) (its exe's 0x128ce81, put in the sim's binding list 0xf5a124 by its patch
// at 0xfe4986..0xfe4a08, help "SetFocusArmy(armyIndex or -1)"; simInit.lua's MergeArmyPair calls it with the
// 0-based index of the army the focus army was merged into): lua_tonumber of argument 1, truncated, written to
// the pending focus SimDriver+0xb0. No argument count, no range check: it takes effect at this beat's Sync, as
// the interface's request does (Sim::RequestFocusArmy). Returns nothing.
int l_SetFocusArmy(lua_State* L) {
  S(L)->RequestFocusArmy(static_cast<int>(static_cast<float>(lua_tonumber(L, 1))));
  return 0;
}

int l_ArmyIsCivilian(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  lua_pushboolean(L, a && a->civilian);
  return 1;
}

// The army's damage handicap (army +0x1e0 when its flag +0x1dc is set; 0 otherwise). Nothing in
// the lobby sets one, so it is 0 (sim/damage.cpp applies none).
int l_ArmyGetHandicap(lua_State* L) {
  S(L)->GetArmy(L, 1);
  lua_pushnumber(L, 0);
  return 1;
}

int l_ArmyIsOutOfGame(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  lua_pushboolean(L, a && a->outOfGame);
  return 1;
}

int l_SetArmyOutOfGame(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1)) a->outOfGame = true;
  return 0;
}

// SetArmyColor(army, r, g, b) and SetArmyShowScore(army, bool): kept for the interface (sim/sim_view.h)
int l_SetArmyColor(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1))
    for (int i = 0; i < 3; ++i) a->rgb[i] = static_cast<int>(luaL_checknumber(L, 2 + i));
  return 0;
}
int l_SetArmyShowScore(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1)) a->showScore = lua_toboolean(L, 2) != 0;
  return 0;
}

// ---- the campaign's army and map functions (ui REQUESTS 31) -----------------------------------------------------

// "%s\n  expected %d args, but got %d" (0xe0a220): every cfunc's first test, with the binding's help text
void ArgCount(lua_State* L, int n, const char* doc) {
  if (lua_gettop(L) != n) luaL_error(L, "%s\n  expected %d args, but got %d", doc, n, lua_gettop(L));
}
// LuaStackObject "integer" (lua_type must be a number, TypeError 0x4154b0), then _ftol2 0xa89cc0: truncation, out of
// range 0x80000000
int IntArg(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "integer");
  const double d = lua_tonumber(L, idx);
  return d > -2147483649.0 && d < 2147483648.0 ? static_cast<int>(d) : static_cast<int>(0x80000000u);
}
// ... and the same number through fistp with the rounding set to chop (SetArmyColorIndex 0x709f7c,
// SetArmyFactionIndex 0x70a0db: a 64-bit integer, its low half kept)
int ChopArg(lua_State* L, int idx) {
  if (lua_type(L, idx) != LUA_TNUMBER) luaL_typerror(L, idx, "integer");
  const double d = std::trunc(lua_tonumber(L, idx));
  return d > -9.2e18 && d < 9.2e18 ? static_cast<int>(static_cast<uint32_t>(static_cast<int64_t>(d)))
                                   : static_cast<int>(0x80000000u);
}
// ARMY_FromLuaState 0x707d60: a 1-based index (LuaObject::GetInteger) or an army's name
Army* ArmyArg(lua_State* L, int idx) {
  Sim* sim = S(L);
  if (lua_type(L, idx) == LUA_TNUMBER) {
    const int i = static_cast<int>(static_cast<float>(lua_tonumber(L, idx))) - 1;
    if (i >= 0 && static_cast<size_t>(i) < sim->armies().size()) return sim->armies()[static_cast<size_t>(i)].get();
    if (i < 0) luaL_error(L, "Invalid army %d. (Use a 1-based index)", i + 1);
    luaL_error(L, "Invalid army %d", i);
  }
  if (lua_type(L, idx) == LUA_TSTRING) {
    if (Army* a = sim->GetArmy(L, idx)) return a;
    luaL_error(L, "Unknown army: %s", lua_tostring(L, idx));
  }
  luaL_error(L, "Unexpected type for army object");
  return nullptr;
}

// SetPlayableRect(minX, minZ, maxX, maxZ) 0x75c830: four integers (truncated); STIMap::SetPlayableMapRect 0x577df0
// clamps each into 0 .. the map's size (heightfield size - 1) and takes the rect only when maxX > minX and
// maxZ > minZ, else the error. What it changes: sim.h playableRect (IsWithin and the rect's other readers).
int l_SetPlayableRect(lua_State* L) {
  ArgCount(L, 4, "SetPlayableRect( minX, minZ, maxX, maxZ )");
  Sim& sim = *S(L);
  const int z1 = IntArg(L, 4);  // (read 4, 3, 2, 1: the first wrong one names itself)
  const int x1 = IntArg(L, 3);
  const int z0 = IntArg(L, 2);
  const int x0 = IntArg(L, 1);
  const int w = sim.map() ? sim.map()->width() : 0, h = sim.map() ? sim.map()->height() : 0;
  auto clamp = [](int v, int hi) { return std::max(0, std::min(v, hi)); };
  const int r[4] = {clamp(x0, w), clamp(z0, h), clamp(x1, w), clamp(z1, h)};
  if (!(r[2] > r[0] && r[3] > r[1])) return luaL_error(L, "Attempted to set an invalid playable rect.");
  std::copy(r, r + 4, sim.playableRect);
  return 0;
}

// SetIgnorePlayableRect(army, flag) 0x709230: the army's flag (army vt+0xb4 = CArmyImpl::SetIgnorePlayableRect
// 0x6fdec0, +0x178), a boolean (GetBoolean: nil is false)
int l_SetIgnorePlayableRect(lua_State* L) {
  ArgCount(L, 2, "army, flag");
  Army* a = ArmyArg(L, 1);
  if (lua_type(L, 2) != LUA_TBOOLEAN && lua_type(L, 2) != LUA_TNIL) luaL_typerror(L, 2, "boolean");
  a->ignorePlayableRect = lua_toboolean(L, 2) != 0;
  return 0;
}

// SetArmyFactionIndex(army, index) 0x70a030: the 0-based faction (+0x174), as the integer given
int l_SetArmyFactionIndex(lua_State* L) {
  ArgCount(L, 2, "SetArmyFactionIndex(army,index)");
  Army* a = ArmyArg(L, 1);
  a->faction = ChopArg(L, 2);
  return 0;
}

// SetArmyColorIndex(army, index) 0x709ed0: GetPlayerColor 0x5068b0 (/lua/gameColors.lua run once in a colour state
// of its own, GameColors.PlayerColors[index + 1], SCR_DecodeColor 0x4b2d20) becomes the army's colour and its
// player colour (+0x150 and +0x154), as SetArmyColor sets both: ours keeps it as the colour set outright (rgb).
int l_SetArmyColorIndex(lua_State* L) {
  ArgCount(L, 2, "SetArmyColorIndex(army,index)");
  Sim& sim = *S(L);
  Army* a = ArmyArg(L, 1);
  const int index = ChopArg(L, 2);
  const int top = lua_gettop(L);
  std::string text;
  bool isString = false;
  if (sim.PushImport(L, "/lua/gameColors.lua")) {
    lua_pushstring(L, "GameColors");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "PlayerColors");
      lua_gettable(L, -2);
      if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, index + 1);
        isString = lua_type(L, -1) == LUA_TSTRING;
        if (isString) text = lua_tostring(L, -1);
      }
    }
  }
  lua_settop(L, top);
  if (!isString) return luaL_error(L, "Invalid color, must be a string.");
  uint32_t argb = 0;
  if (!DecodeColor(text.c_str(), &argb)) return luaL_error(L, "Unknown color: %s", text.c_str());
  a->rgb[0] = static_cast<int>((argb >> 16) & 0xff);
  a->rgb[1] = static_cast<int>((argb >> 8) & 0xff);
  a->rgb[2] = static_cast<int>(argb & 0xff);
  return 0;
}

// SetArmyAIPersonality(army, personality) 0x70a180: a string (lua_tostring, TypeError "string"); a non-empty one
// replaces the army's personality name (+0x158)
int l_SetArmyAIPersonality(lua_State* L) {
  ArgCount(L, 2, "SetArmyAIPersonality(army,personality)");
  Army* a = ArmyArg(L, 1);
  const char* s = lua_tostring(L, 2);
  if (!s) luaL_typerror(L, 2, "string");
  if (*s) a->personality = s;
  return 0;
}

int l_GetGameTick(lua_State* L) {
  lua_pushnumber(L, S(L)->tick());
  return 1;
}

int l_GetGameTimeSeconds(lua_State* L) {
  lua_pushnumber(L, S(L)->tick() * 0.1f);
  return 1;
}

int l_GetSystemTimeSecondsOnlyForProfileUse(lua_State* L) {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  lua_pushnumber(L, duration<float>(steady_clock::now() - t0).count());
  return 1;
}

int l_GetMapSize(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->width() : 0);
  lua_pushnumber(L, m ? m->height() : 0);
  return 2;
}

int l_GetTerrainHeight(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->TerrainHeight(luaL_checknumber(L, 1), luaL_checknumber(L, 2)) : 0);
  return 1;
}

// Terrain types: /lua/TerrainTypes.lua's TerrainTypes list, by TypeCode (loaded on first use).
const char kTerrainKey = 0;
void PushTerrainTypes(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kTerrainKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);  // code -> entry
  int byCode = lua_gettop(L);
  lua_newtable(L);  // env with access to globals
  int env = lua_gettop(L);
  lua_newtable(L);
  lua_pushstring(L, "__index");
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_rawset(L, -3);
  lua_setmetatable(L, env);
  lua_getglobal(L, "doscript");
  lua_pushstring(L, "/lua/TerrainTypes.lua");
  lua_pushvalue(L, env);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    LogScriptError(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pushstring(L, "TerrainTypes");
  lua_rawget(L, env);
  if (lua_istable(L, -1))
    for (int i = 1;; ++i) {
      lua_rawgeti(L, -1, i);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      lua_pushstring(L, "TypeCode");
      lua_rawget(L, -2);
      int code = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : -1;
      lua_pop(L, 1);
      lua_pushstring(L, "Name");
      lua_rawget(L, -2);
      bool isDefault = lua_isstring(L, -1) && !std::strcmp(lua_tostring(L, -1), "Default");
      lua_pop(L, 1);
      if (isDefault) {
        lua_pushstring(L, "Default");
        lua_pushvalue(L, -2);
        lua_rawset(L, byCode);
      }
      if (code >= 0) lua_rawseti(L, byCode, code);
      else lua_pop(L, 1);
    }
  lua_settop(L, byCode);
  lua_pushlightuserdata(L, const_cast<char*>(&kTerrainKey));
  lua_pushvalue(L, byCode);
  lua_rawset(L, LUA_REGISTRYINDEX);
}

// Push the terrain type entry at map cell (x, z); (-1, -1) and unknown codes give 'Default'.
void PushTerrainTypeAt(lua_State* L, float x, float z) {
  PushTerrainTypes(L);
  const TerrainMap* m = S(L)->map();
  int code = -1;
  if (m && x >= 0 && z >= 0) code = m->TerrainType(static_cast<int>(x), static_cast<int>(z));
  lua_rawgeti(L, -1, code);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_pushstring(L, "Default");
    lua_rawget(L, -2);
  }
  lua_remove(L, -2);
}

int l_GetTerrainType(lua_State* L) {
  PushTerrainTypeAt(L, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
  return 1;
}

int l_GetTerrainTypeOffset(lua_State* L) {
  PushTerrainTypeAt(L, static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)));
  float off = 0;
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "HeightOffset");
    lua_rawget(L, -2);
    if (lua_isnumber(L, -1)) off = static_cast<float>(lua_tonumber(L, -1));
  }
  lua_pushnumber(L, off);
  return 1;
}

int l_GetSurfaceHeight(lua_State* L) {
  const TerrainMap* m = S(L)->map();
  lua_pushnumber(L, m ? m->SurfaceHeight(luaL_checknumber(L, 1), luaL_checknumber(L, 2)) : 0);
  return 1;
}

int l_CheatsEnabled(lua_State* L) {
  lua_pushboolean(L, S(L)->cheats);
  return 1;
}

// GetCurrentCommandSource 0x74b790: the current command source + 1 (nil when none is active)
int l_GetCurrentCommandSource(lua_State* L) {
  Sim* sim = S(L);
  if (sim->commandSource == 0xff) lua_pushnil(L);
  else lua_pushnumber(L, static_cast<float>(sim->commandSource + 1));
  return 1;
}

// OkayToMessWithArmy(army) 0x70a9a0 = Sim::OkayToMessWith(army) 0x747320
int l_OkayToMessWithArmy(lua_State* L) {
  Sim* sim = S(L);
  Army* a = lua_isnil(L, 1) ? nullptr : sim->GetArmy(L, 1);
  lua_pushboolean(L, sim->OkayToMessWith(a));
  return 1;
}

// IsGameOver() 0x74b980: the EndGame order has been taken (sim+0x8dc) or the game stands already (sim+0x8dd).
// The beat after the order still runs, so the scripts' threads see it once: FAF's GameOverListenerThread waits
// on it and sends the final score with that beat's sync table.
int l_IsGameOver(lua_State* L) {
  const Sim* sim = Sim::From(L);
  lua_pushboolean(L, sim && (sim->endGame || sim->frozen));
  return 1;
}

// SetCommandSource(army, source, enabled): which command sources may command an army (Army::sources, army+0x130).
// Not the original's: a function FAF's exe patch adds (its name is in the patch section, 0x1294028), so its
// behaviour is taken from how FAF's scripts use it: both numbers count from 0 (victory.lua:
// SetCommandSource(i - 1, index, false) takes a defeated player's source off every army), true adds the source,
// false takes it away. Nothing else happens (no AbandonedByPlayer: that is the stream's CommandSourceTerminated).
int l_SetCommandSource(lua_State* L) {
  Sim* sim = S(L);
  const int army = static_cast<int>(luaL_checknumber(L, 1));
  const int src = static_cast<int>(luaL_checknumber(L, 2));
  const bool on = lua_toboolean(L, 3) != 0;
  const auto& armies = sim->armies();
  if (army < 0 || static_cast<size_t>(army) >= armies.size()) return 0;
  auto& v = armies[static_cast<size_t>(army)]->sources;
  const auto it = std::find(v.begin(), v.end(), src);
  if (on) {
    if (it == v.end()) v.push_back(src);
  } else if (it != v.end()) {
    v.erase(it);
  }
  return 0;
}

// EndGame() 0x74b8b0: Sim::EndGame (the simulation's vtable +0x5c), what the EndGame order of the command stream
// does: the next beat still runs, then the game stands.
int l_EndGame(lua_State* L) {
  S(L)->endGame = true;
  return 0;
}

// Random() -> [0,1]; Random(max) -> 1..max; Random(min, max) -> min..max (integers; 0x759010, sim_random.md 3)
int32_t RandomArg(lua_State* L, int i) {
  if (lua_type(L, i) != LUA_TNUMBER) luaL_typerror(L, i, "number");
  float f = static_cast<float>(lua_tonumber(L, i));
  if (!(f > -2147483904.0f && f < 2147483648.0f)) return INT32_MIN;  // MSVC cvttss2si: NaN / out of range
  return static_cast<int32_t>(f);
}
int l_Random(lua_State* L) {
  Sim* sim = S(L);
  int n = lua_gettop(L);
  if (n > 2) return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", "Random", 0, 2, n);
  if (sim->rngTrace_) {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar))
      Logf(LogLevel::Info, "rnglua %u %s:%d n%d %g %g", sim->tick(), ar.short_src, ar.currentline, n,
           n >= 1 ? lua_tonumber(L, 1) : 0.0, n >= 2 ? lua_tonumber(L, 2) : 0.0);
    if (getenv("MOHO64_DEBUG_RNGTB")) {
      std::string tb;
      for (int lv = 1; lv < 24 && lua_getstack(L, lv, &ar); ++lv)
        if (lua_getinfo(L, "Sl", &ar)) tb += std::string(" < ") + ar.short_src + ":" + std::to_string(ar.currentline);
      Logf(LogLevel::Info, "rngtb %u%s", sim->tick(), tb.c_str());
    }
  }
  if (n == 0) {
    lua_pushnumber(L, sim->U01());
  } else if (n == 1) {
    uint32_t hi = static_cast<uint32_t>(RandomArg(L, 1));
    uint32_t u = sim->NextUInt32();
    lua_pushnumber(L, static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>((static_cast<uint64_t>(u) * hi) >> 32) + 1u)));
  } else {
    uint32_t hp1 = static_cast<uint32_t>(RandomArg(L, 2)) + 1u;
    uint32_t lo = static_cast<uint32_t>(RandomArg(L, 1));
    uint32_t u = sim->NextUInt32();
    lua_pushnumber(L, static_cast<float>(static_cast<int32_t>(
                          static_cast<uint32_t>((static_cast<uint64_t>(u) * (hp1 - lo)) >> 32) + lo)));
  }
  return 1;
}

// Alliances: 0 enemy, 1 neutral, 2 ally.
int Relation(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  if (!a || !b) return luaL_error(L, "Invalid army");
  if (a == b) return 2;
  return a->alliance[b->index - 1];
}
int l_IsAlly(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 2);
  return 1;
}
int l_IsEnemy(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 0);
  return 1;
}
int l_IsNeutral(lua_State* L) {
  lua_pushboolean(L, Relation(L) == 1);
  return 1;
}
int ParseRelation(lua_State* L, int idx) {
  const char* s = luaL_checkstring(L, idx);
  if (!std::strcmp(s, "Ally")) return 2;
  if (!std::strcmp(s, "Neutral")) return 1;
  if (!std::strcmp(s, "Enemy")) return 0;
  return luaL_error(L, "Unknown alliance type %s", s);
}
int l_SetAlliance(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  int r = ParseRelation(L, 3);
  if (!a || !b) return luaL_error(L, "Invalid army");
  a->alliance[b->index - 1] = r;
  b->alliance[a->index - 1] = r;
  return 0;
}
int l_SetAllianceOneWay(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  Army* b = S(L)->GetArmy(L, 2);
  int r = ParseRelation(L, 3);
  if (!a || !b) return luaL_error(L, "Invalid army");
  a->alliance[b->index - 1] = r;
  return 0;
}

int l_SetArmyPlans(lua_State* L) {
  if (Army* a = S(L)->GetArmy(L, 1)) a->plans = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
  return 0;
}

// InitializeArmyAI(army): the brain's own set-up (CAiBrain::Initialize in the original) -
// brain:OnCreateHuman(planName) or brain:OnCreateAI(planName). The plan name is the army's plans string as it
// is (0x57a440: "None" stands in only for a null pointer, and the army's accessor returns its string's text), so
// an army whose plans were never set hands over '' - which the brain scripts test for before importing a plan.
// FAF's OnCreateArmyBrain calls this after choosing the brain's class.
int l_InitializeArmyAI(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushstring(L, a->plans.c_str());
  S(L)->CallMethod(L, a->brain, a->human ? "OnCreateHuman" : "OnCreateAI", 1);
  return 0;
}

int l_GetArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushnumber(L, a->unitCap);
  return 1;
}
int l_GetArmyUnitCostTotal(lua_State* L) {  // the army's units counted against the cap (General.CapCost)
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  lua_pushnumber(L, a->unitCost);
  return 1;
}
int l_SetArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->unitCap = static_cast<float>(luaL_checknumber(L, 2));
  return 0;
}
int l_SetIgnoreArmyUnitCap(lua_State* L) {
  Army* a = S(L)->GetArmy(L, 1);
  if (!a) return luaL_error(L, "Invalid army");
  a->ignoreUnitCap = lua_toboolean(L, 2);
  return 0;
}

int l_ShouldCreateInitialArmyUnits(lua_State* L) {
  lua_pushboolean(L, 1);  // a new session (not a loaded save)
  return 1;
}

// ---- CAiBrain --------------------------------------------------------------------------------

AiBrain* Brain(lua_State* L) { return CheckObject<AiBrain>(L, 1); }

int l_brain_GetPersonality(lua_State* L) {
  PushObject(L, CheckObject<AiBrain>(L, 1)->personality);
  return 1;
}
int l_brain_GetArmyIndex(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->index);
  return 1;
}
int l_brain_GetFactionIndex(lua_State* L) {
  lua_pushnumber(L, Brain(L)->army->faction + 1);
  return 1;
}
int l_brain_IsDefeated(lua_State* L) {
  lua_pushboolean(L, Brain(L)->army->outOfGame);
  return 1;
}

// GetArmyStat(name, default) -> { Value = ... }
int l_brain_GetArmyStat(lua_State* L) {
  Army* a = Brain(L)->army;
  std::string name = luaL_checkstring(L, 2);
  auto it = a->stats.find(name);
  float v = it != a->stats.end() ? it->second : static_cast<float>(luaL_optnumber(L, 3, 0));
  lua_newtable(L);
  lua_pushstring(L, "Value");
  lua_pushnumber(L, v);
  lua_rawset(L, -3);
  return 1;
}
int l_brain_SetArmyStat(lua_State* L) {
  Brain(L)->army->stats[luaL_checkstring(L, 2)] = static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
int l_brain_AddArmyStat(lua_State* L) {
  Brain(L)->army->stats[luaL_checkstring(L, 2)] += static_cast<float>(luaL_checknumber(L, 3));
  return 0;
}
bool IEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
// ETriggerOperator enum parse (0x8d9670): case-insensitive, optional TRIGGER_ prefix, decimal, a|b
int ParseTriggerOp(lua_State* L, const char* s) {
  static const char* names[4] = {"GreaterThan", "GreaterThanOrEqual", "LessThan", "LessThanOrEqual"};
  int v = 0;
  std::string all = s ? s : "";
  size_t pos = 0;
  bool any = false;
  while (pos <= all.size()) {
    size_t bar = all.find('|', pos);
    std::string t = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
    if (t.size() >= 8 && IEquals(t.substr(0, 8), "TRIGGER_")) t = t.substr(8);
    int k = -1;
    for (int i = 0; i < 4; ++i)
      if (IEquals(t, names[i])) k = i;
    if (k < 0 && !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
      k = std::atoi(t.c_str());
    if (k < 0)
      luaL_error(L, "Invalid enum value %s\nValid Options are:\n   GreaterThan\n   GreaterThanOrEqual\n   LessThan\n   LessThanOrEqual", s);
    v |= k;
    any = true;
    if (bar == std::string::npos) break;
    pos = bar + 1;
  }
  return any ? v : 0;
}
// SetArmyStatsTrigger(statName, triggerName, compareType, value [, category]) 0x587c00
int l_brain_SetArmyStatsTrigger(lua_State* L) {
  int n = lua_gettop(L);
  if (n < 5 || n > 6) return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", "SetArmyStatsTrigger", 5, 6, n);
  Army* a = Brain(L)->army;
  std::string stat = luaL_checkstring(L, 2), name = luaL_checkstring(L, 3);
  int op = ParseTriggerOp(L, luaL_checkstring(L, 4));
  if (lua_type(L, 5) != LUA_TNUMBER) luaL_typerror(L, 5, "number");
  float thr = static_cast<float>(lua_tonumber(L, 5));
  bool cat = n > 5 && !lua_isnil(L, 6);
  std::shared_ptr<Army::StatTrigger> t;
  for (auto& x : a->statTriggers)
    if (IEquals(x->name, name)) {
      t = x;
      break;
    }
  if (!t) {
    t = std::make_shared<Army::StatTrigger>();
    t->name = name;
    a->statTriggers.push_back(t);
  }
  if (!a->stats.count(stat)) {
    Logf(LogLevel::Warning, "ArmyStatItem %s does not exist.", stat.c_str());
    return 0;
  }
  t->conds.push_back({stat, op, thr, cat});
  return 0;
}
// RemoveArmyStatsTrigger(statName, triggerName) 0x588020: the first trigger of that name (the stat is ignored)
int l_brain_RemoveArmyStatsTrigger(lua_State* L) {
  if (lua_gettop(L) != 3) return luaL_error(L, "%s\n  expected %d args, but got %d", "RemoveArmyStatsTrigger", 3, lua_gettop(L));
  Army* a = Brain(L)->army;
  luaL_checkstring(L, 2);
  std::string name = luaL_checkstring(L, 3);
  for (auto it = a->statTriggers.begin(); it != a->statTriggers.end(); ++it)
    if (IEquals((*it)->name, name)) {
      a->statTriggers.erase(it);
      break;
    }
  return 0;
}
int l_brain_GetBlueprintStat(lua_State* L) {
  Brain(L);
  lua_pushnumber(L, 0);  // TODO(M4): per-blueprint army stats
  return 1;
}

}  // namespace

// Unit::UpdateTerrainType 0x6a9b50 (declared in sim/motion.h). The cell: Entity::GetFootprint 0x678880, then
// fistp(z - SizeZ * 0.5) and fistp(x - SizeX * 0.5) (0x6a9b8a..0x6a9bd6, kept as int16); it must lie inside the
// heightfield's cells (width - 1, height - 1, unsigned compares 0x6a9bf4 / 0x6a9bfe), else the code is 1. The code
// is stored in unit+0x57c either way (0x6a9c1e); only when it differs from the stored one are the two entries
// fetched from the map's table (STIMap+0x18, 256 LuaObjects by TypeCode, nil where TerrainTypes.lua has none:
// STIMap::LoadTerrainTypes 0x577f60, read by 0x6a4c20) and RunScript_OnTerrainTypeChange 0x6b0c50 called with
// (new, old).
void UpdateTerrainType(Sim& sim, Unit* u, float x, float z) {
  const TerrainMap* m = sim.map();
  if (!m || !u->motion.bp) return;
  int cx, cz;
  GoalCell(UnitFootprint(u), x, z, &cx, &cz);
  const uint8_t code =
      (cx >= 0 && cz >= 0 && cx < m->width() && cz < m->height()) ? m->TerrainType(cx, cz) : uint8_t{1};
  const uint8_t old = u->terrainType;
  u->terrainType = code;
  if (code == old) return;
  // MOHO64_DEBUG_TTYPE=<unit id decimal | -2 for every unit>: each change, with the cell it was read at
  static const long dbg = getenv("MOHO64_DEBUG_TTYPE") ? atol(getenv("MOHO64_DEBUG_TTYPE")) : -1;
  if (dbg == -2 || (dbg >= 0 && dbg == static_cast<long>(u->id)))
    Logf(LogLevel::Info, "ttype %u unit %u (%s) at %.4f %.4f cell %d %d: %d -> %d", sim.tick(), u->id,
         u->blueprint ? u->blueprint->id.c_str() : "?", x, z, cx, cz, old, code);
  lua_State* L = sim.L();
  PushTerrainTypes(L);
  lua_rawgeti(L, -1, code);
  lua_rawgeti(L, -2, old);
  lua_remove(L, -3);
  sim.CallMethod(L, u, "OnTerrainTypeChange", 2);
}

void RegisterSimBindings(lua_State* L) {
  SetMethod(L, "CAiBrain", "GetArmyStat", l_brain_GetArmyStat);
  SetMethod(L, "CAiBrain", "SetArmyStat", l_brain_SetArmyStat);
  SetMethod(L, "CAiBrain", "AddArmyStat", l_brain_AddArmyStat);
  SetMethod(L, "CAiBrain", "GetBlueprintStat", l_brain_GetBlueprintStat);
  SetMethod(L, "CAiBrain", "SetArmyStatsTrigger", l_brain_SetArmyStatsTrigger);
  SetMethod(L, "CAiBrain", "RemoveArmyStatsTrigger", l_brain_RemoveArmyStatsTrigger);
  SetGlobal(L, "ListArmies", l_ListArmies);
  SetGlobal(L, "GetArmyBrain", l_GetArmyBrain);
  SetGlobal(L, "GetFocusArmy", l_GetFocusArmy);
  SetGlobal(L, "SetFocusArmy", l_SetFocusArmy);
  SetGlobal(L, "SessionIsReplay", l_SessionIsReplay);
  SetGlobal(L, "ArmyIsCivilian", l_ArmyIsCivilian);
  SetGlobal(L, "ArmyGetHandicap", l_ArmyGetHandicap);
  SetGlobal(L, "ArmyIsOutOfGame", l_ArmyIsOutOfGame);
  SetGlobal(L, "SetArmyOutOfGame", l_SetArmyOutOfGame);
  SetGlobal(L, "SetArmyColor", l_SetArmyColor);
  SetGlobal(L, "SetArmyShowScore", l_SetArmyShowScore);
  SetGlobal(L, "SetPlayableRect", l_SetPlayableRect);
  SetGlobal(L, "SetIgnorePlayableRect", l_SetIgnorePlayableRect);
  SetGlobal(L, "SetArmyFactionIndex", l_SetArmyFactionIndex);
  SetGlobal(L, "SetArmyColorIndex", l_SetArmyColorIndex);
  SetGlobal(L, "SetArmyAIPersonality", l_SetArmyAIPersonality);
  SetGlobal(L, "GetCueBank", l_GetCueBank);
  SetGlobal(L, "GetGameTick", l_GetGameTick);
  SetGlobal(L, "GetGameTimeSeconds", l_GetGameTimeSeconds);
  SetGlobal(L, "GetSystemTimeSecondsOnlyForProfileUse", l_GetSystemTimeSecondsOnlyForProfileUse);
  SetGlobal(L, "GetMapSize", l_GetMapSize);
  SetGlobal(L, "GetTerrainHeight", l_GetTerrainHeight);
  SetGlobal(L, "GetSurfaceHeight", l_GetSurfaceHeight);
  SetGlobal(L, "GetTerrainType", l_GetTerrainType);
  SetGlobal(L, "GetTerrainTypeOffset", l_GetTerrainTypeOffset);
  SetGlobal(L, "CheatsEnabled", l_CheatsEnabled);
  SetGlobal(L, "GetCurrentCommandSource", l_GetCurrentCommandSource);
  SetGlobal(L, "OkayToMessWithArmy", l_OkayToMessWithArmy);
  SetGlobal(L, "IsGameOver", l_IsGameOver);
  SetGlobal(L, "SetCommandSource", l_SetCommandSource);
  SetGlobal(L, "EndGame", l_EndGame);
  SetGlobal(L, "Random", l_Random);
  SetGlobal(L, "IsAlly", l_IsAlly);
  SetGlobal(L, "IsEnemy", l_IsEnemy);
  SetGlobal(L, "IsNeutral", l_IsNeutral);
  SetGlobal(L, "SetAlliance", l_SetAlliance);
  SetGlobal(L, "SetAllianceOneWay", l_SetAllianceOneWay);
  SetGlobal(L, "SetArmyPlans", l_SetArmyPlans);
  SetGlobal(L, "ShouldCreateInitialArmyUnits", l_ShouldCreateInitialArmyUnits);
  SetGlobal(L, "InitializeArmyAI", l_InitializeArmyAI);
  SetGlobal(L, "GetArmyUnitCap", l_GetArmyUnitCap);
  SetGlobal(L, "GetArmyUnitCostTotal", l_GetArmyUnitCostTotal);
  SetGlobal(L, "SetArmyUnitCap", l_SetArmyUnitCap);
  SetGlobal(L, "SetIgnoreArmyUnitCap", l_SetIgnoreArmyUnitCap);
  SetMethod(L, "CAiBrain", "GetArmyIndex", l_brain_GetArmyIndex);
  SetMethod(L, "CAiBrain", "GetPersonality", l_brain_GetPersonality);
  RegisterAiPersonalityBindings(L);
  SetMethod(L, "CAiBrain", "GetFactionIndex", l_brain_GetFactionIndex);
  SetMethod(L, "CAiBrain", "IsDefeated", l_brain_IsDefeated);
}

// ---- the engine's script fixes ---------------------------------------------------------------

namespace {
// The fixes made in the game's Lua (docs/deviations.md): the .lua files of the repository's data/fixes/hook,
// laid out like the game's own paths (data/fixes/hook/lua/simInit.lua hooks /lua/simInit.lua). The folder is
// found beside the program (data/fixes) or one folder up (the repository's, from build-zig/, build-ui/ or the
// viewer's build folder, all in the repository's root); MOHO64_FIXES_DIR names another. The files are read at
// each start and given to the sim state alone (ScriptState::SetExtraHooks): one more hook folder after the init
// file's (FAF's /schook) and before the mods' hooks, as the game takes a hook folder. Nothing is mounted: the
// game's file system stays as the host made it (the interface host's is shared with its other threads).
std::map<std::string, ScriptState::ExtraHook> FixesHooks() {
  std::map<std::string, ScriptState::ExtraHook> hooks;
  std::string folder;
  if (const char* d = getenv("MOHO64_FIXES_DIR"); d && *d) {
    folder = d;
  } else if (const std::string exe = hostfs::ProgramFolder(); !exe.empty()) {
    for (const std::string& c : {exe + "/data/fixes", exe + "/../data/fixes"})
      if (folder.empty() && hostfs::IsDirectory(c + "/hook")) folder = c;
  }
  const std::string root = folder.empty() ? std::string() : hostfs::Resolve(folder + "/hook");
  if (root.empty() || !hostfs::IsDirectory(root)) {
    Logf(LogLevel::Warning, "moho64: the engine's script fixes are on, but their folder (data/fixes/hook) is not beside the "
                            "program or one folder up (MOHO64_FIXES_DIR names it): the game's scripts run unfixed");
    return hooks;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path base = fs::u8path(root);
  for (auto it = fs::recursive_directory_iterator(base, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::u8string ext = it->path().extension().generic_u8string();
    if (hostfs::ToLower(std::string(ext.begin(), ext.end())) != ".lua") continue;
    const std::u8string rel = fs::relative(it->path(), base, ec).generic_u8string();
    const std::u8string host = it->path().generic_u8string();
    std::ifstream f(it->path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    hooks[Vfs::Normalize("/" + std::string(rel.begin(), rel.end()))] =
        ScriptState::ExtraHook{hostfs::DisplayPath(std::string(host.begin(), host.end())), std::move(text)};
  }
  Logf(LogLevel::Info, "moho64: the engine's script fixes are on: %zu hook files from %s", hooks.size(),
       hostfs::DisplayPath(root).c_str());
  return hooks;
}
}  // namespace

// ---- Sim -----------------------------------------------------------------------------------

Sim::Sim(Vfs* vfs, std::vector<std::string> hookDirs, std::vector<std::string> modUids)
    : vfs_(vfs), hookDirs_(std::move(hookDirs)), modUids_(std::move(modUids)),
      skeletons_(std::make_unique<SkeletonCache>(vfs)) {}

Sim::~Sim() {
  if (rec_) fclose(rec_);
  owned_.clear();  // unbind every engine object while the Lua state still exists
  ReleaseEffectObjects();
  ReleaseAnimators();
  armies_.clear();
  threads_.reset();
  state_.reset();
  // per-sim tables kept by address: last, as the teardown above may still use them
  LandNavForget(*this);
  AirForget(*this);
  FormationsForget(*this);
  MotorsForget(*this);
  ClearSimCaches();  // what was read from this sim's blueprints and map (sim/sim_cache.h)
}

lua_State* Sim::L() const { return state_ ? state_->L() : nullptr; }

Sim* Sim::From(lua_State* L) { return static_cast<Sim*>(lua_getextra(L, 1)); }

// MOHO64_DEBUG_RNG: one line per draw (tick, value, caller address relative to the image, for addr2line /
// llvm-symbolizer)
__attribute__((noinline)) void Sim::RngTrace(uint32_t r) {
  const uintptr_t where = ImageOffset(__builtin_return_address(0));
  if (rec_) {
    RecorderDraw(where);
    if (!rngTrace_) return;
  }
  Logf(LogLevel::Info, "rngdbg %u %08x %llx", tick_, r, static_cast<unsigned long long>(where));
  static const char* bt = getenv("MOHO64_DEBUG_RNGBT");
  if (bt && tick_ == static_cast<uint32_t>(atoi(bt))) {
    void* fr[12];
    int n = CaptureFrames(fr, 12);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
      s += b;
    }
    Logf(LogLevel::Info, "rngbt%s", s.c_str());
  }
}

// The sim random helpers run on the x87 at precision control 24 (core/dmath.h): every op rounds to float.
float Sim::U01() { return static_cast<float>(NextUInt32()) * 0x1p-32f; }
double Sim::FRand(float lo, float hi) {
  uint32_t u = NextUInt32();
  return dmath::MulU32(u, hi - lo) * 0x1p-32f + lo;
}
float Sim::BpUniform(float base, float range) {
  float lo = -0.0f - range;
  uint32_t u = NextUInt32();
  return (dmath::MulU32(u, range - lo) * 0x1p-32f + lo) + base;
}
double Sim::Gauss() {
  if (hasGauss_) {
    hasGauss_ = false;
    return gauss_;
  }
  float x, y, q;
  do {
    x = static_cast<float>(NextUInt32()) * 0x1p-31f - 1.0f;
    y = static_cast<float>(NextUInt32()) * 0x1p-31f - 1.0f;
    q = y * y + x * x;
  } while (!(q < 1.0f));
  float t = static_cast<float>(-2.0 * dmath::Logd(static_cast<double>(q))) / q;  // fyl2x stays wide
  float f = std::sqrt(t);
  gauss_ = y * f;
  hasGauss_ = true;
  return x * f;
}

Army* Sim::GetArmy(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TNUMBER) {
    int i = static_cast<int>(lua_tonumber(L, idx));
    return i >= 1 && i <= static_cast<int>(armies_.size()) ? armies_[i - 1].get() : nullptr;
  }
  if (lua_isstring(L, idx)) {
    const char* n = lua_tostring(L, idx);
    for (auto& a : armies_)
      if (a->name == n) return a.get();
  }
  return nullptr;
}

bool Sim::LoadRules() {
  rules_ = std::make_unique<ScriptState>(ScriptState::Kind::Rules, vfs_);
  rules_->SetHookDirs(hookDirs_);
  SimBlueprints::StartRecording(*rules_);
  SetModsGlobal(*rules_);
  return rules_->DoScript("/lua/ruleinit.lua");
}

bool Sim::RunString(const std::string& code, const std::string& chunkName) {
  return state_->DoString(code, chunkName);
}

void Sim::SetModsGlobal(ScriptState& st) {
  lua_State* L = st.L();
  int top = lua_gettop(L);
  if (sessionMods.empty() || !PushSerializedLua(L, sessionMods) || !lua_istable(L, -1)) {
    lua_settop(L, top);
    SetActiveMods(st, modUids_);
    return;
  }
  // The mounted mods (hook folders) come from the uid list: say so if it differs from the session.
  std::vector<std::string> uids;
  for (int i = 1;; ++i) {
    lua_rawgeti(L, -1, i);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_pushstring(L, "uid");
    lua_rawget(L, -2);
    uids.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
    lua_pop(L, 2);
  }
  if (uids != modUids_)
    Logf(LogLevel::Warning, "moho64: the session's %zu mods differ from the %zu mounted mods", uids.size(),
         modUids_.size());
  lua_setglobal(L, "__active_mods");
}

bool Sim::CallGlobal(const char* fn, int nargs) {
  lua_State* L = state_->L();
  int base = lua_gettop(L) - nargs;
  lua_pushcfunction(L, ScriptTraceback);
  lua_insert(L, base + 1);
  lua_pushstring(L, fn);
  lua_rawget(L, LUA_GLOBALSINDEX);
  lua_insert(L, base + 2);
  bool ok = lua_pcall(L, nargs, 0, base + 1) == 0;
  if (!ok) LogScriptError(lua_isstring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
  lua_settop(L, base);
  return ok;
}

bool Sim::Start(const ReplayHeader& replay) {
  // The sim's focus and the driver's pending one both start as the launch's (Sim::Sim 0x743a63 and the
  // CSimDriver ctor 0x73b7dc both read +0x68 of the launch info they are given): no change at the first Sync.
  requestedFocus_.store(focusArmy);
  rng_.seed(replay.seed);
  sessionSeed_ = replay.seed;
  hasGauss_ = false;
  rngTrace_ = getenv("MOHO64_DEBUG_RNG") != nullptr;
  RecorderOpen();
  cheats = replay.cheats;
  sessionScenario = replay.scenario;
  cmdSources.clear();
  for (const auto& src : replay.sources) cmdSources.push_back({src.name, src.pausesAllowed});

  // Map
  {
    std::string mapPath = replay.map;
    auto data = vfs_->ReadFile(mapPath);
    map_ = std::make_unique<TerrainMap>();
    std::string err;
    if (!data || !map_->Load(*data, &err)) {
      Logf(LogLevel::Error, "moho64: cannot load map %s: %s", mapPath.c_str(), data ? err.c_str() : "file not found");
      return false;
    }
    Logf(LogLevel::Info, "Background task \"Map loader %s\" finished.", Vfs::Normalize(mapPath).c_str());
    // the STIMap ctor 0x5779c0: the playable rect is the whole map until a script sets one
    playableRect[0] = 0;
    playableRect[1] = 0;
    playableRect[2] = map_->width();
    playableRect[3] = map_->height();
  }

  // Sim Lua state
  state_ = std::make_unique<ScriptState>(ScriptState::Kind::Sim, vfs_);
  lua_State* L = state_->L();
  state_->SetHookDirs(hookDirs_);
  if (fixes) state_->SetExtraHooks(FixesHooks());
  lua_pushlightuserdata(L, const_cast<char*>(&kSimKey));
  lua_pushlightuserdata(L, this);
  lua_rawset(L, LUA_REGISTRYINDEX);
  lua_setextra(L, 1, this);
  threads_ = std::make_unique<ThreadScheduler>(L);
  RegisterSimClasses(L);
  RegisterThreadBindings(L, threads_.get());
  RegisterSimBindings(L);
  RegisterEntityBindings(L);
  RegisterPlatoonBindings(L);
  RegisterCommandBindings(L);
  RegisterEffectBindings(L);
  RegisterEconomyBindings(L);
  RegisterBuildBindings(L);
  RegisterCollisionBindings(L);
  RegisterMotorBindings(L);
  RegisterCombatBindings(L);
  RegisterAnimBindings(L);
  RegisterScriptBitBindings(L);
  RegisterAirBindings(L);
  RegisterTransportBindings(L);
  RegisterIntelBindings(L);
  RegisterThreatBindings(L);
  RegisterBrainBindings(L);
  RegisterAiBuildBindings(L);
  SetModsGlobal(*state_);
  // The user layer's language (prefs 'options_overrides.language', default '') - set by the engine.
  lua_pushstring(L, "");
  lua_setglobal(L, "__language");
  bps_.fileExists = [vfs = vfs_](const std::string& path) { return vfs->Exists(path); };
  bps_.CopyToSim(rules_->L(), L);
  RegisterCategoryBindings(L, &bps_);
  rules_.reset();  // the rules state is done

  if (!state_->DoScript("/lua/simInit.lua")) return false;

  // ScenarioInfo from the session; ArmySetup from the armies' player options.
  if (!PushSerializedLua(L, replay.scenario) || !lua_istable(L, -1)) {
    Logf(LogLevel::Error, "moho64: bad scenario info in replay");
    return false;
  }
  lua_pushstring(L, "ArmySetup");
  lua_newtable(L);
  for (const auto& a : replay.armies) {
    if (!PushSerializedLua(L, a.options) || !lua_istable(L, -1)) {
      Logf(LogLevel::Error, "moho64: bad army options in replay");
      return false;
    }
    lua_pushstring(L, "ArmyName");
    lua_rawget(L, -2);
    lua_insert(L, -2);
    lua_rawset(L, -3);
  }
  lua_rawset(L, -3);
  lua_setglobal(L, "ScenarioInfo");

  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  auto lap = [&](const char* what) {
    auto t1 = clk::now();
    Logf(LogLevel::Debug, "moho64: %s %.2f s", what, std::chrono::duration<double>(t1 - t0).count());
    t0 = t1;
  };
  if (!CallGlobal("SetupSession", 0)) return false;
  lap("SetupSession");
  if (!CreateArmies(replay)) return false;
  lap("CreateArmies");
  CreateMapProps();  // Prop::PROP_Create in the original, then the count is logged
  Logf(LogLevel::Warning, " NUM PROPS = %zu", map_->props.size());
  lap("map props");
  if (!CallGlobal("BeginSession", 0)) return false;
  lap("BeginSession");
  SetupSync();  // the original's "beat 0" Sync, before any beat
  return true;
}

bool Sim::IsWithin(float x, float z, float m, bool wholeMap) const {
  if (wholeMap) {  // (0x62ca6e: the heightfield's size - 1, x87 sums at single precision)
    const float W = map_ ? static_cast<float>(map_->width()) : 0.0f, H = map_ ? static_cast<float>(map_->height()) : 0.0f;
    return !(0.0f > x - m) && !(0.0f > z - m) && x + m < W && z + m < H;
  }
  return !(static_cast<float>(playableRect[0]) > x - m) && !(static_cast<float>(playableRect[1]) > z - m) &&
         !(x + m > static_cast<float>(playableRect[2])) && !(z + m > static_cast<float>(playableRect[3]));
}

bool Sim::Start(const SessionDescription& session) {
  focusArmy = session.focusArmy;
  isReplay = session.replay;
  fixes = session.fixes >= 0 ? session.fixes != 0 : !session.replay;
  // The replay of a game this engine played with the fixes carries their mark (Options.RemasterFixes = "on" in its
  // scenario): it plays back with them, as it was played. The original's replays have no mark and keep them off.
  if (session.fixes < 0 && session.replay && ScenarioMarksFixes(session.header.scenario)) fixes = true;
  if (const char* f = getenv("MOHO64_FIXES")) fixes = f[0] == '1';
  return Start(session.header);
}

bool Sim::CreateArmies(const ReplayHeader& replay) {
  lua_State* L = state_->L();
  // Armies of the session, in session order.
  int saveArmies = lua_gettop(L) + 1;
  lua_pushnil(L);
  lua_getglobal(L, "ScenarioInfo");
  lua_pushstring(L, "ArmySetup");
  lua_gettable(L, -2);
  int setup = lua_gettop(L);
  for (const auto& ra : replay.armies) {
    if (!PushSerializedLua(L, ra.options)) continue;
    int opts = lua_gettop(L);
    lua_pushstring(L, "ArmyName");
    lua_rawget(L, opts);
    std::string name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    lua_pop(L, 1);  // every army of the session exists, also those the map's save lacks (probe: ARMY_9)
    auto army = std::make_unique<Army>();
    army->index = static_cast<int>(armies_.size()) + 1;
    army->name = name;
    lua_pushstring(L, name.c_str());
    lua_gettable(L, setup);  // the ArmySetup entry (shared with Lua)
    int entry = lua_gettop(L);
    lua_pushstring(L, "ArmyIndex");
    lua_pushnumber(L, army->index);
    lua_rawset(L, entry);
    auto field = [&](const char* k) {
      lua_pushstring(L, k);
      lua_rawget(L, entry);
    };
    field("PlayerName");
    army->nickname = lua_isstring(L, -1) ? lua_tostring(L, -1) : name;
    field("Faction");
    army->faction = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) - 1 : 0;
    field("Civilian");
    army->civilian = lua_toboolean(L, -1);
    field("Human");
    army->human = lua_toboolean(L, -1);
    field("AIPersonality");  // (CArmyImpl ctor 0x6fe94f: "Human" for a human, else the option)
    army->personality = army->human ? "Human" : (lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
    field("ArmyColor");
    army->armyColor = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    field("PlayerColor");
    army->playerColor = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    army->sources = ra.sources;
    lua_settop(L, setup);
    armies_.push_back(std::move(army));
  }
  lua_settop(L, saveArmies - 1);
  // The lobby's unit cap: the CArmyImpl ctor (0x6ff2ed) takes Options.UnitCap only when it is a string
  // (LuaObject::IsString 0x907370: the type itself, not a number), as atol; 500 when it is none or atol gives 0.
  // (A campaign's options have none: its armies start at 500.)
  float cap = 500;
  lua_getglobal(L, "ScenarioInfo");
  lua_pushstring(L, "Options");
  lua_gettable(L, -2);
  if (lua_istable(L, -1)) {
    lua_pushstring(L, "UnitCap");
    lua_gettable(L, -2);
    if (lua_type(L, -1) == LUA_TSTRING) {
      const long v = std::atol(lua_tostring(L, -1));
      if (v) cap = static_cast<float>(v);
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
  for (auto& a : armies_) {
    a->alliance.assign(armies_.size(), 0);
    a->unitCap = cap;
  }

  InitIntelGrids(*this, L);  // each army's recon DB (CArmyImpl ctor)
  InitThreatMaps(*this);     // and its threat map (CArmyImpl ctor 0x6feb94)
  InitBrainState(*this);     // and its no-rush timer and brain state (sim/ai_brain.cpp)

  // Brains: an instance of /lua/aibrain.lua's AIBrain bound to the engine brain.
  for (auto& a : armies_) {
    int top = lua_gettop(L);
    lua_pushcfunction(L, ScriptTraceback);
    lua_getglobal(L, "import");
    lua_pushstring(L, "/lua/aibrain.lua");
    if (lua_pcall(L, 1, 1, top + 1) != 0) {
      LogScriptError(lua_tostring(L, -1));
      lua_settop(L, top);
      return false;
    }
    lua_pushstring(L, "AIBrain");
    lua_gettable(L, -2);
    int cls = lua_gettop(L);
    GenerateArmyStart(a.get());  // the army ctor's two draws (0x6fe842), before its OnCreateArmyBrain
    auto* brain = new AiBrain;
    brain->army = a.get();
    a->brain = brain;
    brain->personality = CreateAiPersonality(*this, L);  // the brain ctor makes it (0x57a08c) and reads its data
    owned_.emplace_back(brain->personality);
    lua_newtable(L);
    lua_pushvalue(L, cls);
    lua_setmetatable(L, -2);
    BindObject(L, -1, brain);
    owned_.emplace_back(brain);
    lua_settop(L, top);
    a->pool = CreateArmyPool(*this, L, a.get());  // CArmyImpl ctor 0x6ff179 (sim/ai_platoon.cpp)
    lua_pushnumber(L, a->index);
    PushObject(L, brain);
    lua_pushstring(L, a->name.c_str());
    lua_pushstring(L, a->nickname.c_str());
    if (!CallGlobal("OnCreateArmyBrain", 4)) return false;
    lua_settop(L, top);
  }
  return true;
}

// CArmyImpl::GenerateArmyStart 0x6ffcb0 (sim_random.md 4.2): a provisional start from two draws.
void Sim::GenerateArmyStart(Army* a) {
  uint32_t cb = 0x2f4ccccc;
  float c;
  std::memcpy(&c, &cb, 4);
  float fx = dmath::MulU32(NextUInt32(), c) + 0.1f;  // x87 at PC_24: every op rounds to float
  float fz = dmath::MulU32(NextUInt32(), c) + 0.1f;
  uint32_t w = map_ ? static_cast<uint32_t>(map_->width()) : 0, h = map_ ? static_cast<uint32_t>(map_->height()) : 0;
  a->startX = dmath::MulU32(w, fx);
  a->startZ = dmath::MulU32(h, fz);
}

// The map's own props (trees, rocks, wrecks placed in the editor).
void Sim::CreateMapProps() {
  int missing = 0;
  int mapIndex = -1;
  for (const auto& mp : map_->props) {
    ++mapIndex;  // its place in the map file's list, counted with the ones that have no blueprint here
    const BlueprintInfo* bp = bps_.Find(mp.blueprint);
    if (!bp) {
      if (++missing <= 5) Logf(LogLevel::Debug, "moho64: map prop blueprint not found: %s", mp.blueprint.c_str());
      continue;
    }
    // CWldProps::Load 0x8918e0 (the sim's map props are made from its list, 0x7447cc): the nine floats as read are
    // the rows A (the prop's x, y, z axes), turned into a quaternion by MatrixToQuat 0x4eb3f0 (0x891b2c) and then
    // normalised (0x891b31..0x891be8: length of ((z^2 + y^2) + x^2) + w^2, fsqrt, above 1e-6 each part times
    // 1.0f / length, else zero), in the exe's operations. A generic conversion gave other last bits, and a prop box
    // tilted by them picks another SAT axis on a near tie (Skirmish 2158: tree group 804259698 against unit 0x21,
    // face axis 0 against edge axis 11: the original's edge wins, no OnCollision; ours split the group 5 beats early).
    const float* m = mp.rotation;
    const float A[3][3] = {{m[0], m[1], m[2]}, {m[3], m[4], m[5]}, {m[6], m[7], m[8]}};
    float qw, qv[3];
    {
      const float tr = (A[0][0] + A[1][1]) + A[2][2];
      if (tr > 0.0f) {
        const double r = std::sqrt(static_cast<double>(tr + 1.0f));  // 0x40eaf0 (fsqrt)
        const float rf = static_cast<float>(r);
        qw = static_cast<float>(r * 0.5);
        const float inv = 0.5f / rf;
        qv[0] = (A[1][2] - A[2][1]) * inv;
        qv[1] = (A[2][0] - A[0][2]) * inv;
        qv[2] = (A[0][1] - A[1][0]) * inv;
      } else {
        static const int next[3] = {1, 2, 0};  // 0xf61f1c
        int i = A[1][1] > A[0][0] ? 1 : 0;
        if (A[2][2] > A[i][i]) i = 2;
        const int j = next[i], k = next[j];
        const double r = std::sqrt(static_cast<double>((A[i][i] - (A[k][k] + A[j][j])) + 1.0f));
        const float rf = static_cast<float>(r);
        qv[i] = static_cast<float>(r * 0.5);
        const float inv = 0.5f / rf;
        qw = (A[j][k] - A[k][j]) * inv;
        qv[j] = (A[j][i] + A[i][j]) * inv;
        qv[k] = (A[i][k] + A[k][i]) * inv;
      }
    }
    Quat q{};
    {
      const float l2 = ((qv[2] * qv[2] + qv[1] * qv[1]) + qv[0] * qv[0]) + qw * qw;
      const float len = static_cast<float>(std::sqrt(static_cast<double>(l2)));  // 0x452fc0 (fsqrt), fst
      if (len > 9.99999997e-07f) {
        const float inv = 1.0f / len;
        q = {qv[0] * inv, qv[1] * inv, qv[2] * inv, qw * inv};
      }
    }
    if (Prop* p = CreateProp(state_->L(), *bp, mp.position, q, mp.scale)) {
      // (for PropEvents: a host has this one from the map file, as the file has it)
      p->mapIndex = mapIndex;
      p->viewSent = true;
      p->viewPos = mp.position;
      p->viewOri = q;
      p->viewScale[0] = mp.scale.x;
      p->viewScale[1] = mp.scale.y;
      p->viewScale[2] = mp.scale.z;
      p->viewScale[3] = 1;
    }
  }
  if (missing) Logf(LogLevel::Debug, "moho64: %d map props have no blueprint", missing);
}

void Sim::QueueDestroy(Entity* e) {
  if (e->destroyQueued) return;
  e->destroyQueued = true;
  destroyQueue_.push_back(e);
}

void Sim::QueueDestroyAfterBeat(Entity* e) { lateDestroy_.push_back(EntityRef(e)); }

void Sim::QueueLateDestroys() {
  for (uint32_t ref : lateDestroy_)
    if (Entity* e = FindEntity(ref)) QueueDestroy(e);
  lateDestroy_.clear();
}

// Entities destroyed by script: OnDestroy, then they leave the world (FA exe Entity::Destroy queues,
// Entity::OnDestroy runs the script callback later).
// The original (Sim::AdvanceBeat) drains the queue calling each entity's OnDestroy; the engine
// objects are deleted only afterwards, so an OnDestroy can still use an entity destroyed earlier in
// the same pass (a shield's OnDestroy calls Owner:SetShieldRatio after its unit's OnDestroy).
void Sim::ProcessDestroyQueue() {
  lua_State* L = state_->L();
  std::vector<Entity*> done;
  for (size_t i = 0; i < destroyQueue_.size(); ++i) {  // OnDestroy may destroy more
    Entity* e = destroyQueue_[i];
    CallMethod(L, e, "OnDestroy", 0);
    // Unit::OnDestroy 0x6a8905: the o-grid reservation is freed only for a unit that occupies ground (unit+0x560)
    if (e->kind == Entity::Kind::Unit && static_cast<Unit*>(e)->occupiesGround) GroundFreeRect(*this, static_cast<Unit*>(e));
    entities_.erase(e->handle);
    byId_.erase(e->id);
    ReleaseEntityIntel(*this, e);
    GridRemove(*this, e);
    if (e->kind == Entity::Kind::Projectile) {
      auto& v = projectiles;
      v.erase(std::remove(v.begin(), v.end(), static_cast<Projectile*>(e)), v.end());
    } else if (e->kind == Entity::Kind::Shield) {
      shields.erase(std::remove(shields.begin(), shields.end(), static_cast<ShieldEntity*>(e)), shields.end());
    } else if (e->kind == Entity::Kind::Beam) {
      beams.erase(std::remove(beams.begin(), beams.end(), static_cast<CollisionBeam*>(e)), beams.end());
    } else if (e->kind == Entity::Kind::Prop) {
      if (static_cast<Prop*>(e)->occupies) PropOccupancy(*this, static_cast<Prop*>(e), e->position, false);  // ~Prop 0x6fa05a
      if (collectProps) propsGone.emplace_back(e->id, static_cast<Prop*>(e)->mapIndex);
      props.erase(std::remove(props.begin(), props.end(), static_cast<Prop*>(e)), props.end());
      propGridDirty_ = true;
    }
    if (e->kind == Entity::Kind::Shield || e->kind == Entity::Kind::Entity)
      scriptEntities.erase(std::remove(scriptEntities.begin(), scriptEntities.end(), e), scriptEntities.end());
    if (e->kind == Entity::Kind::Unit) {
      Unit* u = static_cast<Unit*>(e);
      ReleaseUnitCombat(*this, u);
      AdjacencyLost(*this, L, u);
      UnitEconomyRelease(u);
      ReleaseStructure(*this, u);
      RemoveUnitFromLists(u);
    }
    if (e->kind == Entity::Kind::Unit && e->army) e->army->unitCost -= static_cast<Unit*>(e)->capCost;
    // the squads' EntitySets drop a purged unit (sim/ai_platoon.cpp)
    if (e->kind == Entity::Kind::Unit) PlatoonsForgetUnit(*this, static_cast<Unit*>(e));
    done.push_back(e);
  }
  destroyQueue_.clear();
  // A destroyed unit's command queue goes with the unit object: Unit::OnDestroy 0x6a88d0 (above) leaves it alone, and
  // only ~Unit 0x6a6bf0 clears it (0x6a6c66), when EntityDb::Purge 0x684560 deletes the entity in Sim::Sync, after
  // this drain. So what the tasks' dtors destroy (an unfinished upgrade, CUnitUpgradeTask dtor 0x5f84c0) is queued
  // for the NEXT beat's drain. (A killed unit's queue was cleared before, by Unit::KillCleanup in step 12.) Seen:
  // Economy 3952, T2 mex 0x3a reclaimed while upgrading: the original's T3 0x57 is still listed after that beat and
  // gone after the next; ours took it in the same drain.
  for (Entity* e : done)
    if (e->kind == Entity::Kind::Unit) ForgetUnitCommands(static_cast<Unit*>(e));
  // ~Entity in EntityDb::Purge: every kind gives its id back, recon blips too (the player's orders name blips
  // by id, and a new unit's blip takes the lowest free serial of its army's blip pool)
  for (Entity* e : done) ReleaseId(e->id);
  static const bool dbg = getenv("MOHO64_DEBUG_DESTROY") != nullptr;
  for (Entity* e : done) {
    if (dbg && e->kind == Entity::Kind::Unit && e->HasLuaObject()) {
      Unit* u = static_cast<Unit*>(e);
      char buf[160];
      std::snprintf(buf, sizeof buf, "%s of army %d, destroyed tick %u, dead %d, complete %.2f, built tick %u",
                    e->blueprint ? e->blueprint->id.c_str() : "?", e->army ? e->army->index : 0, tick_, e->dead ? 1 : 0,
                    e->fractionComplete, u->lastMaterializeTick);
      PushObject(L, e);
      lua_pushstring(L, "_moho64_destroyed");
      lua_pushstring(L, buf);
      lua_rawset(L, -3);
      lua_pop(L, 1);
    }
    e->UnbindLua();
  }
}

// One beat: the tick counter advances, then the script threads due at that tick run.
namespace {
void InsertById(std::vector<Unit*>& v, Unit* u) {
  auto it = std::lower_bound(v.begin(), v.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  v.insert(it, u);
}
void EraseById(std::vector<Unit*>& v, Unit* u) {
  auto it = std::lower_bound(v.begin(), v.end(), u, [](const Unit* a, const Unit* b) { return a->id < b->id; });
  if (it != v.end() && *it == u) v.erase(it);
}
}  // namespace

void Sim::RebuildUnitGrid() {
  int w = map_ ? (map_->width() + 15) / 16 + 1 : 64, h = map_ ? (map_->height() + 15) / 16 + 1 : 64;
  if (w != gridW_ || h != gridH_) {
    gridW_ = w;
    gridH_ = h;
    grid_.assign(static_cast<size_t>(w) * h, {});
  } else {
    for (auto& c : grid_) c.clear();
  }
  for (Unit* u : units_) {
    if (u->destroyQueued) continue;
    int cx = std::clamp(static_cast<int>(u->position.x) >> 4, 0, gridW_ - 1);
    int cz = std::clamp(static_cast<int>(u->position.z) >> 4, 0, gridH_ - 1);
    grid_[static_cast<size_t>(cz) * gridW_ + cx].push_back(u);
  }
  gridDirty_ = false;
}

static int g_unitsCreated = 0;
const std::vector<Unit*>& Sim::CommandOrder() {
  if (cmdOrderDirty_) {
    cmdOrder_ = units_;
    std::sort(cmdOrder_.begin(), cmdOrder_.end(), [](const Unit* a, const Unit* b) { return a->cmdSeq < b->cmdSeq; });
    cmdOrderDirty_ = false;
  }
  return cmdOrder_;
}
const std::vector<Unit*>& Sim::MotionOrder() {
  if (motionOrderDirty_) {
    motionOrder_ = units_;
    std::sort(motionOrder_.begin(), motionOrder_.end(), [](const Unit* a, const Unit* b) { return a->motSeq < b->motSeq; });
    motionOrderDirty_ = false;
  }
  return motionOrder_;
}
const std::vector<Unit*>& Sim::ArmyStageOrder() {
  if (armyOrderDirty_) {
    armyOrder_ = units_;
    // (an entity id holds its army above bit 20, so id order already goes army by army)
    std::sort(armyOrder_.begin(), armyOrder_.end(), [](const Unit* a, const Unit* b) {
      const uint32_t aa = a->id >> 20, ab = b->id >> 20;
      return aa != ab ? aa < ab : a->entSeq < b->entSeq;
    });
    armyOrderDirty_ = false;
  }
  return armyOrder_;
}
void Sim::ResumeCommandThread(Unit* u) {
  u->cmdSeq = ++cmdSeqNext_;
  cmdOrderDirty_ = true;
  // MOHO64_DEBUG_RESUME=<unit id> (or -2: every unit): a command thread re-linked at the end, and who did it
  static const long dbg = getenv("MOHO64_DEBUG_RESUME") ? atol(getenv("MOHO64_DEBUG_RESUME")) : -1;
  if (dbg == -2 || dbg == static_cast<long>(u->id)) {
    void* fr[6];
    const int n = CaptureFrames(fr, 6);
    std::string s;
    for (int i = 1; i < n; ++i) {
      char b[32];
      snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(ImageOffset(fr[i])));
      s += b;
    }
    Logf(LogLevel::Info, "resume %u unit %u seq %llu from%s", tick_, u->id, static_cast<unsigned long long>(u->cmdSeq),
         s.c_str());
  }
}
void Sim::AddUnitToLists(Unit* u) {
  ++g_unitsCreated;
  gridDirty_ = true;
  u->cmdSeq = ++cmdSeqNext_;
  cmdOrderDirty_ = true;
  u->entSeq = ++entSeqNext_;
  u->motSeq = u->entSeq;
  motionOrderDirty_ = true;
  armyOrderDirty_ = true;
  InsertById(units_, u);
  if (u->army) InsertById(u->army->units, u);
}
void Sim::RemoveUnitFromLists(Unit* u) {
  gridDirty_ = true;
  cmdOrderDirty_ = true;
  motionOrderDirty_ = true;
  armyOrderDirty_ = true;
  EraseById(units_, u);
  if (u->army) EraseById(u->army->units, u);
}

namespace {
// MOHO64_PROFILE=1: time spent per phase of the tick, logged every 1000 ticks.
struct PhaseTimer {
  static constexpr int kN = 12;
  const char* names[kN] = {"economy", "recon+cleanup", "commands", "collision", "motion+aim", "anim+unitecon",
                           "projectiles", "beams", "weapons", "navigators+paths", "threads", "destroy"};
  double total[kN] = {};
  std::chrono::steady_clock::time_point t;
  bool on = getenv("MOHO64_PROFILE") != nullptr;
  void Start() {
    if (on) t = std::chrono::steady_clock::now();
  }
  void Lap(int i) {
    if (!on) return;
    auto n = std::chrono::steady_clock::now();
    total[i] += std::chrono::duration<double, std::milli>(n - t).count();
    t = n;
  }
  void Report(uint32_t tick) {
    if (!on || tick % 1000 != 0) return;
    std::string s;
    char buf[64];
    for (int i = 0; i < kN; ++i) {
      std::snprintf(buf, sizeof buf, " %s %.2f", names[i], total[i] / 1000.0);
      s += buf;
      total[i] = 0;
    }
    Logf(LogLevel::Info, "moho64 profile tick %u (ms/tick):%s", tick, s.c_str());
  }
};
PhaseTimer g_prof;
}  // namespace

// Order (Sim::AdvanceBeat 0x749f40, engine-ref beat_order.md): the tick count, then per army the
// economy, navigators and path queues and weapons; then the three task stages: command threads
// (0x958, in thread order), Lua threads (0x944), entities (0x930: unit motion with each unit's
// aim controllers and own beat, then projectiles and beams, so a shot moves in the beat it is
// fired and an impact found in beat N reaches OnImpact in beat N+1's move); then recon, killed
// units' clean-up, intel coordinates and the destroy queue. Lua at tick N sees the positions of
// beat N-1's motion; callbacks raised during motion are labelled N.
// CArmyStats::Update 0x70bea0 (army_stats.md 2.3): after the army's economy pass, from tick 11 on
void EvaluateStatTriggers(Sim& sim, Army& a) {
  std::vector<std::shared_ptr<Army::StatTrigger>> fired;
  for (auto it = a.statTriggers.begin(); it != a.statTriggers.end();) {
    const Army::StatTrigger& t = **it;
    bool ok = !t.conds.empty();
    for (const Army::StatCond& c : t.conds) {
      float v = 0;
      if (!c.category) {
        auto f = a.stats.find(c.stat);
        v = f != a.stats.end() ? f->second : 0.0f;
      }
      switch (c.op) {
        case 0: ok = v > c.thr; break;
        case 1: ok = v >= c.thr; break;
        case 2: ok = c.thr > v; break;
        case 3: ok = c.thr >= v; break;
        default: break;
      }
      if (!ok) break;
    }
    if (ok) {
      fired.push_back(*it);
      it = a.statTriggers.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& t : fired) {
    if (!a.brain || !a.brain->HasLuaObject()) continue;
    lua_State* L = sim.L();
    lua_pushstring(L, t->name.c_str());
    sim.CallMethod(L, a.brain, "OnStatsTrigger", 1);
  }
}

bool Sim::OkayToMessWith(const Army* a) const {
  if (!a) return cheats;
  if (a->outOfGame) return false;  // army+0x1c0, even with cheats
  if (commandSource != 0xff && std::find(a->sources.begin(), a->sources.end(), commandSource) != a->sources.end())
    return true;
  return cheats;  // CheatsEnabled() (also feeds the beat checksum in the original)
}

// The script part of Sim::Sync (0x747b11): the engine's own fields go into the scripts' global Sync (who paused
// and how many pauses that source has left, the focus army's statistics), the table is handed over as bytes, and
// the scripts' ResetSyncTable() empties it for the next beat. (Not built: Sync.Cheaters, the sources that used a
// cheat this beat, and the statistics' Children: only the debug window /lua/debug/ArmyStats.lua reads them.)
// keep: the bytes go to pendingSync_ for TakeSync (collectSync for a beat, always for the setup's Sync).
void Sim::SyncStep(bool keep) {
  lua_State* L = state_->L();
  const int top = lua_gettop(L);
  lua_pushstring(L, "Sync");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (lua_istable(L, -1)) {
    const int sync = lua_gettop(L);
    const bool paused = pauseOwner >= 0 && pauseOwner < static_cast<int>(cmdSources.size());
    lua_pushstring(L, "PausedBy");
    if (paused) lua_pushnumber(L, pauseOwner + 1);
    else lua_pushnil(L);
    lua_rawset(L, sync);
    lua_pushstring(L, "TimeoutsRemaining");
    if (paused) lua_pushnumber(L, cmdSources[pauseOwner].pausesLeft);
    else lua_pushnil(L);
    lua_rawset(L, sync);
    if (focusArmy >= 0 && focusArmy < static_cast<int>(armies_.size())) {
      lua_pushstring(L, "__ArmyStats");
      lua_newtable(L);
      lua_pushstring(L, "Tick");
      lua_pushnumber(L, tick_);
      lua_rawset(L, -3);
      lua_rawset(L, sync);
    }
    if (keep) {
      pendingSync_.emplace_back();
      SerializeLua(L, sync, &pendingSync_.back());
    }
  }
  lua_settop(L, top);
  lua_pushstring(L, "ResetSyncTable");
  lua_rawget(L, LUA_GLOBALSINDEX);
  if (lua_isfunction(L, -1)) {
    lua_pushcfunction(L, ScriptTraceback);
    lua_insert(L, -2);
    if (lua_pcall(L, 0, 0, top + 1) != 0) LogScriptError(lua_tostring(L, -1) ? lua_tostring(L, -1) : "ResetSyncTable");
  }
  lua_settop(L, top);
}

// The start of Sim::Sync (0x7474b0): SimDriver::FinalizeSyncDispatchLocked (0x73dad0) has copied the driver's
// sync filter, whose first field is the pending focus (SimDriver+0xb0: SetArmyIndex 0x73b1b0, FAF's sim
// SetFocusArmy 0x128ce81), and Sim::Sync copies it into the sim's own at once (sim+0xa88, 0x7474ef). Only then,
// when the old focus differs, the scripts' global NoteFocusArmyChanged(new, old) is called (0x747504..0x747562,
// LuaFunction::Call_Int2: each 1-based, -1 stays -1), so GetFocusArmy answers the new army inside it (FAF's
// SimPing.lua and Recall.lua OnArmyChange ask it); an error there is only logged, "NoteFocusArmyChanged()
// puked:\n%s" (Warnf, 0x74757f). After it the sync packet takes the focus (+4, 0x74762c) and the script Sync
// table is handed over (0x747b11: SyncStep). (Also on a change, not built: every entity's sync data and every
// command's are sent in full, not only the changed ones (0x7478aa, CCommandDb::PublishSyncData 0x6e0f50): our
// hosts copy the whole state every beat. And each army's "seen by the focus army" flag, army+0x128, is set from
// it in CArmyImpl::OnTick 0x6ffdaa (SetCanSee 0x6fdee0): read by nothing of ours.)
void Sim::ApplyFocusArmy() {
  const int next = requestedFocus_.load();
  const int old = focusArmy;
  focusArmy = next;
  if (next == old) return;
  lua_State* L = state_->L();
  const int top = lua_gettop(L);
  lua_pushcfunction(L, ScriptTraceback);
  lua_pushstring(L, "NoteFocusArmyChanged");
  lua_rawget(L, LUA_GLOBALSINDEX);
  lua_pushnumber(L, next == -1 ? -1 : next + 1);
  lua_pushnumber(L, old == -1 ? -1 : old + 1);
  if (lua_pcall(L, 2, 0, top + 1) != 0)
    Logf(LogLevel::Warning, "NoteFocusArmyChanged() puked:\n%s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  lua_settop(L, top);
}

std::vector<std::string> Sim::TakeSync() {
  std::vector<std::string> out;
  out.swap(pendingSync_);
  return out;
}

// The setup's own Sync, the original's "beat 0" (ui REQUESTS 33). The sim thread, CSimDriver::ThreadCreateSim
// 0x73d260, makes the sim (0x73d4a0: FAF 0x129142c -> Sim::Create 0x7433b0; simInit.lua, SetupSession, the armies,
// BeginSession with the map's OnPopulate and OnStart) and then, before its beat loop and before any command, runs
// one whole Sync: FinalizeSyncDispatchLocked at 0x73d68d (0x73dad0 -> Sim::Sync 0x7474b0). Its packet carries beat
// number 0 (+0 from sim+0x8f8, 0x74760a), the focus (+4, 0x74762c) and game tick 0 (+8 from sim+0x900, 0x747616: no
// beat has run), every army's constant data (only in this first Sync: the flag sim+0xa6c, 0x747788..0x747815) and
// variable data, the entities and commands the setup made, and the script Sync table with what the setup wrote
// (LuaObject::ToByteStream 0x747e95, then SCR_LuaDoString "ResetSyncTable()" 0x747eae); then sim+0x8f8 becomes 1
// (0x748318), the beat number beat 1 runs with, and sim+0x8dd takes sim+0x8dc. The user side runs it as its beat 0:
// CWldSession::DoInitializing 0x88c3f0 -> DoBeat 0x88c507 (the scripts' OnSync), before the game's screen is made.
// Of what this Sync does, the simulation reads back only a focus the setup's scripts changed (FAF's sim
// SetFocusArmy in simInit.lua's MergeArmyPair: NoteFocusArmyChanged runs here, before beat 1, as in the original)
// and the id quarantine's turn (EntityDb::Purge 0x747af9), which changes no id's wait: no id is released before
// the first beat, and a released id waits the same number of turns whatever the ring's start.
void Sim::SetupSync() {
  ApplyFocusArmy();
  UpdateIdPools();
  SyncStep(true);  // kept whatever collectSync says (sim.h, TakeSync)
  frozen = endGame;
}

void Sim::PausedBeat() {
  // AdvanceBeat steps 15-19 run on paused beats too (beat_order.md 1), then Sim::Sync (EntityDB purge)
  if (rec_) RecorderBeat();
  ProcessDestroyQueue();
  QueueLateDestroys();
  ApplyFocusArmy();  // Sim::Sync's start
  UpdateIdPools();
  SyncStep(collectSync);
  frozen = endGame;  // Sync copies sim+0x8dc to sim+0x8dd
}

void Sim::Tick() {
  if (rec_) RecorderBeat();  // labhook's AdvanceBeat entry: the state after the previous tick
  beatStarted_ = true;
  ++tick_;
  g_crashTick = tick_;  // (named by a crash's log line, core/crash.h)
  g_prof.Start();
  {  // debug: MOHO64_DEBUG_RNGSHIFT=<tick>:<n> discards n raw draws at the start of that tick. When the Lua
     // stream's counts match the oracle but its values do not from tick T on, the n that makes the probe's
     // rngl hash at T match is how many engine draws we miss before T.
    static const char* sh = getenv("MOHO64_DEBUG_RNGSHIFT");
    if (sh) {
      unsigned t = 0;
      int n = 0;
      if (sscanf(sh, "%u:%d", &t, &n) == 2 && t == tick_)
        for (int i = 0; i < n; ++i) NextUInt32();
    }
  }
  LandNavBackground(*this);  // AdvanceBeat step 3: path tables' background build
  // the armies' stage: economy, navigators and path queues, weapons (acquire, fire)
  EconomyBeginBeat(*this);
  // CArmyImpl::OnTick 0x6ffe11, after the army's economy and stats: the army whose slot it is (tick % 30) sums its
  // threat map. It reads only the blips (changed at the beat's end) and writes only the map, so doing it here for
  // that army rather than between the armies' other stages changes nothing (inferred from what it reads).
  ThreatMapBeat(*this);
  threads_->WakeSignalled();  // (a thread whose event a stage signalled goes to the run order's end at that moment)
  g_prof.Lap(0);
  LandNavTickAll(*this);
  g_prof.Lap(9);
  // CArmyImpl::OnTick 0x6ffd70: the brain's stages in this order: navigators (brain+0xa8), steering (brain+0xac,
  // CAiSteeringImpl::OnTick 0x5d32b0), attackers and weapons (brain+0xb0); only then the sim's command stage. So
  // a waypoint the navigator set this beat already has its spline when a command task stops the unit in the same
  // beat, and the stop turns it into a brake spline (the unit slows down along its hull) instead of a coast.
  SteeringTickAll(*this);  // the steering stage (spline points, predicted collisions)
  g_prof.Lap(3);
  WeaponsTick(*this);
  threads_->WakeSignalled();
  g_prof.Lap(8);
  CommandStage(*this);  // stage 0x958 (arrivals from last beat's motion end their commands here)
  g_prof.Lap(2);
  threads_->RunTick(tick_);  // stage 0x944: Lua threads
  g_prof.Lap(10);
  std::vector<Unit*> motionOrder = MotionOrder();  // (a copy: a unit sent to the pass's end is appended to it)
  // The entity stage runs its tasks in creation order, units and collision beams alike, and a unit's task is its
  // whole beat (Unit::MotionTick 0x6a9010: motion, manipulators, regeneration, its economy). A collision beam is
  // made with its weapon, so it comes after its owner and before every unit made later: such a unit, killed by the
  // beam, is dead BEFORE its own beat - it does not move or turn in it and takes no resources; a unit older than
  // the beam has had its beat. So the beams take their turn here, between the units, by creation handle. (Seen: a
  // destroyer whose target a beam of an older ship kills stops turning in that very beat; and an experimental
  // that earns a rank with its own beam regenerates at the old rate that beat.)
  const std::vector<CollisionBeam*> entityBeams = beams;
  size_t entityBeamAt = 0;
  auto beamsBefore = [&](uint32_t handle) {
    while (entityBeamAt < entityBeams.size() && entityBeams[entityBeamAt]->handle < handle)
      BeamTick(*this, entityBeams[entityBeamAt++]);
  };
  for (size_t i = 0; i < motionOrder.size(); ++i) {
    Unit* u = motionOrder[i];
    beamsBefore(u->handle);
    // Entity::TaskTick 0x679c40, step 1 (0x679c51): the texture scroller, before the attached test below, so a
    // unit sent to the pass's end takes that step twice in that beat. It reads the committed transform and the
    // one before it: here the unit's transform before this turn's motion and lastPosition / lastOrientation, which
    // this turn has not taken yet (on a unit's first beat the two are the same: the unit ctor advances its coords
    // twice, 0x6a5c59 / 0x6a5c5f).
    if (u->scroller) {
      const bool had = u->lastPosTick != 0;
      TextureScrollerTick(u, u->position, u->orientation, had ? u->lastPosition : u->position,
                          had ? u->lastOrientation : u->orientation);
      // MOHO64_DEBUG_SCROLL=<unit id decimal | -2 all>: the offsets after each step
      static const long dbgScroll = getenv("MOHO64_DEBUG_SCROLL") ? atol(getenv("MOHO64_DEBUG_SCROLL")) : -1;
      if (dbgScroll == -2 || dbgScroll == static_cast<long>(u->id))
        Logf(LogLevel::Info, "scroll %u unit %u type %d: %.9g %.9g -> %.9g %.9g", tick_, u->id,
             static_cast<int>(u->scroller->type), u->scrollPrev[0], u->scrollPrev[1], u->scroll[0], u->scroll[1]);
    }
    // (A unit whose Destroy is queued still has its turn: the entity is only taken away at the beat's end. Seen: a
    // dead aircraft that the flight code had just taken back starts its crash, three draws, in the beat its script
    // destroys it.)
    // Entity::TaskTick 0x679c40, step 2: attached to a parent that has not had its turn in this pass, the task
    // answers -4: the thread is linked at the run list's new end, runs later in this pass, and keeps that place.
    if (u->parentId && u->orderedParent != u->parentId) {
      u->orderedParent = u->parentId;
      if (const Entity* p = FindEntity(u->parentId)) {
        const bool parentLater = p->kind == Entity::Kind::Unit
                                     ? static_cast<const Unit*>(p)->lastPosTick != tick_
                                     : p->handle > (u->orderHandle ? u->orderHandle : u->handle);
        if (parentLater) {
          u->motSeq = ++entSeqNext_;
          u->orderHandle = nextHandle_;
          motionOrderDirty_ = true;
          motionOrder.push_back(u);
          continue;
        }
      }
    }
    // (lastPosition = the entity's coords before this beat's motion. On a unit's first beat that is where it was
    // CREATED: an attach in the same beat, a factory putting its new unit on the build bone, only writes the
    // pending transform, so the entity "moved" in its first AdvanceCoords and position != previous position for
    // one beat. CUnitRepairTask 0x5f946f ends on that: an assist begun in the unit's first beat is started again
    // a beat later. Transports replay, tick 364.)
    if (u->lastPosTick) {
      u->prevLastPosition = u->lastPosition;
      u->lastPosition = u->position;
    } else {
      u->prevLastPosition = u->lastPosition;
    }
    u->prevLastOrientation = u->lastPosTick ? u->lastOrientation : u->orientation;
    u->lastOrientation = u->orientation;
    u->lastPosTick = tick_;
    PurgeShooters(*this, u);
    HoldShape(u);
    // Unit::MotionTick 0x6a906e: the stun's countdown (u+0x29c, while > 0), every unit, after UpdateInfoCache and
    // before CUnitMotion::MotionTick (neither the guard formation nor the info cache reads it): a unit stunned in a
    // beat after its own turn (a Loyalist's death stun) is held for (int)(seconds * 10) of its turns, and its weapons,
    // which run before the entity stage, fire again in the beat after the count reached 0.
    if (u->stunned > 0) --u->stunned;
    if (!u->beingBuilt) FuelTick(*this, u);  // CUnitMotion::ProcessFuelLevels (attached units too)
    if (u->parentId && u->attachFull) {  // transport cargo: after every unit moved
      UnitEconomyTick(*this, u);
      continue;
    }
    if (!u->guarders.empty() && !u->guardForm) UpdateGuardFormation(*this, u);  // MotionTick order
    UpdateInfoCache(*this, u);  // formation fields and the speed cap
    const Quat prevOri = u->orientation;
    // A parent that no longer exists holds nothing (AttachParentGone): the unit goes back to its own motion on the
    // Air layer, although it still shows state Attached. Seen: a dead aircraft sinking on FAF's sinker projectile;
    // when the projectile is gone at the sea floor the wreck is in the Air layer for a beat, and the beat after
    // it starts a crash (three draws) before the script's Destroy comes through.
    Entity* const attachParent = u->parentId ? FindEntity(u->parentId) : nullptr;
    {
      // MOHO64_DEBUG_ATTACHED=<unit id>: what holds an attached unit each beat
      static const long dbgAtt = getenv("MOHO64_DEBUG_ATTACHED") ? atol(getenv("MOHO64_DEBUG_ATTACHED")) : -1;
      if (dbgAtt >= 0 && static_cast<long>(u->id) == dbgAtt)
        Logf(LogLevel::Info, "attached %u unit %u (made %llu%s) parent ref %u: %s%s, full %d, layer %s, dead %d, pos %.9g %.9g %.9g",
             tick_, u->id, static_cast<unsigned long long>(u->entSeq), u->destroyQueued ? ", destroy queued" : "",
             u->parentId, !u->parentId ? "none" : attachParent ? "there" : "gone",
             attachParent && attachParent->destroyQueued ? " (destroy queued)" : "", u->attachFull ? 1 : 0,
             u->layer.c_str(), u->dead ? 1 : 0, u->position.x, u->position.y, u->position.z);
    }
    if (u->parentId && attachParent) {  // attached (a factory's product): held at the parent's bone
      Entity* p = attachParent;
      if (p && !p->destroyQueued) {  // the child takes the bone's transform (Entity attach)
        Vec3 bp;
        Quat bq;
        BoneWorld(p, u->parentBone, &bp, &bq);
        // bone -1 of a unit is half its SizeY straight above its origin, in the world's axes, not the unit's
        // (Unit::GetBoneWorldTransform 0x6aa637 composes the offset as the outer transform): a drone's guard dummy,
        // AttachTo(drone, -1), rides there. (Seen: a UEF construction drone banking 0.004; the original's dummy
        // keeps the drone's x to the last digit, ours, turning the offset, slid 0.001 aside.)
        if (u->parentBone == -1 && p->kind == Entity::Kind::Unit) {
          const Unit* pu = static_cast<const Unit*>(p);
          if (pu->motion.bp) bp = vm::Add(bp, Vec3{0.0f, pu->motion.bp->sizeY * 0.5f, 0.0f});
        }
        Vec3 np = bp;
        Quat nq = bq;
        // The unit hangs by its OWN bone (Entity::CalculateAttachedTransform 0x67a020: the parent bone's transform
        // times the inverse of the child's own bone's, Unit::GetBoneLocalTransform 0x6aa440): AttachBoneTo's first
        // argument, 0 for AttachTo; a bone of its skeleton is taken from its pose, -1 is half its SizeY above its
        // origin, anything else (FAF's factories attach their product with -2) is the unit's origin itself
        // (0x6aa440: a bone outside the skeleton other than -1 gives the identity). The transform is still made for
        // -2: 0x67a020 ends with the normalising 0x4edaa0 whatever the bone, so a factory's product has the build
        // bone's quaternion brought to unit length. (Seen: bench, a Seraphim factory's engineer 0x200002, all four
        // components one float step below the original's from its first beat, 256, and normalised = equal.)
        // So a wreck taken at bone 0 by FAF's sinker stays where it was, and an aircraft a carrier builds sits
        // with its root bone on the carrier's attach bone. On something that is no unit it also takes the
        // parent's layer (CUnitMotion::MotionTick 0x6ba1ad: SetLayer(parent's), every beat; a parent UNIT's only
        // when the child is not what that unit works on, 0x6ba1a2, which is left to the transport code here).
        const bool onProjectile = p->kind == Entity::Kind::Projectile;
        {
          Vec3 cp;
          Quat cq;
          AttachedTransform(u, bp, bq, &np, &nq, &cp, &cq);
          static const long dbgAtt2 = getenv("MOHO64_DEBUG_ATTACHED") ? atol(getenv("MOHO64_DEBUG_ATTACHED")) : -1;
          if (dbgAtt2 >= 0 && static_cast<long>(u->id) == dbgAtt2)
            Logf(LogLevel::Info,
                 "attached %u   on shell %u bone %d (skeleton %d) own bone %d: P %.9g %.9g %.9g q %.9g %.9g %.9g %.9g | C %.9g %.9g "
                 "%.9g q %.9g %.9g %.9g %.9g | out %.9g %.9g %.9g q %.9g %.9g %.9g %.9g | posed %d base %.9g %.9g %.9g",
                 tick_, p->id, u->parentBone, p->skeleton ? p->skeleton->Count() : -1, u->ownBone, bp.x, bp.y, bp.z, bq.x,
                 bq.y, bq.z, bq.w, cp.x, cp.y, cp.z, cq.x, cq.y, cq.z, cq.w, np.x, np.y, np.z, nq.x, nq.y, nq.z, nq.w,
                 u->poseSet ? 1 : 0, u->poseBasePos.x, u->poseBasePos.y, u->poseBasePos.z);
        }
        u->position = np;
        u->orientation = nq;
      }
      u->motion.vel = {};
    } else if (u->parentId) {
      AttachParentGone(*this, u);  // (the transform stays for this beat; the motion takes the unit from the next)
      u->motion.vel = {};
    } else {
      MotionTick(*this, u);
      // CUnitMotion::MoveTo 0x6b92e0 (after every CalcMove*) first brings the transform's quaternion back to unit
      // length (0x4edaa0). Without it a hovering unit's orientation, re-tilted from itself every beat, creeps off.
      // (Not for aircraft: AirMotionTick has done its own MoveTo; a third normalising after the rotation step's and
      // that one moves the last bits back: Transports replay, tick 63.)
      if (u->motion.bp && u->motion.bp->mobile() && !u->beingBuilt && u->motion.bp->motionType != kMotionAir) {
        Quat& q = u->orientation;
        const float len = std::sqrt(((q.w * q.w + q.x * q.x) + q.y * q.y) + q.z * q.z);
        if (len > 9.99999997e-07f) {
          const float inv = 1.0f / len;
          q.w *= inv;
          q.x *= inv;
          q.y *= inv;
          q.z *= inv;
        }
      }
    }
    if (u->position.x != u->lastPosition.x || u->position.y != u->lastPosition.y ||
        u->position.z != u->lastPosition.z)
      u->lastMoveTick = tick_;
    // CAniActor::UpdateManipulators 0x63aa80, after CUnitMotion::MotionTick (0x6a908c before 0x6a90d5): the pose
    // is built on the pending transform this motion just wrote; the aim bone is read from last beat's pose
    PoseAdvance(u);
    if (!u->builderArms.empty()) {
      AimReadLastPose(*this, u);  // (the arms' step writes the bones a gun may share with them)
      BuilderArmsTick(*this, u, u->lastPosition, prevOri);
    }
    UnitAimTick(*this, u);
    // the unit's own beat: economy events, regeneration or decay, consumption and production (health regen /
    // decay and HandleResourceManagement 0x6aaac0, after the manipulators)
    UnitEconomyTick(*this, u);
    if (u->destroyQueued) continue;
    if (u->isFactoryBuilder) ValidateFactoryCommandQueue(*this, u);  // end of Unit::MotionTick
    // ... and a pod with an empty queue orders itself to assist (0x6a921a; pod_assist.md 1)
    if (u->isPod && u->commands.empty() && !u->dead) IssuePodAssist(*this, u);
  }
  beamsBefore(0xffffffffu);  // the beams younger than every unit
  threads_->WakeSignalled();
  // transport cargo follows its attach bone (Entity::TaskTick: after the parents moved)
  if (anyAttached) {
    AttachedUnitsTick(*this);
    anyAttached = false;
    for (size_t i = 0; i < units_.size(); ++i) {
      Unit* u = units_[i];
      if (u->destroyQueued || !u->transportedBy) continue;
      anyAttached = true;
      if (u->parentId && u->attachFull) {
        PoseAdvance(u);
        UnitAimTick(*this, u);
      }
    }
  }
  // plain entities attached to a bone (Entity:AttachTo / AttachBoneTo) take its transform now, after the units
  // moved; a unit hanging on one read it before this, so it is a tick behind
  if (!attachedEntities.empty()) {
    size_t keep = 0;
    for (size_t i = 0; i < attachedEntities.size(); ++i) {
      Entity* e = FindEntity(attachedEntities[i]);
      if (!e || e->destroyQueued || !e->attachParent) continue;
      Entity* p = FindEntity(e->attachParent);
      if (!p || p->destroyQueued) continue;
      HoldShape(e);
      BoneWorld(p, e->attachBone, &e->position, &e->orientation);
      if (p->kind == Entity::Kind::Projectile) {
        // On a shell (FAF's torpedo decoy and missile flare ride on one): the shell's transform as the beat began
        // (what is committed; its step of this beat is not), and bone -1 is its centre, half its SizeY above its
        // origin. Seen in the original's shell lines: two torpedoes turn onto their decoys in the beat they come
        // within the decoy's radius of where its shell was two beats before, measured from that centre.
        e->position = p->shapeHeld ? p->shapePos : p->position;
        e->orientation = p->shapeHeld ? p->shapeOri : p->orientation;
        if (e->attachBone == -1 && p->blueprint) e->position = vm::Add(e->position, vm::RotateM(e->orientation, p->bpCenter));
      }
      // bone -1 of a unit is half its SizeY straight above its origin, in the world's axes (Unit::GetBoneWorldTransform
      // 0x6aa637): a shield bubble hangs on that (seen in the original: a Fatboy's shield centre is 1.5 above its
      // origin before the ShieldVerticalOffset)
      if (e->attachBone == -1 && p->kind == Entity::Kind::Unit) {
        const Unit* pu = static_cast<const Unit*>(p);
        if (pu->motion.bp) e->position = vm::Add(e->position, Vec3{0.0f, pu->motion.bp->sizeY * 0.5f, 0.0f});
      }
      // Entity:SetParentOffset 0x68c330: its transform relative to the bone (entity+0x19c: no rotation, the vector)
      if (e->attachOffset.x != 0 || e->attachOffset.y != 0 || e->attachOffset.z != 0)
        e->position = vm::Add(e->position, vm::RotateM(e->orientation, e->attachOffset));
      attachedEntities[keep++] = attachedEntities[i];
    }
    attachedEntities.resize(keep);
  }
  MotorsTick(*this);  // the entity stage: props with a motor (falling trees, sinking)
  g_prof.Lap(4);
  AnimTick(*this);
  threads_->WakeSignalled();
  gridDirty_ = true;
  g_prof.Lap(5);
  g_prof.Lap(7);
  // Projectiles move after every task stage (Sim::AdvanceBeat runs the three task stages first), so a shot
  // moves in the beat it was fired, and Lua sees the move next beat.
  ProjectilesTick(*this);
  // A unit that hangs on a shell (a wreck on FAF's sinker) takes the shell's layer (0x6ba1ad), and it does so
  // after the shell's step of this beat: a unit attached to a parent that has not had its turn takes its own turn
  // behind the parent's (Entity::TaskTick 0x679c40), and a layer is no committed value. The layer is read from
  // where the shell is: above the water Air, else Water. (Ours keeps a projectile's own layer from where it was
  // made until it crosses the surface in flight; the sinker is made beats before it is warped under the wreck.
  // Seen: a ship's wreck taken 0.022 above the water goes to Air in the beat it is attached and back to Water in
  // the beat the sinker goes under; an aircraft's wreck lying on the surface stays in Water.)
  if (map_ && map_->hasWater)
    for (size_t i = 0; i < units_.size(); ++i) {
      Unit* u = units_[i];
      if (u->destroyQueued || !u->parentId) continue;
      const Entity* p = FindEntity(u->parentId);
      if (!p || p->destroyQueued || p->kind != Entity::Kind::Projectile) continue;
      const char* pl = p->position.y > map_->waterElevation ? "Air" : "Water";
      if (u->layer != pl) SetAttachedUnitLayer(*this, u, pl);
    }
  g_prof.Lap(6);
  ReconBeat(*this);  // army (tick % armies) updates its blips
  {
    // Step 11 (the formation DB) runs before step 13 (Entity::AdvanceCoords): motion only wrote each entity's
    // PENDING transform, so the formation update still sees every unit where it stood before this beat's
    // motion (the oracle recorder: the original's slot point at tick t = ours at t-1 with post-motion positions).
    std::vector<std::pair<Vec3, Quat>> moved;
    std::vector<Unit*> who;
    for (Unit* u : units_)
      if (u->lastPosTick == tick_ && !u->destroyQueued) {
        who.push_back(u);
        moved.emplace_back(u->position, u->orientation);
        u->position = u->lastPosition;
        u->orientation = u->lastOrientation;
      }
    FormationsTick(*this);  // step 11: the formation DB
    for (size_t i = 0; i < who.size(); ++i) {
      who[i]->position = moved[i].first;
      who[i]->orientation = moved[i].second;
    }
  }
  KillCleanupTick(*this);
  for (Unit* u : units_) u->motion.velCommitted = u->motion.lastMove;  // step 13: previous = committed = pending
  // (the same for a shell that did not move in this beat, its impact beat for one: its velocity, committed -
  // previous, is zero from here on; seen in the original's per-beat shell log)
  for (Projectile* p : projectiles)
    if (p->lastMoveTick != tick()) p->prevPos = p->position;
  for (Entity* e : heldShapes_) e->shapeHeld = false;  // (vt+0x84: the collision shapes follow)
  heldShapes_.clear();
  GridAdvanceCoords(*this);    // step 13: moved units take their (widened) cells in the entity grid
  MotorsAdvanceCoords(*this);  // and moved props theirs
  AdvanceIntelCoords(*this);  // moved intel sources move their circles
  g_prof.Lap(1);
  ProcessDestroyQueue();
  QueueLateDestroys();
  ApplyFocusArmy();  // Sim::Sync's start: a requested focus army, NoteFocusArmyChanged
  UpdateIdPools();  // EntityDb::Purge (Sim::Sync)
  SyncStep(collectSync);
  singleStep = false;  // step 14 (sim+0x8e4 = 0)
  frozen = endGame;    // Sync copies sim+0x8dc to sim+0x8dd
  g_prof.Lap(11);
  g_prof.Report(tick_);
  static const bool stats = getenv("MOHO64_STATS") != nullptr;
  if (stats && tick_ % 600 == 0) {  // a quick health line per game minute
    int n = 0, air = 0, flying = 0, dead = 0;
    for (Unit* u : units_) {
      if (u->destroyQueued) continue;
      ++n;
      if (u->dead) ++dead;
      if (u->motion.bp && u->motion.bp->motionType == kMotionAir) {
        ++air;
        if (u->layer == "Air") ++flying;
      }
    }
    Logf(LogLevel::Info, "moho64 stats tick %u: units %d (aircraft %d, flying %d, dying %d), created %d, projectiles %zu", tick_,
         n, air, flying, dead, g_unitsCreated, projectiles.size());
  }
}

// EntityDB::DoReserveId 0x684480: the lowest free serial, else a new one
uint32_t Sim::ReserveId(Army* army, uint32_t family) {
  uint32_t a = army ? static_cast<uint32_t>(army->index - 1) : 0xffu;
  uint32_t key = ((family << 8) | a) << 20;
  IdPool& p = idPools_[key];
  uint32_t s;
  if (!p.free.empty()) {
    s = *p.free.begin();
    p.free.erase(p.free.begin());
  } else {
    s = p.next++;
  }
  return key | s;
}
// EntityDB::ReleaseId 0x684690: into the newest quarantine slot
void Sim::ReleaseId(uint32_t id) {
  auto it = idPools_.find(id & 0xFFF00000u);
  if (it == idPools_.end()) return;
  IdPool& p = it->second;
  p.ring[(p.tail + 99) % 100].push_back(id & 0xFFFFFu);
}
// IdPool::Update 0x403a30, once per beat (EntityDb::Purge)
void Sim::UpdateIdPools() {
  for (auto& [key, p] : idPools_) {
    if ((p.tail + 1) % 100 == p.head) {
      for (uint32_t s : p.ring[p.head]) p.free.insert(s);
      p.ring[p.head].clear();
      while (p.next > 0 && p.free.count(p.next - 1)) {
        p.free.erase(p.next - 1);
        --p.next;
      }
      p.head = (p.head + 1) % 100;
    }
    p.ring[p.tail].clear();
    p.tail = (p.tail + 1) % 100;
  }
}

void Sim::RebuildPropGrid() {
  if (gridW_ == 0) RebuildUnitGrid();
  propGrid_.assign(static_cast<size_t>(gridW_) * gridH_, {});
  for (Prop* p : props) {
    if (p->destroyQueued) continue;
    int cx = std::clamp(static_cast<int>(p->position.x) >> 4, 0, gridW_ - 1);
    int cz = std::clamp(static_cast<int>(p->position.z) >> 4, 0, gridH_ - 1);
    propGrid_[static_cast<size_t>(cz) * gridW_ + cx].push_back(p);
  }
  propGridDirty_ = false;
}

EntityGrid& Sim::entityGrid() {
  if (!egrid_) egrid_ = std::make_unique<EntityGrid>();
  if (!egrid_->ready() && map_) egrid_->Init(map_->width(), map_->height());
  return *egrid_;
}

Navigation& Sim::navigation() {
  if (!nav_) {
    nav_ = std::make_unique<Navigation>(map_.get());
    // blocking terrain types (TerrainTypes.lua: Blocking = true)
    lua_State* L = state_->L();
    int top = lua_gettop(L);
    PushTerrainTypes(L);
    if (lua_istable(L, -1)) {
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        if (lua_isnumber(L, -2) && lua_istable(L, -1)) {
          lua_pushstring(L, "Blocking");
          lua_rawget(L, -2);
          if (lua_toboolean(L, -1)) nav_->SetBlockingTerrainType(static_cast<int>(lua_tonumber(L, -3)), true);
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      }
    }
    lua_settop(L, top);
  }
  return *nav_;
}

}  // namespace moho
