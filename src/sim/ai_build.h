// What the classic AI brains build with (sim/ai_build.cpp): CAiBrain's build functions (DecideWhatToBuild,
// FindPlaceToBuild, BuildStructure, BuildUnit, BuildPlatoon, CanBuildPlatoon, ...), the brain's pending structure
// plans that keep two engineers off one spot, and the unit queries the AI scripts lean on (GetNumUnitsAroundPoint,
// GetAvailableFactories, NumCurrentlyBuilding, ...). Read from the FA exe (build 3839) 2026-10-15; the addresses are
// in sim/ai_build.cpp.
#pragma once
#include "script/lua.hpp"

namespace moho {

class AiBrain;
class Sim;
struct BlueprintInfo;
struct Vec3;

// CAiBrain::CanBuildStructureAt 0x57cbb0. alliance: EAlliance (0 Neutral, 1 Ally, 2 Enemy) whose structures may
// stand there (the location test then falls back to "none of the others' structures around"), 3 none.
bool AiBrainCanBuildStructureAt(Sim& sim, AiBrain* brain, const BlueprintInfo& bp, const Vec3& pos, int alliance);
// The last part of it (0x57cfac..0x57d283): a pending plan of the brain's BuildStructure (brain+0x98) whose skirt
// meets the blueprint's skirt at (x, z). Plans whose builder died or whose order is gone are dropped on the way.
bool AiBuildPlansBlock(Sim& sim, AiBrain* brain, const BlueprintInfo& bp, float x, float z);

void RegisterAiBuildBindings(lua_State* L);

}  // namespace moho
