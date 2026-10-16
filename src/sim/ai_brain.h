// The classic AI brain's own small state and the CAiBrain methods that read or write it (sim/ai_brain.cpp):
// the current enemy (GetCurrentEnemy, SetCurrentEnemy), the current plan (SetCurrentPlan), the army's no-rush
// timer (GetNoRushTicks), SetGreaterOf, IsOpponentAIRunning, FindClosestArmyWithBase, and the attack vectors
// (SetUpAttackVectorsToArmy, GetAttackVectors, PickBestAttackVector).
//
// What the original keeps (FA exe, read 2026-10-15; the addresses are in sim/ai_brain.cpp):
// - CAiBrain +0x34 its army, +0x38 the current enemy (an IArmy*, null = none), +0x40 the plan (a std::string that
//   only SetCurrentPlan writes), +0x5c the attack vectors (a vector of SPointVector: a point and a direction, six
//   floats), +0x70 the category the last SetUpAttackVectorsToArmy used.
// - CArmyImpl +0x1cc the no-rush timer in beats: the army's ctor sets it from ScenarioInfo.Options.NoRushOption
//   (FAF's patch: 600 per minute, 'Off' = 0), and every OnTick takes one off while it is above 0.
// - The attack vectors are only computed when a script asks (SetUpAttackVectorsToArmy). CArmyImpl::OnTick
//   0x6fff4d also calls CAiBrain::ProcessAttackVectors for every army when the sim vars AI_RenderDebugAttackVectors
//   (default off) and AI_DebugArmyIndex (default -1) are set - a debug view set by console command only
//   (DoSimCommand, a DebugCommand message, which we do not handle), so that call is not made here.
#pragma once
#include "script/lua.hpp"

namespace moho {

class Sim;

// Each army's brain state and no-rush timer, made with the armies (the CArmyImpl ctor reads NoRushOption): call
// after the armies exist and ScenarioInfo.Options is set, before the brains' OnCreateArmyBrain.
void InitBrainState(Sim& sim);

// The CAiBrain methods listed above.
void RegisterBrainBindings(lua_State* L);

}  // namespace moho
