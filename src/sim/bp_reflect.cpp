// Blueprints as the sim sees them.
//
// The original converts every script blueprint into its engine structure (RUnitBlueprint, ...)
// and gives the sim that structure back as a table: every engine field is present (defaults
// filled in), sounds are engine objects, bit sets are written as numbers or enum names, and
// some fields are derived (icon name, ordinals, ...). Scripts rely on it - e.g. unit.lua reads
// bp.Defense.Shield.ShieldSize for every unit because "registration always creates a dummy
// Shield entry". The defaults come from the oracle probe (tools/gen_bp_defaults.py).
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "sim/blueprints.h"
#include "sim/script_object.h"

namespace moho {
namespace {

struct BpDefault {
  BpKind kind;
  const char* path;
  int type;  // 1 number, 2 string, 3 boolean
  double num;
  const char* str;
  bool b;
};
#define N(x) 1, static_cast<double>(x), nullptr, false
#define S(x) 2, 0.0, x, false
#define B(x) 3, 0.0, nullptr, x
const BpDefault kDefaults[] = {
#include "sim/bp_defaults.inc"
};
#undef N
#undef S
#undef B

void PushDefault(lua_State* L, const BpDefault& d) {
  if (d.type == 1) lua_pushnumber(L, static_cast<lua_Number>(d.num));
  else if (d.type == 2) lua_pushstring(L, d.str);
  else lua_pushboolean(L, d.b);
}

// Apply one default below the table at `t` following path segments [i..].
void ApplyPath(lua_State* L, int t, const std::vector<std::string>& seg, size_t i, const BpDefault& d) {
  lua_checkstack(L, 6);
  if (seg[i] == "[]") {
    for (int k = 1;; ++k) {
      lua_rawgeti(L, t, k);
      if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
      }
      if (i + 1 < seg.size()) ApplyPath(L, lua_gettop(L), seg, i + 1, d);
      lua_pop(L, 1);
    }
  }
  lua_pushstring(L, seg[i].c_str());
  lua_rawget(L, t);
  if (i + 1 == seg.size()) {
    bool missing = lua_isnil(L, -1);
    // a numeric engine field given as a string ("2.5") is stored as a number
    bool coerce = d.type == 1 && lua_type(L, -1) == LUA_TSTRING;
    double num = coerce ? std::atof(lua_tostring(L, -1)) : 0;
    lua_pop(L, 1);
    if (coerce) {
      lua_pushstring(L, seg[i].c_str());
      lua_pushnumber(L, static_cast<lua_Number>(num));
      lua_rawset(L, t);
      return;
    }
    if (missing) {
      lua_pushstring(L, seg[i].c_str());
      PushDefault(L, d);
      lua_rawset(L, t);
    }
    return;
  }
  if (lua_isnil(L, -1)) {
    if (seg[i + 1] == "[]") {  // no array to fill
      lua_pop(L, 1);
      return;
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, seg[i].c_str());
    lua_pushvalue(L, -2);
    lua_rawset(L, t);
  }
  if (lua_istable(L, -1)) ApplyPath(L, lua_gettop(L), seg, i + 1, d);
  lua_pop(L, 1);
}

std::vector<std::vector<std::string>>& SplitPaths() {
  static std::vector<std::vector<std::string>> v;
  if (v.empty())
    for (const auto& d : kDefaults) {
      std::vector<std::string> seg;
      std::string p = d.path;
      size_t a = 0;
      for (size_t b; (b = p.find('.', a)) != std::string::npos; a = b + 1) seg.push_back(p.substr(a, b - a));
      seg.push_back(p.substr(a));
      v.push_back(seg);
    }
  return v;
}

// ---- sounds ---------------------------------------------------------------------------------
// A sound is an engine object: { Bank, Cue, LodCutoff } become an opaque handle.
struct SoundParams {
  std::string bank, cue, lodCutoff;
};
std::vector<SoundParams>& Sounds() {
  static std::vector<SoundParams> v;
  return v;
}

std::string Field(lua_State* L, int t, const char* k) {
  lua_pushstring(L, k);
  lua_rawget(L, t);
  std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
  lua_pop(L, 1);
  return s;
}

void PushSound(lua_State* L, int t) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  SoundParams sp{Field(L, t, "Bank"), Field(L, t, "Cue"), Field(L, t, "LodCutoff")};
  Sounds().push_back(sp);
  auto* id = static_cast<int32_t*>(lua_newuserdata(L, sizeof(int32_t)));
  *id = static_cast<int32_t>(Sounds().size() - 1);
  PushClassTable(L, "Sound");
  lua_setmetatable(L, -2);
}

