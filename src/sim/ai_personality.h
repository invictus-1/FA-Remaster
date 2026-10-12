// CAiPersonality: the numbers a classic AI brain steers by (how big an army, how often it attacks, which kinds
// of unit it favours). Every brain has one (the CAiBrain ctor 0x579f80 makes it and reads its data); the brain
// scripts reach it through CAiBrain:GetPersonality().
#pragma once
#include <string>
#include <vector>

#include "sim/script_object.h"

namespace moho {

class Sim;

class AiPersonality : public ScriptObject {
 public:
  enum { kRanges = 29 };
  std::string name;             // +0x38: the template's name
  std::string chatPersonality;  // +0x54
  // The template's {min, max} pairs in its own order (+0x70..): army size, platoon size, attack frequency, repeat
  // attack frequency, counter forces, intel gathering, coordinated attacks, expansion driven, tech advancement,
  // upgrades driven; defense driven, economy driven, factory tycoon, intel building tycoon, super weapon tendency;
  // air, tank, bot, sea, specialty forces, support units, direct damage, indirect damage emphasis; survival
  // emphasis, team support, formation use, target spread; quitting tendency, chat frequency.
  float lo[kRanges] = {};
  float hi[kRanges] = {};
  std::vector<std::string> favouriteStructures;  // +0xe8
  std::vector<std::string> favouriteUnits;       // +0x138
  float difficulty = 0.5f;                       // +0x178: set by the ctor (0x5b70bf) and never changed
};

// A new personality with its Lua object (an instance of /lua/aipersonality.lua's AIPersonality), filled from that
// file's AIPersonalityTemplate (CAiPersonality ctor 0x5b6dc0 + ReadData 0x5b7340). The caller owns it.
AiPersonality* CreateAiPersonality(Sim& sim, lua_State* L);

void RegisterAiPersonalityBindings(lua_State* L);

}  // namespace moho
