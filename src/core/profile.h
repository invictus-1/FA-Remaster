// MOHO64_PROFILE=1: the measuring tools of performance-lessons.md 4.1 (measure only). Everything here is off unless the
// variable is set, and none of it reads or writes simulation state: it only reads the clock.
//
// Lua time is taken at the Lua API's two entry points, lua_pcall and lua_resume (hooks in third_party/lua), and only at
// the outermost one on the thread that installed the timer: Lua's own pcall, coroutine.resume and a binding that calls
// back into Lua count inside their caller. So "Lua time" is the VM plus the engine functions the scripts call from
// there (a binding's C++ work counts as Lua), and everything else is the engine's.
//
// The beat breakdown, MOHO64_PROFILE_HITCH=<ms> (with MOHO64_PROFILE; docs/benchmarks.md B9): a beat that takes at
// least <ms> writes its breakdown to the log, as lines "profile hitch tick <N> ..." (sim.cpp PhaseTimer). It lists
// the beat's phases with their Lua, its Lua by source, its longest script calls by name (the outermost calls, and the
// engine's callbacks at any depth), its Lua collections, and the engine sections below. Like the rest, it only reads
// the clock and the engine's own counts.
//
// Two more for a hitch beat's Lua, only with MOHO64_PROFILE_HITCH. They set debug hooks on the sim's Lua state and on
// every script thread it makes. A hook makes Lua 5.0 grow a state's stack at a call (luaD_callhook), so the script
// state's memory, and with it the moments the collector runs, may differ from a run without them. They are for
// measuring, never for a gate or a recorded game:
//   MOHO64_PROFILE_LUASAMPLE=<n>  a count hook every n VM instructions: per function, the share of the samples it was
//                                 on the stack for (inclusive; time spent in C functions is not seen)
//   MOHO64_PROFILE_LUACALLS=1     call and return hooks: per function, Lua or C, the inclusive time of its calls that
//                                 returned in the beat (a coroutine's frame that yielded counts its wait too)
#pragma once

#include <cstdint>
#include <string>

struct lua_State;

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
void ResetLuaTotals();    // forget what was counted so far (the start-up's Lua, and the beat breakdown's counts)

double PeakWorkingSetMB();  // the process's peak working set so far (0 where unknown)

// ---- the beat breakdown (MOHO64_PROFILE_HITCH) ----
bool Hitch();      // MOHO64_PROFILE and MOHO64_PROFILE_HITCH are set (read once)
double HitchMs();  // its threshold in ms (0 when off)
double NowMs();    // the clock, in ms
int Depth();       // outermost Lua calls running now (0, or 1 inside one)
void NextLabel(const std::string& label);  // the name the next outermost Lua call is listed by
void AddCall(std::string label, double ms);  // a nested engine callback's time (a Lua call inside a Lua call)

// The engine's sections a hitch beat lists: the time and count of each (sections may hold others).
enum Section : int {
  kSecLuaGC,             // Lua 5.0's collections (luaC_collectgarbage; inside whatever ran a collector check)
  kSecCreateUnit,        // Sim::CreateUnit, all of it (its scripts included); the parts:
  kSecCuHeading,         //   the heading (Display.SpawnRandomRotation)
  kSecCuSkeleton,        //   AttachSkeleton
  kSecCuId,              //   ReserveId
  kSecCuShape,           //   RevertCollisionShape
  kSecCuEconomy,         //   UnitEconomyInit
  kSecCuMotion,          //   the motion blueprint
  kSecCuPlace,           //   the starting layer and the spawn height (it may make a path grid)
  kSecCuGrid,            //   the entity grid
  kSecCuAirTransport,    //   AirInit, TransportCreate
  kSecCuIntel,           //   CreateUnitIntel
  kSecCuFields,          //   health, fuel, cap cost
  kSecCuScript,          //   the script object, the lists, the occupancy and the callbacks (OnPreCreate, OnCreate, ...)
  kSecSkeletonRead,      // skeleton files read and parsed (SkeletonCache, the first of each mesh)
  kSecUnitBpData,        // a unit blueprint's data read (GetUnitBpData, the first time per blueprint)
  kSecPathGridMade,      // a path grid made for a new footprint (Navigation::Grid)
  kSecFlatHeights,       // FlattenMapRect: the heights
  kSecFlatDrawn,         //   the drawn ground's note (NoteHeightChange)
  kSecFlatAir,           //   AirTerrainChanged
  kSecFlatPaths,         //   every path grid's update (Navigation::TerrainChanged)
  kSecFlatUnits,         //   the units standing there
  kSecs
};
void AddSection(int s, double ms);
// Times one section while it lives, or a run of sections with To (closes the current one, opens the next; -1: none).
// Reads the clock only when Hitch().
class SectionTimer {
 public:
  explicit SectionTimer(int s);
  ~SectionTimer() { To(-1); }
  void To(int s);
  SectionTimer(const SectionTimer&) = delete;
  SectionTimer& operator=(const SectionTimer&) = delete;

 private:
  int s_ = -1;
  double t0_ = 0;
};

// The debug hooks on a sim Lua state or script thread (MOHO64_PROFILE_LUASAMPLE / _LUACALLS), and the collector's
// timer: only when Hitch(); the hooks only when their variable is set.
void InstallLuaHooks(lua_State* L);
// MOHO64_PROFILE_LUATOTAL=<file>: write the whole run's Lua by function (see profile.cpp)
void DumpLuaTotals();

// The beat's lines after its phases (sim.cpp), when beatMs is at least the threshold (*hitch); clears the beat's counts.
std::string TakeBeatReport(uint32_t tick, double beatMs, bool* hitch);

}  // namespace moho::prof