// Replace every table value of t[key] (a sound table set) by sound handles.
void ConvertSounds(lua_State* L, int t, const char* key) {
  lua_pushstring(L, key);
  lua_rawget(L, t);
  if (lua_istable(L, -1)) {
    int a = lua_gettop(L);
    std::vector<std::string> names;
    lua_pushnil(L);
    while (lua_next(L, a)) {
      if (lua_istable(L, -1) && lua_type(L, -2) == LUA_TSTRING) names.push_back(lua_tostring(L, -2));
      lua_pop(L, 1);
    }
    for (const auto& n : names) {
      lua_pushstring(L, n.c_str());
      lua_pushstring(L, n.c_str());
      lua_rawget(L, a);
      PushSound(L, -1);
      lua_remove(L, -2);
      lua_rawset(L, a);
    }
  }
  lua_pop(L, 1);
}

// ---- bit sets -------------------------------------------------------------------------------
// The original writes a bit set as the enum name when exactly one named value matches,
// otherwise as its decimal value (a string).
const char* kCommandCaps[] = {"RULEUCC_Move", "RULEUCC_Stop", "RULEUCC_Attack", "RULEUCC_Guard", "RULEUCC_Patrol",
                              "RULEUCC_RetaliateToggle", "RULEUCC_Repair", "RULEUCC_Capture", "RULEUCC_Transport",
                              "RULEUCC_CallTransport", "RULEUCC_Nuke", "RULEUCC_Tactical", "RULEUCC_Teleport",
                              "RULEUCC_Ferry", "RULEUCC_SiloBuildTactical", "RULEUCC_SiloBuildNuke",
                              "RULEUCC_Sacrifice", "RULEUCC_Pause", "RULEUCC_Overcharge", "RULEUCC_Dive",
                              "RULEUCC_Reclaim", "RULEUCC_SpecialAction", "RULEUCC_Dock", "RULEUCC_Script"};
const char* kToggleCaps[] = {"RULEUTC_ShieldToggle", "RULEUTC_WeaponToggle", "RULEUTC_JammingToggle",
                             "RULEUTC_IntelToggle", "RULEUTC_ProductionToggle", "RULEUTC_StealthToggle",
                             "RULEUTC_GenericToggle", "RULEUTC_SpecialToggle", "RULEUTC_CloakToggle"};
const char* kLayers[] = {"LAYER_Land", "LAYER_Seabed", "LAYER_Sub", "LAYER_Water", "LAYER_Air", "LAYER_Orbit"};
const char* kLayerNames[] = {"Land", "Seabed", "Sub", "Water", "Air", "Orbit"};

void ConvertBits(lua_State* L, int t, const char* key, const char* const* flags, const char* const* names, int n) {
  lua_pushstring(L, key);
  lua_rawget(L, t);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) {
    lua_pushstring(L, flags[i]);
    lua_rawget(L, -2);
    if (lua_toboolean(L, -1)) v |= 1u << i;
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  std::string s = std::to_string(v);
  for (int i = 0; i < n; ++i)
    if (v == (1u << i)) s = names[i];
  lua_pushstring(L, key);
  lua_pushstring(L, s.c_str());
  lua_rawset(L, t);
}

