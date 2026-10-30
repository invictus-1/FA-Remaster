// Transports: the transport object of units that can carry others (the original's
// CAiTransportImpl), attaching and detaching cargo, the load / unload command tasks of both the
// transport and its cargo, and the fall of dropped cargo.
//
// What the original does (FA exe, read 2026-10-09; specs engine-ref/specs/transport_core.md and
// transport_tasks.md):
// - A unit with the Transport command cap gets a transport object. Its attach points are the
//   skeleton's Attachpoint (class 1), Attachpoint_Med (2), Attachpoint_Lrg (3), Attachpoint_Spr
//   bones; a class-2/3 cargo occupies the Class2/Class3AttachSize nearest small points.
// - IssueTransportLoad puts one command in the queues of the cargo and the transport. The
//   transport waits until every cargo unit has the command current, assigns slots (heaviest
//   first, then nearest), flies to the mean cargo position and hovers at TransportHoverHeight;
//   each cargo unit walks under its attach point, "beams up" over 9 ticks and is attached.
//   A load gives up 300 ticks after the transport arrived.
// - IssueTransportUnload: the transport flies to the point, hovers, and detaches all its cargo in
//   one tick; dropped land units fall to the ground (ballistic) and continue their queues.
// - Cargo follows its attach bone every tick (after the transport moved) and takes the
//   transport's layer; a cargo unit runs no commands while attached.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "script/lua.hpp"
#include "sim/entity.h"

