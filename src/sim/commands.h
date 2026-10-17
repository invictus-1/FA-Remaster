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
struct Formation;

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
  int targetPoint = -1;  // CAiTarget bone pick of an entity target (index into AI.TargetBones; -1 none)
  // the order named the army's recon BLIP of an enemy unit (a player's order: the interface only knows blips),
  // not the unit itself (an order the engine issues from what a weapon found: [entity+0x68] at 0x5d9415)
  bool targetViaBlip = false;
  bool targetPastDeath = false;  // ... and that unit was dead already (AiTarget::pastDeath)
  uint32_t targetFakeId = 0;     // ... or the order names one of a jammer's FAKE blips (AiTarget::fakeId)
  uint32_t id = 0;
  CommandType type = CommandType::None;
  Vec3 pos;
  bool hasPos = false;
  uint32_t targetId = 0;  // entity target (0: none)
  std::string blueprintId;
  std::string formation;
  // formation issue data (SSTICommandIssueData +0x38..+0x4c): script index (-1 none), orientation
  // quaternion (w, x, y, z; all zero = none) and scale; the formation is made at dispatch
  int formIndex = -1;
  float formQw = 1, formQx = 0, formQy = 0, formQz = 0, formScale = 1;
  std::shared_ptr<Formation> form;
  int count = 1;
  int luaRef = -2;        // LUA_NOREF: the table handed to Lua (IsCommandDone)
  int scriptRef = -2;     // IssueScript: the command data table (TaskName, ...)
  std::set<Unit*> units;  // units that still have it queued
  uint32_t beaconRef = 0; // Ferry: the beacon created for it (CUnitCommand+0x158); dies with the command
  bool factoryIssued = false;  // cmd+0x142: a factory (rally) command or MoveOffFactory
  bool fromStream = false;     // issued by the command stream: id = its stream id (source << 24 | n)
  int maxCount = 1;            // cmd+0xe0
  std::vector<std::pair<int16_t, int16_t>> cells;  // cmd+0xc8 (SetCommandCells)
  // the target as the stream sent it (cmd+0x9c SSTITarget: type, raw entity id, point), for the "same order
  // again" test of ProcessUnitCommand
  int issueTargetType = 0;
  uint32_t issueTargetEntity = 0xf0000000u;
  Vec3 issueTargetPos;
};

// A target as the command stream sends it (SSTITarget): 0 none, 1 entity id, 2 position.
struct StreamTarget {
  int type = 0;
  uint32_t entity = 0xf0000000u;
  Vec3 pos;
};
// SSTICommandIssueData (engine-ref command_stream.md 3): what an IssueCommand message carries.
struct StreamIssueData {
  uint32_t id = 0xffffffffu;
  int32_t coordWith = -1;
  int type = 0;
  int32_t insertBefore = -1;
  StreamTarget target, target2;
  int formIndex = -1;
  float qw = 1, qx = 0, qy = 0, qz = 0, scale = 1;
  std::string blueprint;
  std::vector<std::pair<int16_t, int16_t>> cells;
  int maxCount = 1, count = 1;
  std::string lua;  // the serialized Lua value (nil = "\x02")
};
// UNIT_IssueCommand 0x6f12c0 for a command-stream order: one command for every unit that accepts it (units in
// ascending entity id), created at the first; clear empties each accepting unit's queue first. nullptr if no
// unit took it. (A TransportReverseLoadUnits goes to its target and the transport chosen for it instead:
// transport.h ReverseLoadIssueSet.)
std::shared_ptr<UnitCommand> IssueFromStream(Sim& sim, const std::vector<Unit*>& units, const StreamIssueData& d,
                                             bool clear);
// UNIT_IssueFactoryCommand 0x6f14d0 for a command-stream order: into the rally queues of the factories among
// the units (clear empties each first). nullptr if none took it.
std::shared_ptr<UnitCommand> IssueFactoryFromStream(Sim& sim, const std::vector<Unit*>& units,
                                                    const StreamIssueData& d, bool clear);
