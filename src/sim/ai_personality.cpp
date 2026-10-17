#include "sim/ai_personality.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "sim/sim.h"

namespace moho {
namespace {

// The getters of the ranges, in the template's order (the index is the pair's).
const char* const kRangeGetters[AiPersonality::kRanges] = {
    "GetArmySize", "GetPlatoonSize", "GetAttackFrequency", "GetRepeatAttackFrequency", "GetCounterForces",
    "GetIntelGathering", "GetCoordinatedAttacks", "GetExpansionDriven", "GetTechAdvancement", "GetUpgradesDriven",
    "GetDefenseDriven", "GetEconomyDriven", "GetFactoryTycoon", "GetIntelBuildingTycoon", "GetSuperWeaponTendency",
    "GetAirUnitsEmphasis", "GetTankUnitsEmphasis", "GetBotUnitsEmphasis", "GetSeaUnitsEmphasis",
    "GetSpecialtyForcesEmphasis", "GetSupportUnitsEmphasis", "GetDirectDamageEmphasis", "GetInDirectDamageEmphasis",
    "GetSurvivalEmphasis", "GetTeamSupport", "GetFormationUse", "GetTargetSpread",
    "GetQuittingTendency", "GetChatFrequency"};

// Where the template's entries go: 1 name, 2 chat personality, 3..17 fifteen pairs, 18 favourite structures,
// 19..26 eight pairs, 27 favourite units, 28..33 six pairs.
const int kEntries = 33, kFavStructures = 18, kFavUnits = 27;

bool EqualNoCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
  return *a == *b;
}

// The favourite lists (0x5b81d6 / 0x5b8a02): [1 .. GetCount] (read again each pass), each LuaObject::GetString
// 0x907a90. (A value that is no string would be the original's Lua error; ReadData runs outside any Lua call here,
// so ours leaves it out. The lists are aipersonality.lua's own.)
void ReadStrings(lua_State* L, int idx, std::vector<std::string>* out) {
  if (!lua_istable(L, idx)) return;
  const int n = ObjectCount(L, idx);
  for (int i = 1; i <= n; ++i) {
    lua_rawgeti(L, idx, i);
    if (lua_type(L, -1) == LUA_TSTRING) out->push_back(lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// CAiPersonality::ReadData 0x5b7340: the template named 'AverageJoe' (the name is a constant in this build).
void ReadData(Sim& sim, lua_State* L, AiPersonality* p) {
  const char* const want = "AverageJoe";
  const int top = lua_gettop(L);
  if (!sim.PushImport(L, "/lua/aipersonality.lua")) {
    lua_settop(L, top);
    return;
  }
  lua_pushstring(L, "AIPersonalityTemplate");
  lua_gettable(L, -2);
  if (!lua_istable(L, -1)) {
    Logf(LogLevel::Warning, "Can't find AIPersonalityTemplate");
    lua_settop(L, top);
    return;
  }
  const int templates = lua_gettop(L);
  // GetCount 0x5b743e, kept as an unsigned 16-bit number (movzx); a template that is no table, or whose GetCount
  // (0x5b7481) is not 33, is skipped
  const int count = ObjectCount(L, templates) & 0xffff;
  for (int i = 1; i <= count; ++i) {
    lua_rawgeti(L, templates, i);
    const int e = lua_gettop(L);
    bool match = false;
    if (lua_istable(L, e) && ObjectCount(L, e) == kEntries) {
      lua_rawgeti(L, e, 1);
      match = lua_isstring(L, -1) && EqualNoCase(lua_tostring(L, -1), want);
      lua_pop(L, 1);
    }
    if (!match) {
      lua_pop(L, 1);
      continue;
    }
    lua_rawgeti(L, e, 1);
    p->name = lua_tostring(L, -1);
    lua_rawgeti(L, e, 2);
    p->chatPersonality = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    int pair = 0;
    for (int k = 3; k <= kEntries; ++k) {
      lua_rawgeti(L, e, k);
      if (k == kFavStructures) {
        ReadStrings(L, lua_gettop(L), &p->favouriteStructures);
      } else if (k == kFavUnits) {
        ReadStrings(L, lua_gettop(L), &p->favouriteUnits);
      } else if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        p->lo[pair] = static_cast<float>(lua_tonumber(L, -2));
        p->hi[pair] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 2);
        ++pair;
      } else {
        ++pair;
      }
      lua_pop(L, 1);
    }
    lua_settop(L, top);
    return;
  }
  Logf(LogLevel::Warning, "Can't find the template for personality %s", want);
  lua_settop(L, top);
}

AiPersonality* Self(lua_State* L) { return CheckObject<AiPersonality>(L, 1); }

// every range getter (0x5babc0 and its 28 siblings): (1 - difficulty) * min + max * difficulty
int l_Range(lua_State* L) {
  const AiPersonality* p = Self(L);
  const int i = static_cast<int>(lua_tonumber(L, lua_upvalueindex(1)));
  const float a = (1.0f - p->difficulty) * p->lo[i];
  const float b = p->hi[i] * p->difficulty;
  lua_pushnumber(L, a + b);
  return 1;
}
int l_GetPersonalityName(lua_State* L) {
  lua_pushstring(L, Self(L)->name.c_str());
  return 1;
}
int l_GetChatPersonality(lua_State* L) {
  lua_pushstring(L, Self(L)->chatPersonality.c_str());
  return 1;
}
int l_GetDifficulty(lua_State* L) {
  lua_pushnumber(L, Self(L)->difficulty);
  return 1;
}
// AdjustDelay(delay, multiplier) 0x5ba9f0: both taken as integers (truncated);
// delay + round((1 - difficulty) * (delay * multiplier))
int l_AdjustDelay(lua_State* L) {
  const AiPersonality* p = Self(L);
  const int delay = static_cast<int>(luaL_checknumber(L, 2));
  const int mult = static_cast<int>(luaL_checknumber(L, 3));
  const float f = (1.0f - p->difficulty) * static_cast<float>(mult * delay);
  lua_pushnumber(L, static_cast<float>(static_cast<int>(std::nearbyint(f)) + delay));
  return 1;
}
int PushStrings(lua_State* L, const std::vector<std::string>& v) {
  lua_newtable(L);
  int i = 1;
  for (const auto& s : v) {
    lua_pushstring(L, s.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}
int l_GetFavouriteStructures(lua_State* L) { return PushStrings(L, Self(L)->favouriteStructures); }
int l_GetFavouriteUnits(lua_State* L) { return PushStrings(L, Self(L)->favouriteUnits); }

}  // namespace

AiPersonality* CreateAiPersonality(Sim& sim, lua_State* L) {
  auto* p = new AiPersonality;
  const int top = lua_gettop(L);
  // the ctor (func_LuaAiPersonality 0x5b7220): an instance of the scripts' AIPersonality class
  bool haveClass = false;
  if (sim.PushImport(L, "/lua/aipersonality.lua")) {
    lua_pushstring(L, "AIPersonality");
    lua_gettable(L, -2);
    haveClass = lua_istable(L, -1);
  }
  if (!haveClass) {
    Logf(LogLevel::Warning, "Can't find AIPersonality, using CAiPersonality directly");
    lua_settop(L, top);
    PushClassTable(L, "CAiPersonality");
  }
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_setmetatable(L, -2);
  BindObject(L, -1, p);
  lua_settop(L, top);
  ReadData(sim, L, p);
  return p;
}

void RegisterAiPersonalityBindings(lua_State* L) {
  for (int i = 0; i < AiPersonality::kRanges; ++i) {
    // SetMethod takes a plain function: one closure per getter is made by hand
    PushClassTable(L, "CAiPersonality");
    lua_pushstring(L, kRangeGetters[i]);
    lua_pushnumber(L, i);
    lua_pushcclosure(L, l_Range, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }
  SetMethod(L, "CAiPersonality", "GetPersonalityName", l_GetPersonalityName);
  SetMethod(L, "CAiPersonality", "GetChatPersonality", l_GetChatPersonality);
  SetMethod(L, "CAiPersonality", "GetDifficulty", l_GetDifficulty);
  SetMethod(L, "CAiPersonality", "AdjustDelay", l_AdjustDelay);
  SetMethod(L, "CAiPersonality", "GetFavouriteStructures", l_GetFavouriteStructures);
  SetMethod(L, "CAiPersonality", "GetFavouriteUnits", l_GetFavouriteUnits);
}

}  // namespace moho