void Lowercase(lua_State* L, int t, const char* key) {
  lua_pushstring(L, key);
  lua_rawget(L, t);
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::string s = lua_tostring(L, -1);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    lua_pushstring(L, key);
    lua_pushstring(L, s.c_str());
    lua_rawset(L, t);
  }
  lua_pop(L, 1);
}

void SetNumber(lua_State* L, int t, const char* key, double v) {
  lua_pushstring(L, key);
  lua_pushnumber(L, static_cast<lua_Number>(v));
  lua_rawset(L, t);
}

bool PushField(lua_State* L, int t, const char* key) {
  lua_pushstring(L, key);
  lua_rawget(L, t);
  if (lua_istable(L, -1)) return true;
  lua_pop(L, 1);
  return false;
}

void DefaultString(lua_State* L, int t, const char* key, const char* v) {
  lua_pushstring(L, key);
  lua_rawget(L, t);
  bool missing = lua_isnil(L, -1);
  lua_pop(L, 1);
  if (missing) {
    lua_pushstring(L, key);
    lua_pushstring(L, v);
    lua_rawset(L, t);
  }
}

}  // namespace

// Turn the copied script blueprint at `t` into the sim's view of it (see the file comment).
void ReflectBlueprint(lua_State* L, int t, const BlueprintInfo& bp) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  lua_checkstack(L, 20);
  SetNumber(L, t, "BlueprintOrdinal", bp.ordinal);
  auto& paths = SplitPaths();
  for (size_t i = 0; i < sizeof kDefaults / sizeof kDefaults[0]; ++i)
    if (kDefaults[i].kind == bp.kind) ApplyPath(L, t, paths[i], 0, kDefaults[i]);
  if (bp.kind != BpKind::Prop) DefaultString(L, t, "Description", "");
  if (bp.kind == BpKind::Unit) {
    if (PushField(L, t, "General")) {  // absent bit sets are written as "0"
      DefaultString(L, lua_gettop(L), "CommandCaps", "0");
      DefaultString(L, lua_gettop(L), "ToggleCaps", "0");
      lua_pop(L, 1);
    }
    if (PushField(L, t, "Physics")) {
      DefaultString(L, lua_gettop(L), "BuildOnLayerCaps", "Land");
      lua_pop(L, 1);
    }
    lua_pushstring(L, "Display");
    lua_rawget(L, t);
    if (lua_istable(L, -1)) {
      lua_pushstring(L, "IconName");
      lua_rawget(L, -2);
      bool has = !lua_isnil(L, -1);
      lua_pop(L, 1);
      if (!has) {
        lua_pushstring(L, "IconName");
        lua_pushstring(L, bp.id.c_str());
        lua_rawset(L, -3);
      }
    }
    lua_pop(L, 1);
    if (PushField(L, t, "General")) {
      ConvertBits(L, lua_gettop(L), "CommandCaps", kCommandCaps, kCommandCaps, 24);
      ConvertBits(L, lua_gettop(L), "ToggleCaps", kToggleCaps, kToggleCaps, 9);
      lua_pop(L, 1);
    }
    if (PushField(L, t, "Physics")) {
      ConvertBits(L, lua_gettop(L), "BuildOnLayerCaps", kLayers, kLayerNames, 6);
      lua_pop(L, 1);
    }
    if (PushField(L, t, "Weapon")) {
      int w = lua_gettop(L);
      for (int k = 1;; ++k) {
        lua_rawgeti(L, w, k);
        if (!lua_istable(L, -1)) {
          lua_pop(L, 1);
          break;
        }
        Lowercase(L, lua_gettop(L), "ProjectileId");
        DefaultString(L, lua_gettop(L), "ProjectileId", "");
        DefaultString(L, lua_gettop(L), "Label", "");
        ConvertSounds(L, lua_gettop(L), "Audio");
        lua_pop(L, 1);
      }
      lua_pop(L, 1);
    }
  }
  ConvertSounds(L, t, "Audio");
}


}  // namespace moho
