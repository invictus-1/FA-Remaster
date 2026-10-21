// The engine's reading of a reflected enum argument (sim/enum_arg.h). Every cfunc that takes an enum by name goes
// through SCR_GetEnum 0x4d2550, which hands the string to the type's Reflection::SetLexical 0x8d9670:
//   - the string is cut at each '|' and the parts' values are or-ed;
//   - a part loses the type's prefix ("INTEL_", "ALLIANCE_", ...) when it starts with it (strncmp: case counts);
//   - it is then looked up among the names WITHOUT case and over the whole part (_strnicmp, the name ends there:
//     0x8d9729..0x8d9744); the names are kept with the prefix stripped (StripPrefix 0x8d86f0 at registration);
//   - else it is read as a number (func_ParseNum 0x8d8fa0: an optional '-', "0x" hex, a leading 0 octal, else
//     decimal; its letter test takes the lower-case letters and, by a slip, 'A');
//   - if a part is neither, SCR_GetEnum raises "Invalid enum value %s\nValid Options are:\n" followed by
//     "   name\n" for every name (0xe0a2bc, 0xe0a2dc, 0xe0a2d4).
// The tables below are each type's AddEnums, in its order (the first name of a value wins a lookup).
#pragma once
#include <cstring>
#include <string>

#include "script/lua.hpp"

