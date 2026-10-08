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
  int type;  // 1 number, 2 string, 3 boolean, 4 table (an engine list/set: always there, maybe empty)
  double num;
  const char* str;
  bool b;
};
#define N(x) 1, static_cast<double>(x), nullptr, false
#define S(x) 2, 0.0, x, false
#define B(x) 3, 0.0, nullptr, x
#define T() 4, 0.0, nullptr, false
const BpDefault kDefaults[] = {
#include "sim/bp_defaults.inc"
#include "sim/bp_defaults_extra.inc"
};
#undef N
#undef S
#undef B
#undef T

void PushDefault(lua_State* L, const BpDefault& d) {
  if (d.type == 1) lua_pushnumber(L, static_cast<lua_Number>(d.num));
  else if (d.type == 2) lua_pushstring(L, d.str);
  else if (d.type == 4) lua_newtable(L);
  else lua_pushboolean(L, d.b);
}

// Engine enums: a value is matched without regard to case and stored by its proper name.
const char* const kEnums[] = {
    "RULEUMT_None", "RULEUMT_Land", "RULEUMT_Air", "RULEUMT_Water", "RULEUMT_Biped", "RULEUMT_SurfacingSub",
    "RULEUMT_Amphibious", "RULEUMT_Hover", "RULEUMT_AmphibiousFloating", "RULEUMT_Special",
    "RULEUBR_None", "RULEUBR_Bridge", "RULEUBR_OnMassDeposit", "RULEUBR_OnHydrocarbonDeposit",
    "RULEUBA_None", "RULEUBA_LowArc", "RULEUBA_HighArc",
    "RULEWTT_Unit", "RULEWTT_Projectile", "RULEWTT_Prop",
    "UWRC_Undefined", "UWRC_DirectFire", "UWRC_IndirectFire", "UWRC_AntiAir", "UWRC_AntiNavy", "UWRC_Countermeasure"};

bool SameNoCase(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
  return *a == *b;
}

// The enum a default value belongs to: its prefix ("RULEUBA_"), or "" when it is a plain string.
std::string EnumPrefix(const char* v) {
  for (const char* e : kEnums)
    if (!std::strcmp(e, v)) return std::string(v, std::strchr(v, '_') + 1);
  return "";
}

// The value at the top of the stack as the engine would store it in a field of the default's
// type: numbers from numeric strings, booleans from numbers and "true"/"false", enum names in
// their proper case. Pushes the replacement and returns true, or returns false to keep the value.
// A value the field cannot hold leaves the engine default.
bool CoerceValue(lua_State* L, const BpDefault& d) {
  int ty = lua_type(L, -1);
  if (d.type == 1) {
    if (ty == LUA_TNUMBER) return false;
    if (ty == LUA_TSTRING) lua_pushnumber(L, static_cast<lua_Number>(std::atof(lua_tostring(L, -1))));
    else PushDefault(L, d);
    return true;
  }
  if (d.type == 3) {
    if (ty == LUA_TBOOLEAN) return false;
    if (ty == LUA_TNUMBER) lua_pushboolean(L, lua_tonumber(L, -1) != 0);
    else if (ty == LUA_TSTRING) lua_pushboolean(L, SameNoCase(lua_tostring(L, -1), "true"));
    else PushDefault(L, d);
    return true;
  }
  if (d.type == 2) {
    if (ty != LUA_TSTRING) {
      if (ty == LUA_TNUMBER) lua_pushstring(L, lua_tostring(L, -1));
      else PushDefault(L, d);
      return true;
    }
    std::string prefix = EnumPrefix(d.str);
    if (prefix.empty()) return false;
    const char* v = lua_tostring(L, -1);
    for (const char* e : kEnums)
      if (!std::strncmp(e, prefix.c_str(), prefix.size()) && SameNoCase(e, v)) {
        if (!std::strcmp(e, v)) return false;
        lua_pushstring(L, e);
        return true;
      }
    PushDefault(L, d);  // not a value of the enum
    return true;
  }
  return false;
}

// Apply one default below the table at `t` following path segments [i..].
void ApplyPath(lua_State* L, int t, const std::vector<std::string>& seg, size_t i, const BpDefault& d) {
  lua_checkstack(L, 8);
  if (seg[i] == "[]") {  // every element of the list (as pairs() sees them: holes included)
    lua_pushnil(L);
    while (lua_next(L, t)) {
      if (lua_type(L, -2) == LUA_TNUMBER && lua_istable(L, -1) && i + 1 < seg.size())
        ApplyPath(L, lua_gettop(L), seg, i + 1, d);
      lua_pop(L, 1);
    }
    return;
  }
  lua_pushstring(L, seg[i].c_str());
  lua_rawget(L, t);
  if (i + 1 == seg.size()) {
    bool missing = lua_isnil(L, -1);
    if (!missing && d.type != 4) {
      if (CoerceValue(L, d)) {
        lua_pushstring(L, seg[i].c_str());
        lua_insert(L, -2);
        lua_rawset(L, t);
      }
      lua_pop(L, 1);
      return;
    }
    lua_pop(L, 1);
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

}  // namespace

void PushSound(lua_State* L, int t) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  SoundParams sp{Field(L, t, "Bank"), Field(L, t, "Cue"), Field(L, t, "LodCutoff")};
  Sounds().push_back(sp);
  auto* id = static_cast<int32_t*>(lua_newuserdata(L, sizeof(int32_t)));
  *id = static_cast<int32_t>(Sounds().size() - 1);
  PushClassTable(L, "Sound");
  lua_setmetatable(L, -2);
}