// The order a pod with an empty queue gives itself at the end of its motion tick (Unit::MotionTick 0x6a92b0):
// AssistCommander (29), no target, appended.
std::shared_ptr<UnitCommand> IssuePodAssist(Sim& sim, Unit* u);
// CUnitCommand::SetTarget from an SSTITarget (DecodeFromSSTITarget 0x5e2620: an entity target draws its
// target point); draw = false for SetCommandCells' ground target.
void SetCommandTargetFromStream(Sim& sim, UnitCommand& c, const StreamTarget& t);
// CUnitMoveTask::OnEvent 0x618c40: the running Move tasks of a retargeted command give their navigators the new goal
void RetargetMoveTasks(Sim& sim, UnitCommand& c);
// CUnitPatrolTask's listener 0x61c3e0 (and its reclaim child's 0x620160) for a retargeted patrol command
void RetargetPatrolTasks(Sim& sim, UnitCommand& c);
// CUnitGuardTask::OnEvent 0x6147b0: the guard tasks of a retargeted command take its target again (Refresh 0x611a40)
void RefreshGuardTasks(Sim& sim, UnitCommand& c);
// RemoveCommandFromQueue 0x6edf80: u drops c (the executing head ends its task). False if u did not hold it.
bool RemoveCommandFromUnit(Sim& sim, Unit* u, UnitCommand& c);
// What IssueClearCommands (0x6f1950) does to one unit: ClearCommandQueue 0x6ee2d0 and its attacker's Stop
// 0x5d56f0 (also CSquad::Stop 0x724820, for CPlatoon:Stop).
void IssueClearCommandsTo(Sim& sim, Unit* u);
// The Attack order a weapon with AutoInitiateAttackCommand gives its own idle unit (CAcquireTargetTask 0x5d9415).
std::shared_ptr<UnitCommand> IssueAutoAttack(Sim& sim, Unit* u, Entity* target, bool viaBlip = false,
                                             bool pastDeath = false, uint32_t fakeId = 0);

// Lua's view of a unit's navigator (Unit:GetNavigator()).
class NavigatorObject : public ScriptObject {
 public:
  NavigatorObject() { typeBits |= kTypeNavigator; }
  Unit* unit = nullptr;
  bool speedThroughGoal = false;
  bool ignoreFormation = false;
};

template <> struct ScriptTypeOf<NavigatorObject> { static constexpr uint32_t bit = kTypeNavigator; };

// The command stage (0x958): per unit in thread order, end an arrived move, then run the head command.
void CommandStage(Sim& sim);
// The patrol / attack-move task (CUnitPatrolTask, engine-ref attack_move.md 2).
struct BuildTask;
BuildTask* StartPatrolTask(Sim& sim, Unit* u, const UnitCommand& c);
int TickPatrol(Sim& sim, Unit* u, BuildTask& t);
void EndPatrol(Sim& sim, Unit* u, BuildTask& t);
// u no longer holds c (CUnitCommand::RemoveUnit; a Ferry command's beacon dies with its last holder).
void ReleaseCommand(Unit* u, UnitCommand& c);
// A unit is going away: drop its commands.
void ForgetUnitCommands(Unit* u);
// Whether the navigator's event (Succeeded / Failed) wakes the unit's command thread (a move task listens).
bool NavEventWakesCommandThread(const Unit* u);
void RegisterCommandBindings(lua_State* L);
// func_GetUnitList 0x6eee40, the unit list of every Issue* (IssueFactoryRallyPoint, IssueTransport*, ... included):
// a value that is no table is the error "Invalid unit set in <binding>; ...". i = 1 .. GetCount (ObjectCount, read
// once) through func_GetUnitOpt 0x593970: a nil hole or a value that is no game object is the error "Expected a game
// object...", another class "Incorrect type...", a destroyed unit is skipped, and so is a dead one (IUnit vt+0x28
// IsDead, u+0xa1; a destroy-queued one stays). The rest go into an EntitySet: ascending entity id, each once
// (EntitySetTemplate_Unit::Add 0x57ddd0). A single unit (a table without numeric keys) gives none.
std::vector<Unit*> ScriptUnitList(lua_State* L, int idx);
// The test of SCR_GetTarget 0x6eef60 (the target argument of IssueMove and its kin): nil is no target, a live entity
// (SCR_FromLuaNoError_Entity 0x5e3800) the entity, a table whose lua_getn (0x6ef011, ObjectCount) is 3 a position;
// anything else is the error "Invalid target set in <binding>; expected an entity or a Vec3 but got a <type>" (a
// destroyed unit's table and a position of 2 or 4 numbers included). The callers then read the target as before.
void CheckScriptTarget(lua_State* L, int idx);
// Lua's view of a command (the Issue* functions' result; nil for none).
void PushUnitCommand(lua_State* L, const std::shared_ptr<UnitCommand>& c);

}  // namespace moho