namespace moho {

class Sim;
class Unit;
struct BuildTask;
struct UnitCommand;
struct BlueprintInfo;

struct TransportAttachPoint {
  int bone = -1;
  Vec3 localPos;
};
struct TransportReservation {
  int transportBone = -1, cargoBone = -1;
  uint32_t cargo = 0;      // entity ref
  std::vector<int> bones;  // the small points it occupies
};

// CAiTransportImpl
struct TransportObj {
  Unit* owner = nullptr;
  bool isAirStaging = false, isTeleporter = false;
  int attachPointCount = 0;
  std::vector<TransportAttachPoint> generic, cls[4], special, launch;  // cls[0] = class 1
  std::vector<TransportReservation> reservations;
  // pickup info
  Vec3 pickupPos, pickupFacing;
  std::vector<uint32_t> pickup;  // entity refs, ascending id
  bool atPickup = false;
  // internal storage (carriers, carriers.md 6): T+0x20 reserve index, +0x24 launch index, +0x28 stagger
  int resIdx = 0, launchIdx = 0, stagger = 0, storageSlots = 0;
  std::vector<uint32_t> stored, storageReserved;  // entity refs
  // a teleporter (TELEPORTATION): T+0x10 the beacon it sends its cargo to (weak; entity ref), set by an unload
  // order on an entity (IssueTeleportToBeacon)
  uint32_t teleportBeacon = 0;
  // T+0x30 the units waiting for a later round of a teleporter's (or staging platform's) load (entity refs,
  // ascending id), and whether T+0x110, the waiting formation made for them, exists
  std::vector<uint32_t> waiting;
  bool waitingFormation = false;
};

// Unit creation: a transport object for units with the Transport command cap.
void TransportCreate(Sim& sim, Unit* u);
bool TransportHasCargo(const Unit* u);
// CAiTransportImpl vt+0x2c TransportAssignSlot 0x5e6e30 (requested bone, -1: the first free fitting one) and vt+0x18
// TransportRemoveUnitReservation 0x5e64d0 of a unit's transport object (false / nothing without one).
bool TransportAssignSlotFor(Sim& sim, Unit* transport, Unit* cargo, int requested);
void TransportRemoveReservationFor(Sim& sim, Unit* transport, const Unit* cargo);
// The factory hand-off: append TransportLoadUnits(T) to u if the issue path accepts it.
bool IssueTransportLoadOne(Sim& sim, Unit* u, Unit* T);
// TransportReverseLoadUnits (23, the interface's "selected transport, right click on a unit"; no Lua binding) at
// issue time. UNIT_IssueCommand 0x6f12c0 -> 0x6ef660: the units the order goes to become {the best transport of
// `units` for the target, the target} (ascending id; empty: nobody gets it). ProcessUnitCommand 0x6ef9e0, case 23
// (0x6f0f53): whether u of that set refuses it (it may call a transport's OnTransportReject).
std::vector<Unit*> ReverseLoadIssueSet(Sim& sim, const std::vector<Unit*>& units, Entity* target);
bool RefusesReverseLoad(Sim& sim, Unit* u, const std::vector<Unit*>& set, Entity* target);
// TransportLoadUnits (22) at issue time from the command stream: ProcessUnitCommand's case 0x6f0bf3 for u.
bool RefusesLoadOrder(Sim& sim, Unit* u, const Entity* target);
// A transport task (load, unload, call-transport, ...) whose innermost step is a pushed move task: the unit's command
// thread sleeps in that move task (TaskThreadAsleepOnMove).
bool TransportTaskAsleepOnMove(const BuildTask* t);
// CUnitRefuel 0x621490 on its own navigator goal (no move task of its own): the navigator's arrival wakes nobody.
bool RefuelGoalWakesNobody(const Unit* u);
// Children of the guard task: a ctor-B ferry (T guards a factory or a beacon) and a refuel at P.
BuildTask* MakeGuardFerryTask(Sim& sim, Unit* T, Unit* G);
BuildTask* MakeRefuelTask(Sim& sim, Unit* u, Unit* P);
// The task of an AssistCommander order: CUnitPodAssist, a drone or assist station (pod_assist.md).
BuildTask* StartPodTask(Sim& sim, Unit* u, const UnitCommand& c);
Unit* FindPlatform(Sim& sim, Unit* plane);
// Command caps of a unit's blueprint (RULEUCC_* bits).
uint32_t UnitCommandCaps(lua_State* L, const BlueprintInfo& bp);
// A General.CommandCaps value at idx as RULEUCC_* bits: a number, one cap's name or the mask in decimal (as
// bp_reflect leaves it), or an unconverted { RULEUCC_x = true } table.
uint32_t CommandCapsOfValue(lua_State* L, int idx);
// The unit's caps as they are now (scripts change them: Unit:AddCommandCap / RemoveCommandCap).
uint32_t LiveCommandCaps(lua_State* L, const Unit* u);

// Command dispatch (sim/build.cpp StartBuildTask/TickBuildTask/EndBuildTask call these).
// TransportLoadUnits (22) and Dock (39) share one case of the dispatch; TransportReverseLoadUnits (23), the unload
// types (24, 25) and Ferry (17).
BuildTask* StartTransportTask(Sim& sim, Unit* u, const UnitCommand& c);
// The transport orders DispatchTask carries out without a task: DetachFromTransport (26), an unload order a
// teleporter gets on an entity (its destination), a carrier's Dock on itself. true: done (the order is popped).
bool TransportOrderOnTheSpot(Sim& sim, Unit* u, const UnitCommand& c);
// CAiTransportImpl::TransportIsTeleportBeaconReady 0x5e81d0 of a teleporter (false: none, or not a transport).
bool TeleportBeaconReady(Sim& sim, const Unit* teleporter);
int TickTransportTask(Sim& sim, Unit* u, BuildTask& t);
void EndTransportTask(Sim& sim, Unit* u, BuildTask& t, bool success);
// The head task is a load (CUnitLoadUnits) whose landing move (its child CUnitMoveTask) is running.
bool TransportLoadMoving(const Unit* u);
// The head task is a load with no landing move of its own running (waiting for its cargo at the pickup point).
bool TransportLoadWaiting(const Unit* u);
// The head task is a cargo's call for a transport (CUnitCallTransport 0x5ffc70).
bool TransportCallTask(const Unit* u);

// Per tick after all units moved: attached units follow their parents (transport cargo gets the
// full attach transform and the parent's layer; orphaned cargo starts to fall).
void AttachedUnitsTick(Sim& sim);
// A script-attached unit whose parent no longer exists: free motion again (a flyer) or a fall, on the Air layer.
void AttachParentGone(Sim& sim, Unit* u);
// Unit:DetachFrom() without the skip flag, for a unit that cannot fly: it falls (NotifyDetached's ballistic branch).
// false: not that case.
bool DetachFromFalls(Sim& sim, Unit* u);
// An attached unit takes its parent's layer (with the script's OnLayerChange when it changes).
void SetAttachedUnitLayer(Sim& sim, Unit* u, const std::string& layer);
// Entity::CalculateAttachedTransform 0x67a020 for a unit attached by its own bone (Unit::ownBone) to a parent bone
// whose world transform is (parentPos, parentOri).
void AttachedTransform(Unit* u, const Vec3& parentPos, const Quat& parentOri, Vec3* pos, Quat* ori,
                       Vec3* ownPos = nullptr, Quat* ownOri = nullptr, const Vec3* givenOwnPos = nullptr,
                       const Quat* givenOwnOri = nullptr);
// Motion of a falling (detached) land unit: true while it falls.
bool LandBallisticTick(Sim& sim, Unit* u);
// Unit::Kill hooks: before SetDead (cargo leaves its transport) and after it (the dying unit's
// parent/children callbacks, a dying transport's cargo).
float TransportOnKillBegin(Sim& sim, lua_State* L, Unit* u, float ratio);
void TransportOnKillEnd(Sim& sim, lua_State* L, Unit* u);
// Unit::GetTransportFerryBeacon 0x6a8890: the ferry beacon of the unit's current command (nullptr: none)
Unit* UnitFerryBeacon(Sim& sim, const Unit* u);

void RegisterTransportBindings(lua_State* L);

// The command listener 0x618c40 of an unload's landing move (the move links the unload command): the command's
// target changed (SetCommandTarget), so the landing goes to the new place (COORDS_ToWorldPos, PrepareMove, SetGoal).
void RetargetUnloadMoves(Sim& sim, UnitCommand& c);

}  // namespace moho