namespace {

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

namespace {

// Fields the engine derives (sim/bp_derived.cpp): part of the engine structure like the defaults.
const char* const kDerivedUnitFields[] = {
    "InertiaTensorX", "InertiaTensorY", "InertiaTensorZ", "Footprint.SizeX", "Footprint.SizeZ",
    "Footprint.OccupancyCaps", "Footprint.Flags", "Footprint.MaxSlope", "Footprint.MinWaterDepth",
    "AltFootprint.SizeX", "AltFootprint.SizeZ", "AltFootprint.OccupancyCaps", "AltFootprint.Flags",
    "AltFootprint.MaxSlope", "AltFootprint.MinWaterDepth", "Physics.SkirtOffsetX", "Physics.SkirtOffsetZ",
    "Physics.SkirtSizeX", "Physics.SkirtSizeZ", "Physics.BackUpDistance", "Physics.CatchUpAcc",
    "Physics.MaxSpeedReverse", "Physics.AttackElevation", "Air.CanFly", "Air.MaxAirspeed", "Air.MinAirspeed",
    "Air.StartTurnDistance"};

void CarryPath(lua_State* L, int to, int from, const std::vector<std::string>& seg) {
  int top = lua_gettop(L);
  // the value in `from`
  lua_pushvalue(L, from);
  for (const auto& s : seg) {
    if (s == "[]" || !lua_istable(L, -1)) {
      lua_settop(L, top);
      return;
    }
    lua_pushstring(L, s.c_str());
    lua_rawget(L, -2);
    lua_remove(L, -2);
  }
  if (lua_isnil(L, -1)) {
    lua_settop(L, top);
    return;
  }
  int value = lua_gettop(L);
  // the place in `to` (tables made as needed)
  lua_pushvalue(L, to);
  for (size_t i = 0; i + 1 < seg.size(); ++i) {
    lua_pushstring(L, seg[i].c_str());
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushstring(L, seg[i].c_str());
      lua_pushvalue(L, -2);
      lua_rawset(L, -4);
    }
    if (!lua_istable(L, -1)) {
      lua_settop(L, top);
      return;
    }
    lua_remove(L, -2);
  }
  lua_pushstring(L, seg.back().c_str());
  lua_rawget(L, -2);
  bool missing = lua_isnil(L, -1);
  lua_pop(L, 1);
  if (missing) {
    lua_pushstring(L, seg.back().c_str());
    lua_pushvalue(L, value);
    lua_rawset(L, -3);
  }
  lua_settop(L, top);
}

}  // namespace

void CarryOverEngineFields(lua_State* L, int to, int from, BpKind kind) {
  if (to < 0) to = lua_gettop(L) + to + 1;
  if (from < 0) from = lua_gettop(L) + from + 1;
  lua_checkstack(L, 20);
  auto& paths = SplitPaths();
  for (size_t i = 0; i < sizeof kDefaults / sizeof kDefaults[0]; ++i)
    if (kDefaults[i].kind == kind) CarryPath(L, to, from, paths[i]);
  if (kind == BpKind::Unit)
    for (const char* f : kDerivedUnitFields) {
      std::vector<std::string> seg;
      std::string p = f;
      size_t dot = p.find('.');
      if (dot == std::string::npos) seg = {p};
      else seg = {p.substr(0, dot), p.substr(dot + 1)};
      CarryPath(L, to, from, seg);
    }
}

// Turn the copied script blueprint at `t` into the sim's view of it (see the file comment).
void ReflectBlueprint(lua_State* L, int t, const BlueprintInfo& bp, const SimBlueprints& bps) {
  if (t < 0) t = lua_gettop(L) + t + 1;
  lua_checkstack(L, 20);
  SetNumber(L, t, "BlueprintOrdinal", bp.ordinal);
  auto& paths = SplitPaths();
  for (size_t i = 0; i < sizeof kDefaults / sizeof kDefaults[0]; ++i)
    if (kDefaults[i].kind == bp.kind) ApplyPath(L, t, paths[i], 0, kDefaults[i]);
  DefaultString(L, t, "Description", "");
  DeriveBlueprint(L, t, bp, bps);
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
      if (!has) {  // the unit id, which the engine keeps in lower case
        std::string icon = bp.id;
        for (auto& c : icon) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        lua_pushstring(L, "IconName");
        lua_pushstring(L, icon.c_str());
        lua_rawset(L, -3);
      }
    }
    lua_pop(L, 1);
    if (PushField(L, t, "General")) {
      for (const char* k : {"UpgradesTo", "UpgradesFrom", "UpgradesFromBase"})  // unit ids
        Lowercase(L, lua_gettop(L), k);
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
