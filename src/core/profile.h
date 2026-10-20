// MOHO64_PROFILE=1: the measuring tools of performance-lessons.md 4.1 (measure only). Everything here is off unless the
// variable is set, and none of it reads or writes simulation state: it only reads the clock.
//
// Lua time is taken at the Lua API's two entry points, lua_pcall and lua_resume (hooks in third_party/lua), and only at
// the outermost one on the thread that installed the timer: Lua's own pcall, coroutine.resume and a binding that calls
// back into Lua count inside their caller. So "Lua time" is the VM plus the engine functions the scripts call from
// there (a binding's C++ work counts as Lua), and everything else is the engine's.
#pragma once

namespace moho::prof {

bool On();  // MOHO64_PROFILE is set (read once)

// Where an outermost Lua call comes from.
enum Source : int {
  kThreadAI,          // script threads whose function comes from AI code (/AI/ paths, aibrain, platoon)
  kThreadUnit,        // script threads of unit, weapon, projectile and effect scripts
  kThreadOther,       // every other script thread
  kCallUnit,          // engine callbacks on a unit (Sim::CallMethod)
  kCallProjectile,    // on a projectile
  kCallWeapon,        // on a weapon
  kCallBrain,         // on an army's brain
  kCallEntity,        // on any other entity (props, blips, shields, beams)
  kCallOther,         // on any other script object (platoons, ...)
  kSimCallback,       // the command stream's Lua: LuaSimCallback (SimCallbacks.DoCallback, the UI-to-sim callbacks) and
                      // ExecuteLuaInSim
  kEngine,            // every other lua_pcall of the engine (global functions, scripts run by the engine)
  kSources
};
const char* SourceName(int s);

void InstallLuaTimer();  // sets the Lua hooks; the calling thread is the one measured (once per process)
void NextSource(int s);  // the source of the next outermost Lua call
double TakeLuaMs();      // outermost Lua time since the last call (for the phase laps)
double LuaMsTotal(int s);  // the whole run's Lua time of one source
void ResetLuaTotals();    // forget what was counted so far (the start-up's Lua)

double PeakWorkingSetMB();  // the process's peak working set so far (0 where unknown)

}  // namespace moho::prof
