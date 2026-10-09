// Unit commands: the command queue every unit has, the Issue* functions that fill it, and the
// tasks that carry the commands out (the original's CUnitCommandQueue, CUnitCommand and
// CUnitMoveTask).
//
// What the original does (FA exe and the oracle probe, 2026-10-08):
// - Issue* appends one command, shared by all the units it was issued to, to each unit's queue
//   (IssueStop too: it is queued behind the current command). IssueClearCommands empties it.
// - The head command starts at the next tick (the unit enters the "Moving" state); a move then
//   waits for its path (land and naval units start driving 3 ticks later, a few searches finish
//   per tick) and ends when the unit reaches the goal cell. A queued move after it starts in the
//   same tick, so the unit drives through.
// - GetCommandQueue lists { commandType = EUnitCommandType, x, y, z, target, targetId, blueprintId }
//   (FAF's binary patch); commandType numbers are /lua/sim/commands/shared.lua's.
#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sim/entity.h"
#include "sim/script_object.h"

namespace moho {

class Sim;
class Unit;

enum class CommandType : int {
  None = 0, Stop = 1, Move = 2, Dive = 3, FormMove = 4, BuildSiloTactical = 5, BuildSiloNuke = 6,
  BuildFactory = 7, BuildMobile = 8, BuildAssist = 9, Attack = 10, FormAttack = 11, Nuke = 12,
  Tactical = 13, Teleport = 14, Guard = 15, Patrol = 16, Ferry = 17, FormPatrol = 18, Reclaim = 19,
  Repair = 20, Capture = 21, TransportLoadUnits = 22, TransportReverseLoadUnits = 23,
  TransportUnloadUnits = 24, TransportUnloadSpecificUnits = 25, DetachFromTransport = 26,
  Upgrade = 27, Script = 28, AssistCommander = 29, KillSelf = 30, DestroySelf = 31, Sacrifice = 32,
  Pause = 33, OverCharge = 34, AggressiveMove = 35, FormAggressiveMove = 36, AssistMove = 37,
  SpecialAction = 38, Dock = 39,
};

struct UnitCommand {
  uint32_t id = 0;
  CommandType type = CommandType::None;
  Vec3 pos;
  bool hasPos = false;
  uint32_t targetId = 0;  // entity target (0: none)
  std::string blueprintId;
  std::string formation;
  float heading = 0;
  // Formation moves: each unit's place (the formation's slot, centred on the goal and turned to
  // the heading) and the speed the formation keeps (its slowest unit's).
  std::map<Unit*, Vec3> slots;
  float formationSpeed = 0;
  int count = 1;
  int luaRef = -2;        // LUA_NOREF: the table handed to Lua (IsCommandDone)
  int scriptRef = -2;     // IssueScript: the command data table (TaskName, ...)
  std::set<Unit*> units;  // units that still have it queued
};

// Lua's view of a unit's navigator (Unit:GetNavigator()).
class NavigatorObject : public ScriptObject {
 public:
  NavigatorObject() { typeBits |= kTypeNavigator; }
  Unit* unit = nullptr;
  bool speedThroughGoal = false;
  bool ignoreFormation = false;
};

template <> struct ScriptTypeOf<NavigatorObject> { static constexpr uint32_t bit = kTypeNavigator; };

// Run the head command of every unit (before motion), and finish arrived moves (after motion).
void CommandsBeforeMotion(Sim& sim);
void CommandsAfterMotion(Sim& sim);
// A unit is going away: drop its commands.
void ForgetUnitCommands(Unit* u);
void RegisterCommandBindings(lua_State* L);
// Lua's view of a command (the Issue* functions' result; nil for none).
void PushUnitCommand(lua_State* L, const std::shared_ptr<UnitCommand>& c);

}  // namespace moho
