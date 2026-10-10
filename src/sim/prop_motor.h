// Entity motors (engine-ref prop_falldown.md): Entity:FallDown() gives a falling-tree motor
// (MotorFallDown:Whack starts the fall), Entity:SinkAway(vy) a sinking one. A motor runs once a beat in
// the entity stage for entities that are not units, projectiles or beams.
#pragma once
#include "script/lua.hpp"

namespace moho {

class Sim;

void RegisterMotorBindings(lua_State* L);
// Beat step 8: every entity with a motor (not attached) advances it.
void MotorsTick(Sim& sim);
// Step 13: the moved entities take their new cells in the entity grid.
void MotorsAdvanceCoords(Sim& sim);

}  // namespace moho
