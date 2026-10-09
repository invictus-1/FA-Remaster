// Building: structure placement and the tasks behind build-like commands (see build.cpp).
#pragma once
#include <memory>
#include <cstdint>
#include <string>

#include "sim/entity.h"
#include "sim/script_object.h"

namespace moho {

class Sim;
class Unit;
struct BlueprintInfo;
struct UnitCommand;
enum class CommandType : int;

enum : int { kTaskRunning = 0, kTaskDone = 1, kTaskFailed = -1 };

// The task carrying out a unit's head command when it is build-like (build, upgrade, repair,
// guard/assist). Owned by the sim; the unit points at it while it runs.
struct BuildTask : public ScriptObject {
  CommandType type{};
  int state = 0;
  std::string order;  // OnStartBuild's order: "MobileBuild", "FactoryBuild", "Upgrade", "Repair"
  const BlueprintInfo* bp = nullptr;
  Vec3 site;
  uint32_t goalId = 0;    // the command's target (repair / guard)
  uint32_t targetId = 0;  // the unit being worked on (focus)
  bool started = false;   // OnStartBuild ran for targetId
  bool ended = false;
  float lastFraction = 0;
  // reclaim: the fraction taken per tick and the resources it yields per tick
  float reclaimStep = 0, reclaimPerTick[2] = {0, 0};
  bool reclaimStarted = false;
  // script tasks: the Lua task object is this object's table
  bool scriptTask = false;
  uint32_t unitId = 0;
  uint32_t nextTick = 0;
  int count = 1, tries = 0;
  uint32_t waitUntil = 0;
  std::shared_ptr<struct TransportTaskData> tdata;  // transport load / unload tasks (sim/transport.cpp)
};

Vec3 SnapStructurePosition(Sim& sim, const BlueprintInfo& bp, Vec3 p);
bool LocationIsFree(Sim& sim, const BlueprintInfo& bp, float x, float z);
bool CanBuildStructureAt(Sim& sim, const BlueprintInfo& bp, float x, float z);
bool UnitCanBuild(const Unit* u, const BlueprintInfo& bp);
void OccupyStructure(Sim& sim, Unit* u);
void ReleaseStructure(Sim& sim, Unit* u);
// Adjacency of finished structures (OnAdjacentTo / OnNotAdjacentTo).
void AdjacencyGained(Sim& sim, lua_State* L, Unit* u);
void AdjacencyLost(Sim& sim, lua_State* L, Unit* u);

// nullptr: the command is not a build-like command this unit carries out with a task.
BuildTask* StartBuildTask(Sim& sim, Unit* u, const UnitCommand& c);
// One tick; kTaskRunning, kTaskDone or kTaskFailed (the task has ended then).
int TickBuildTask(Sim& sim, Unit* u, BuildTask& t);
// The command goes away (cleared, unit destroyed, ...).
void EndBuildTask(Sim& sim, Unit* u, BuildTask& t, bool success);
void StopMovingIfTask(Unit* u, const BuildTask& t);
// Movement helpers for command tasks (sim/combat.cpp).
bool TaskCanMove(const Unit* u);
void TaskMoveToward(Sim& sim, Unit* u, const Vec3& goal);
void TaskStopMoving(Unit* u);
void InheritFactoryCommands(Sim& sim, Unit* factory, Unit* built);

// Missile silos (CAiSiloBuildImpl): queued and automatic missile builds; a unit's beat.
void SiloTick(Sim& sim, Unit* u);
void SiloRelease(Unit* u);
bool SiloCommand(Sim& sim, Unit* u, const UnitCommand& c);  // true: handled (done at once)
void RegisterSiloBindings(lua_State* L);

void RegisterBuildBindings(lua_State* L);

}  // namespace moho