namespace moho {

struct EnumName {
  const char* name;
  int value;
};
struct EnumType {
  const char* prefix;  // nullptr: none
  const EnumName* names;
  int count;
};

// func_ParseNum 0x8d8fa0 on [p, end)
inline bool EnumParseNumber(const char* p, const char* end, int* out) {
  bool neg = false;
  if (*p == '-') {
    neg = true;
    ++p;
  }
  int base = 10;
  if (*p == '0') {
    if (p[1] == 'x') {
      base = 16;
      p += 2;
    } else {
      base = 8;
    }
  }
  int v = 0;
  for (;;) {
    const char c = *p;
    int d;
    if (static_cast<unsigned char>(c - '0') <= 9) d = c - '0';
    else if (static_cast<unsigned char>(c - 'a') <= 25 || c == 'A') d = c - 0x57;
    else return false;
    if (d >= base) return false;
    v = base * v + d;
    ++p;
    if (*p == 0 || p == end) break;
  }
  *out = neg ? -v : v;
  return true;
}

// Reflection::SetLexical 0x8d9670 for an enum type: true and the value, or false
inline bool EnumLexical(const EnumType& t, const char* s, int* out) {
  const size_t plen = t.prefix ? std::strlen(t.prefix) : 0;
  int value = 0;
  const char* p = s;
  for (;;) {
    const char* bar = std::strchr(p, '|');
    const char* end = bar ? bar : p + std::strlen(p);
    const char* q = plen && std::strncmp(p, t.prefix, plen) == 0 ? p + plen : p;
    const size_t n = end > q ? static_cast<size_t>(end - q) : 0;
    bool found = false;
    for (int i = 0; i < t.count; ++i)
      if (strncasecmp(q, t.names[i].name, n) == 0 && t.names[i].name[n] == 0) {
        value |= t.names[i].value;
        found = true;
        break;
      }
    if (!found) {
      int num = 0;
      if (!EnumParseNumber(q, end, &num)) return false;
      value |= num;
    }
    if (!bar) break;
    p = bar + 1;
  }
  *out = value;
  return true;
}

// SCR_GetEnum 0x4d2550: the value, or the exe's error (which does not return)
inline int EnumValue(lua_State* L, const EnumType& t, const char* s) {
  int v = 0;
  if (EnumLexical(t, s, &v)) return v;
  std::string msg = std::string("Invalid enum value ") + s + "\nValid Options are:\n";
  for (int i = 0; i < t.count; ++i) msg += std::string("   ") + t.names[i].name + "\n";
  luaL_error(L, "%s", msg.c_str());
  return 0;
}
// The same for argument idx, taken as a string the way the cfuncs take it (lua_tostring, else a "string" type
// error: 0x90ca90, 0x4154b0 with 0xe00ad0)
inline int EnumArgAt(lua_State* L, int idx, const EnumType& t) {
  const char* s = lua_tostring(L, idx);
  if (!s) luaL_typerror(L, idx, "string");
  return EnumValue(L, t, s);
}
// The name of a value as the type keeps it (its first), or nullptr
inline const char* EnumNameOf(const EnumType& t, int v) {
  for (int i = 0; i < t.count; ++i)
    if (t.names[i].value == v) return t.names[i].name;
  return nullptr;
}

// EAlliance: EAllianceTypeInfo::AddEnums 0x509e20, prefix ALLIANCE_
inline constexpr EnumName kAllianceNames[] = {{"Neutral", 0}, {"Ally", 1}, {"Enemy", 2}};
inline constexpr EnumType kEnumAlliance{"ALLIANCE_", kAllianceNames, 3};
// EVisibilityMode: EVisibilityModeTypeInfo::AddEnums 0x50a1c0, prefix VIZMODE_
inline constexpr EnumName kVizModeNames[] = {{"Always", 2}, {"Never", 1}, {"Intel", 4}};
inline constexpr EnumType kEnumVizMode{"VIZMODE_", kVizModeNames, 3};
// EIntel: EIntelTypeInfo::AddEnums 0x50a460, prefix INTEL_
inline constexpr EnumName kIntelNames[] = {{"None", 0}, {"Vision", 1}, {"WaterVision", 2}, {"Radar", 3},
    {"Sonar", 4}, {"Omni", 5}, {"RadarStealthField", 6}, {"SonarStealthField", 7}, {"CloakField", 8}, {"Jammer", 9},
    {"Spoof", 10}, {"Cloak", 11}, {"RadarStealth", 12}, {"SonarStealth", 13}};
inline constexpr EnumType kEnumIntel{"INTEL_", kIntelNames, 14};
// ELayer: ELayerTypeInfo::AddEnums 0x50bab0, prefix LAYER_
inline constexpr EnumName kLayerNames[] = {{"None", 0}, {"Land", 1}, {"Seabed", 2}, {"Sub", 4}, {"Water", 8},
    {"Air", 16}, {"Orbit", 32}, {"All", 127}};
inline constexpr EnumType kEnumLayer{"LAYER_", kLayerNames, 8};
// ERuleBPUnitCommandCaps: ERuleBPUnitCommandCapsTypeInfo::AddEnums 0x51fd40, prefix None
inline constexpr EnumName kCommandCapsNames[] = {{"RULEUCC_Move", 1}, {"RULEUCC_Stop", 2}, {"RULEUCC_Attack", 4},
    {"RULEUCC_Guard", 8}, {"RULEUCC_Patrol", 16}, {"RULEUCC_RetaliateToggle", 32}, {"RULEUCC_Repair", 64},
    {"RULEUCC_Capture", 128}, {"RULEUCC_Transport", 256}, {"RULEUCC_CallTransport", 512}, {"RULEUCC_Nuke", 1024},
    {"RULEUCC_Tactical", 2048}, {"RULEUCC_Teleport", 4096}, {"RULEUCC_Ferry", 8192},
    {"RULEUCC_SiloBuildTactical", 16384}, {"RULEUCC_SiloBuildNuke", 32768}, {"RULEUCC_Sacrifice", 65536},
    {"RULEUCC_Pause", 131072}, {"RULEUCC_Overcharge", 262144}, {"RULEUCC_Dive", 524288},
    {"RULEUCC_Reclaim", 1048576}, {"RULEUCC_SpecialAction", 2097152}, {"RULEUCC_Dock", 4194304},
    {"RULEUCC_Script", 8388608}, {"RULEUCC_Invalid", 8388608}};
inline constexpr EnumType kEnumCommandCaps{nullptr, kCommandCapsNames, 25};
// ERuleBPUnitToggleCaps: ERuleBPUnitToggleCapsTypeInfo::AddEnums 0x5200c0, prefix None
inline constexpr EnumName kToggleCapsNames[] = {{"RULEUTC_ShieldToggle", 1}, {"RULEUTC_WeaponToggle", 2},
    {"RULEUTC_JammingToggle", 4}, {"RULEUTC_IntelToggle", 8}, {"RULEUTC_ProductionToggle", 16},
    {"RULEUTC_StealthToggle", 32}, {"RULEUTC_GenericToggle", 64}, {"RULEUTC_SpecialToggle", 128},
    {"RULEUTC_CloakToggle", 256}};
inline constexpr EnumType kEnumToggleCaps{nullptr, kToggleCapsNames, 9};
// EUnitState: EUnitStateTypeInfo::AddEnums 0x55bbd0, prefix UNITSTATE_
inline constexpr EnumName kUnitStateNames[] = {{"Immobile", 1}, {"Moving", 2}, {"Attacking", 3}, {"Guarding", 4},
    {"Building", 5}, {"Upgrading", 6}, {"WaitingForTransport", 7}, {"TransportLoading", 8},
    {"TransportUnloading", 9}, {"MovingDown", 10}, {"MovingUp", 11}, {"Patrolling", 12}, {"Busy", 13},
    {"Attached", 14}, {"BeingReclaimed", 15}, {"Repairing", 16}, {"Diving", 17}, {"Surfacing", 18},
    {"Teleporting", 19}, {"Ferrying", 20}, {"WaitForFerry", 21}, {"AssistMoving", 22}, {"PathFinding", 23},
    {"ProblemGettingToGoal", 24}, {"NeedToTerminateTask", 25}, {"Capturing", 26}, {"BeingCaptured", 27},
    {"Reclaiming", 28}, {"AssistingCommander", 29}, {"Refueling", 30}, {"GuardBusy", 31}, {"ForceSpeedThrough", 32},
    {"UnSelectable", 33}, {"DoNotTarget", 34}, {"LandingOnPlatform", 35}, {"CannotFindPlaceToLand", 36},
    {"BeingUpgraded", 37}, {"Enhancing", 38}, {"BeingBuilt", 39}, {"NoReclaim", 40}, {"NoCost", 41},
    {"BlockCommandQueue", 42}, {"MakingAttackRun", 43}, {"HoldingPattern", 44}, {"SiloBuildingAmmo", 45}};
inline constexpr EnumType kEnumUnitState{"UNITSTATE_", kUnitStateNames, 45};
// ECompareType: ECompareTypeTypeInfo::AddEnums 0x5798d0, prefix COMPARE_
inline constexpr EnumName kCompareNames[] = {{"Closest", 0}, {"Furthest", 1}, {"HighestValue", 2},
    {"LeastDefended", 4}};
inline constexpr EnumType kEnumCompare{"COMPARE_", kCompareNames, 4};
// EThreatType: EThreatTypeTypeInfo::AddEnums 0x715590, prefix THREATTYPE_
inline constexpr EnumName kThreatTypeNames[] = {{"Overall", 0}, {"OverallNotAssigned", 1}, {"StructuresNotMex", 3},
    {"Structures", 2}, {"Naval", 4}, {"Air", 6}, {"Land", 5}, {"Experimental", 7}, {"Commander", 8},
    {"Artillery", 9}, {"AntiAir", 10}, {"AntiSurface", 11}, {"AntiSub", 12}, {"Economy", 13}, {"Unknown", 14}};
inline constexpr EnumType kEnumThreatType{"THREATTYPE_", kThreatTypeNames, 15};
// ESquadClass: ESquadClassTypeInfo::AddEnums 0x723bd0, prefix SQUADCLASS_
inline constexpr EnumName kSquadNames[] = {{"Unassigned", 0}, {"Attack", 1}, {"Artillery", 2}, {"Guard", 3},
    {"Support", 4}, {"Scout", 5}};
inline constexpr EnumType kEnumSquad{"SQUADCLASS_", kSquadNames, 6};
// ETriggerOperator: ETriggerOperatorTypeInfo::AddEnums 0x70aef0, prefix TRIGGER_
inline constexpr EnumName kTriggerNames[] = {{"GreaterThan", 0}, {"GreaterThanOrEqual", 1}, {"LessThan", 2},
    {"LessThanOrEqual", 3}};
inline constexpr EnumType kEnumTrigger{"TRIGGER_", kTriggerNames, 4};
// EEconResource: EEconResourceTypeInfo::AddEnums 0x563a70, prefix ECON_
inline constexpr EnumName kEconNames[] = {{"ENERGY", 0}, {"MASS", 1}};
inline constexpr EnumType kEnumEcon{"ECON_", kEconNames, 2};

}  // namespace moho
