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
};

// Unit creation: a transport object for units with the Transport command cap.
void TransportCreate(Sim& sim, Unit* u);
bool TransportHasCargo(const Unit* u);
// Command caps of a unit's blueprint (RULEUCC_* bits).
uint32_t UnitCommandCaps(lua_State* L, const BlueprintInfo& bp);

// Command dispatch (sim/build.cpp StartBuildTask/TickBuildTask/EndBuildTask call these).
BuildTask* StartTransportTask(Sim& sim, Unit* u, const UnitCommand& c);
int TickTransportTask(Sim& sim, Unit* u, BuildTask& t);
void EndTransportTask(Sim& sim, Unit* u, BuildTask& t, bool success);

// Per tick after all units moved: attached units follow their parents (transport cargo gets the
// full attach transform and the parent's layer; orphaned cargo starts to fall).
void AttachedUnitsTick(Sim& sim);
// Motion of a falling (detached) land unit: true while it falls.
bool LandBallisticTick(Sim& sim, Unit* u);
// Unit::Kill hooks: before SetDead (cargo leaves its transport) and after it (the dying unit's
// parent/children callbacks, a dying transport's cargo).
float TransportOnKillBegin(Sim& sim, lua_State* L, Unit* u, float ratio);
void TransportOnKillEnd(Sim& sim, lua_State* L, Unit* u);

void RegisterTransportBindings(lua_State* L);

}  // namespace moho
